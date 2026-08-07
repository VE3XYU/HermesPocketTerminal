/* Station-mode Wi-Fi join (design §5.5). See idf_wifi.h for the contract. */
#include "idf_wifi.h"
#include "util.h"
#include "tick_ms.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "idf_wifi";

/* Task 4 owns the "bssid"/"chan" kv keys (wifi_fast_join_load/store). The
 * cached record deliberately stops there because those two values are all
 * the *design* cares about -- but esp_wifi_set_config() also needs an SSID
 * and a passphrase to select credentials with, and a BSSID alone does not
 * identify which /wifi.json profile it belonged to. This third key (same
 * NVS namespace, owned by this file, never read by app_core) closes that
 * gap: the SSID is looked up in the profile list to recover the password,
 * and a cache entry whose SSID is no longer in wifi.json is discarded
 * rather than joined blind. */
#define FJ_SSID_KEY "fj_ssid"

#define BIT_STARTED  BIT0
#define BIT_GOT_IP   BIT1
#define BIT_FAILED   BIT2

#define WIFI_START_TIMEOUT_MS 3000
#define SCAN_MAX_AP 24

static EventGroupHandle_t s_events;
static esp_netif_t *s_netif;
static port_kv_t *s_kv;

/* The caller's wifi_profiles_t is typically a stack local in app_main(),
 * but an async join outlives the call that started it (Task 17 starts the
 * join, records, then waits) -- so keep our own copy rather than a pointer
 * that can dangle. ~1.2 KB of BSS. */
static wifi_profiles_t s_profiles;

static int s_inited;        /* netif + event loop + esp_wifi_init() done */
static int s_started;       /* esp_wifi_start() done and STA_START seen */
static int s_fast_join;     /* the join in flight is the cached-BSSID path */
static int s_dhcpc_stopped; /* we stopped the DHCP client for a static-IP profile */

/* Scan scratch. Static, not stack: wifi_ap_record_t is ~80 bytes and the
 * main task's stack is shared with the whole harness. */
static wifi_ap_record_t s_recs[SCAN_MAX_AP];
static wifi_scan_hit_t s_hits[SCAN_MAX_AP];

/* Runs on the system event task, whose stack is small (a couple of KB) --
 * so this only logs and sets bits. Everything expensive (NVS writes for the
 * fast-join cache) happens in the main task once it observes BIT_GOT_IP. */
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        xEventGroupSetBits(s_events, BIT_STARTED);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *e = data;
        ESP_LOGW(TAG, "disconnected (reason %d)", e ? e->reason : -1);
        xEventGroupSetBits(s_events, BIT_FAILED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_events, BIT_GOT_IP);
    }
}

static int ensure_init(void) {
    if (s_inited) return 0;

    s_events = xEventGroupCreate();
    if (!s_events) return -1;

    if (esp_netif_init() != ESP_OK) return -1;
    /* Already created by an earlier component is fine, anything else is not. */
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(e));
        return -1;
    }
    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) return -1;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if ((e = esp_wifi_init(&cfg)) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(e));
        return -1;
    }
    if (esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL) != ESP_OK ||
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL) != ESP_OK)
        return -1;

    /* Credentials live on the SD card, not in NVS: keeping the driver's
     * copy in RAM avoids a second, stale source of truth. */
    if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK) return -1;
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) return -1;

    s_inited = 1;
    return 0;
}

/* esp_wifi_start() is asynchronous; esp_wifi_connect()/esp_wifi_scan_start()
 * both fail with ESP_ERR_WIFI_NOT_STARTED until STA_START lands. Wait for it
 * once here so every caller below can assume a started radio. */
static int ensure_started(void) {
    if (s_started) return 0;
    xEventGroupClearBits(s_events, BIT_STARTED);
    esp_err_t e = esp_wifi_start();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(e));
        return -1;
    }
    EventBits_t b = xEventGroupWaitBits(s_events, BIT_STARTED, pdFALSE, pdFALSE,
                                        ms_to_ticks_min1(WIFI_START_TIMEOUT_MS));
    if (!(b & BIT_STARTED)) {
        ESP_LOGE(TAG, "radio did not start within %d ms", WIFI_START_TIMEOUT_MS);
        return -1;
    }
    s_started = 1;
    return 0;
}

static int profile_index_by_ssid(const char *ssid) {
    for (int i = 0; i < s_profiles.count; i++)
        if (!strcmp(s_profiles.nets[i].ssid, ssid)) return i;
    return -1;
}

