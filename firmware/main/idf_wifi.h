#ifndef IDF_WIFI_H
#define IDF_WIFI_H
#include "app_config.h"   /* wifi_profiles_t, wifi_fast_join_* */
#include "ports.h"        /* port_kv_t */

/* Station-mode Wi-Fi join for the device side (design §5.5).
 *
 * Two join paths:
 *   - fast join: the BSSID + channel + SSID cached in NVS by the previous
 *     successful join are handed straight to esp_wifi_set_config() with
 *     .sta.bssid_set, skipping the scan (about a second faster).
 *   - scan join: an active all-channel scan, wifi_select_profile() (Task 4:
 *     profile order is the priority, not RSSI), then connect to the winner.
 * A failed fast join falls back to the scan path exactly once per
 * idf_wifi_wait_connected() call before giving up.
 *
 * NVS ordering: the kv port initializes NVS lazily and esp_wifi_init()
 * requires NVS to be up, so the cached-join lookup (which goes through kv)
 * deliberately runs *before* the Wi-Fi stack is initialized. Callers do not
 * need to pre-initialize NVS themselves.
 *
 * Single-caller/single-task by design (the main task): all state is static.
 */

int idf_wifi_connect(const wifi_profiles_t *p, port_kv_t *kv, unsigned timeout_ms);
    /* Blocking join: start_connect_async() + wait_connected(). 0 ok, -1 no network. */

int idf_wifi_start_connect_async(const wifi_profiles_t *p, port_kv_t *kv);
    /* Begins the join without blocking (Task 17 overlaps it with recording);
     * pair with idf_wifi_wait_connected(). 0 = join started, -1 = could not
     * even start one (no profiles, driver init failure, nothing in range). */

int idf_wifi_wait_connected(unsigned timeout_ms);
    /* Waits for an IP. 0 ok, -1 on timeout/association failure. On success
     * the BSSID, channel and SSID of the AP that answered are written back
     * to kv for the next boot's fast join. */

void idf_wifi_stop(void);
    /* Disconnects and stops the radio. The driver stays initialized, so a
     * later idf_wifi_start_connect_async() restarts without re-creating the
     * netif or the event loop. */

#endif
