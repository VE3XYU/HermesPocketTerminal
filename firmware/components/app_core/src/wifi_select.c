#include "app_config.h"
#include <stdio.h>
#include <string.h>

int wifi_select_profile(const wifi_profiles_t *p, const wifi_scan_hit_t hits[], int nhits) {
    if (!p || !hits || nhits <= 0) return -1;

    /* Priority = profile order, not signal strength.
     * Find the first profile that appears in the scan results. */
    for (int i = 0; i < p->count; i++) {
        for (int j = 0; j < nhits; j++) {
            if (!strcmp(p->nets[i].ssid, hits[j].ssid)) {
                return i;
            }
        }
    }

    return -1;
}

void wifi_fast_join_store(port_kv_t *kv, const uint8_t bssid[6], uint8_t channel) {
    /* Write BSSID as 12 lowercase hex chars */
    char bssid_hex[13];
    snprintf(bssid_hex, sizeof bssid_hex, "%02x%02x%02x%02x%02x%02x",
             bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
    kv->set(kv->ctx, "bssid", bssid_hex);

    /* Write channel as decimal */
    char channel_str[4];
    snprintf(channel_str, sizeof channel_str, "%d", channel);
    kv->set(kv->ctx, "chan", channel_str);
}

void wifi_fast_join_load(port_kv_t *kv, wifi_fast_join_t *out) {
    memset(out, 0, sizeof *out);

    char bssid_hex[13];
    char channel_str[4];

    /* Try to read both keys */
    if (kv->get(kv->ctx, "bssid", bssid_hex, sizeof bssid_hex) != 0) {
        return;  /* valid remains 0 */
    }
    if (kv->get(kv->ctx, "chan", channel_str, sizeof channel_str) != 0) {
        return;  /* valid remains 0 */
    }

    /* Parse BSSID from 12 lowercase hex chars */
    if (sscanf(bssid_hex, "%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx",
               &out->bssid[0], &out->bssid[1], &out->bssid[2],
               &out->bssid[3], &out->bssid[4], &out->bssid[5]) != 6) {
        memset(out, 0, sizeof *out);
        return;  /* valid remains 0 */
    }

    /* Parse channel as decimal */
    unsigned char c;
    if (sscanf(channel_str, "%hhu", &c) != 1) {
        memset(out, 0, sizeof *out);
        return;  /* valid remains 0 */
    }
    out->channel = c;

    out->valid = 1;
}