/* Static addressing when the profile carries one, DHCP otherwise. Either
 * way the join completes on IP_EVENT_STA_GOT_IP: esp_netif posts that event
 * for a valid static address too, once the association comes up. */
static void apply_ip_config(const wifi_profile_t *p) {
    if (p->has_static) {
        esp_netif_ip_info_t ip = {0};
        if (esp_netif_str_to_ip4(p->ip, &ip.ip) != ESP_OK ||
            esp_netif_str_to_ip4(p->gateway, &ip.gw) != ESP_OK ||
            esp_netif_str_to_ip4(p->netmask, &ip.netmask) != ESP_OK) {
            ESP_LOGE(TAG, "profile static IP is malformed; falling back to DHCP");
            return;
        }
        esp_netif_dhcpc_stop(s_netif);
        s_dhcpc_stopped = 1;
        if (esp_netif_set_ip_info(s_netif, &ip) != ESP_OK)
            ESP_LOGE(TAG, "esp_netif_set_ip_info failed");
        else
            ESP_LOGI(TAG, "static IP " IPSTR, IP2STR(&ip.ip));
    } else if (s_dhcpc_stopped) {
        /* Only reachable when an earlier join in this same session used a
         * static profile: esp_netif_action_connected() takes the static
         * branch whenever the DHCP client is in the STOPPED state, so it
         * has to be handed back before a DHCP profile can work. */
        esp_netif_dhcpc_start(s_netif);
        s_dhcpc_stopped = 0;
    }
}

/* esp_wifi's ssid[32]/password[64] are length-capped byte fields, not C
 * strings: a 32-character SSID (or a 64-hex-character raw PSK) legitimately
 * fills the field with no room for a terminator, so str_copy() -- which
 * always reserves one -- would silently drop the last character. The
 * surrounding wifi_config_t is zeroed, so shorter values still end up
 * NUL-terminated. */
static void copy_capped(uint8_t *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n > cap) n = cap;
    memcpy(dst, src, n);
}

/* bssid == NULL: plain SSID join (scan path). bssid != NULL: pinned fast join. */
static int connect_to_profile(int idx, const uint8_t *bssid, uint8_t channel) {
    const wifi_profile_t *p = &s_profiles.nets[idx];
    wifi_config_t wc = {0};
    copy_capped(wc.sta.ssid, sizeof wc.sta.ssid, p->ssid);
    copy_capped(wc.sta.password, sizeof wc.sta.password, p->password);
    if (bssid) {
        memcpy(wc.sta.bssid, bssid, 6);
        wc.sta.bssid_set = true;
        wc.sta.channel = channel;
    }
    /* threshold.authmode stays WIFI_AUTH_OPEN (0): accept whatever security
     * the AP actually offers rather than filtering on a guess. */

    esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config: %s", esp_err_to_name(e));
        return -1;
    }
    apply_ip_config(p);

    xEventGroupClearBits(s_events, BIT_GOT_IP | BIT_FAILED);
    if ((e = esp_wifi_connect()) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect: %s", esp_err_to_name(e));
        return -1;
    }
    return 0;
}

static int join_by_scan(void) {
    esp_wifi_disconnect();   /* a scan is refused while a join is in flight */

    wifi_scan_config_t sc = {0};   /* active scan, all channels, any SSID */
    esp_err_t e = esp_wifi_scan_start(&sc, true);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "scan: %s", esp_err_to_name(e));
        return -1;
    }
    uint16_t n = SCAN_MAX_AP;
    if ((e = esp_wifi_scan_get_ap_records(&n, s_recs)) != ESP_OK) {
        ESP_LOGE(TAG, "scan records: %s", esp_err_to_name(e));
        esp_wifi_clear_ap_list();
        return -1;
    }
    if (n > SCAN_MAX_AP) n = SCAN_MAX_AP;
    for (uint16_t i = 0; i < n; i++) {
        str_copy(s_hits[i].ssid, sizeof s_hits[i].ssid, (const char *)s_recs[i].ssid);
        s_hits[i].rssi = s_recs[i].rssi;
    }

    int idx = wifi_select_profile(&s_profiles, s_hits, (int)n);
    if (idx < 0) {
        ESP_LOGE(TAG, "no configured network among %u APs in range", (unsigned)n);
        return -1;
    }
    /* wifi_select_profile() returns a *profile* index; report the strongest
     * scan hit carrying that SSID (0 = no reading, RSSI is always < 0). */
    int rssi = 0;
    for (uint16_t i = 0; i < n; i++)
        if (!strcmp(s_hits[i].ssid, s_profiles.nets[idx].ssid) && (rssi == 0 || s_hits[i].rssi > rssi))
            rssi = s_hits[i].rssi;
    ESP_LOGI(TAG, "scan join: %s (rssi %d, %u APs seen)", s_profiles.nets[idx].ssid, rssi, (unsigned)n);
    return connect_to_profile(idx, NULL, 0);
}

/* Waits for the outcome of the join currently in flight. 0 = got an IP. */
static int wait_outcome(unsigned timeout_ms) {
    EventBits_t b = xEventGroupWaitBits(s_events, BIT_GOT_IP | BIT_FAILED,
                                        pdFALSE, pdFALSE, ms_to_ticks_min1(timeout_ms));
    return (b & BIT_GOT_IP) ? 0 : -1;
}

/* Main-task side of IP_EVENT_STA_GOT_IP: refresh the fast-join cache (an
 * NVS write, too stack-hungry for the event task) and log the address. */
static void on_connected(void) {
    esp_netif_ip_info_t ip = {0};
    if (esp_netif_get_ip_info(s_netif, &ip) == ESP_OK)
        ESP_LOGI(TAG, "connected, ip " IPSTR, IP2STR(&ip.ip));

    wifi_ap_record_t ap;
    if (!s_kv || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return;
    wifi_fast_join_store(s_kv, ap.bssid, ap.primary);
    char ssid[33];
    str_copy(ssid, sizeof ssid, (const char *)ap.ssid);
    s_kv->set(s_kv->ctx, FJ_SSID_KEY, ssid);
    ESP_LOGI(TAG, "fast-join cache: ssid=%s bssid=%02x%02x%02x%02x%02x%02x chan=%u", ssid,
             ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5],
             (unsigned)ap.primary);
}

int idf_wifi_start_connect_async(const wifi_profiles_t *p, port_kv_t *kv) {
    if (!p || p->count <= 0 || !kv) return -1;
    s_profiles = *p;
    s_kv = kv;

    /* Before ensure_init(): this is what brings NVS up (the kv port
     * initializes it lazily) and esp_wifi_init() needs it there already. */
    wifi_fast_join_t fj;
    wifi_fast_join_load(kv, &fj);
    char ssid[33] = "";
    int idx = -1;
    if (fj.valid && kv->get(kv->ctx, FJ_SSID_KEY, ssid, sizeof ssid) == 0)
        idx = profile_index_by_ssid(ssid);

    if (ensure_init() != 0) return -1;
    if (ensure_started() != 0) return -1;

    if (idx >= 0) {
        s_fast_join = 1;
        ESP_LOGI(TAG, "fast join: %s on channel %u (cached bssid)", ssid, (unsigned)fj.channel);
        if (connect_to_profile(idx, fj.bssid, fj.channel) == 0) return 0;
        ESP_LOGW(TAG, "fast join could not be started; scanning");
    }
    s_fast_join = 0;
    return join_by_scan();
}

int idf_wifi_wait_connected(unsigned timeout_ms) {
    if (!s_inited) return -1;

    if (wait_outcome(timeout_ms) == 0) { on_connected(); return 0; }

    if (s_fast_join) {
        /* design §5.5: a stale cached BSSID (AP moved channel, roamed, or
         * is simply out of range) costs one failed attempt, not the boot. */
        ESP_LOGW(TAG, "fast join failed; falling back to a full scan");
        s_fast_join = 0;
        if (join_by_scan() != 0) return -1;
        if (wait_outcome(timeout_ms) == 0) { on_connected(); return 0; }
    }
    ESP_LOGE(TAG, "no network");
    return -1;
}

int idf_wifi_connect(const wifi_profiles_t *p, port_kv_t *kv, unsigned timeout_ms) {
    if (idf_wifi_start_connect_async(p, kv) != 0) return -1;
    return idf_wifi_wait_connected(timeout_ms);
}

void idf_wifi_stop(void) {
    if (!s_inited) return;
    esp_wifi_disconnect();
    if (s_started) {
        esp_wifi_stop();
        s_started = 0;
    }
    xEventGroupClearBits(s_events, BIT_STARTED | BIT_GOT_IP | BIT_FAILED);
}
