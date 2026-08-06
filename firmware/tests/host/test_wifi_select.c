#include "harness.h"
#include "app_config.h"
#include "fakes/fake_kv.h"
#include <string.h>

int main(void) {
    wifi_profiles_t p = { .count = 3 };
    strcpy(p.nets[0].ssid, "first"); strcpy(p.nets[1].ssid, "second");
    strcpy(p.nets[2].ssid, "third");

    /* Priority = profile order, not signal strength */
    wifi_scan_hit_t hits[] = { { "third", -40 }, { "second", -80 } };
    CHECK_EQ_INT(wifi_select_profile(&p, hits, 2), 1);
    wifi_scan_hit_t only3[] = { { "third", -40 } };
    CHECK_EQ_INT(wifi_select_profile(&p, only3, 1), 2);
    wifi_scan_hit_t none[] = { { "stranger", -30 } };
    CHECK_EQ_INT(wifi_select_profile(&p, none, 1), -1);
    CHECK_EQ_INT(wifi_select_profile(&p, NULL, 0), -1);

    /* BSSID fast-join cache round-trip through kv */
    fake_kv_t fk; port_kv_t kv; fkv_init(&fk, &kv);
    wifi_fast_join_t fj;
    wifi_fast_join_load(&kv, &fj);
    CHECK_EQ_INT(fj.valid, 0);
    const uint8_t bssid[6] = { 0xa0, 0xb1, 0xc2, 0xd3, 0xe4, 0xf5 };
    wifi_fast_join_store(&kv, bssid, 6);
    wifi_fast_join_load(&kv, &fj);
    CHECK_EQ_INT(fj.valid, 1);
    CHECK_EQ_INT(fj.channel, 6);
    CHECK_EQ_INT(fj.bssid[0], 0xa0);
    CHECK_EQ_INT(fj.bssid[5], 0xf5);

    /* Malformed BSSID: 11 chars (truncated) + valid channel */
    fkv_init(&fk, &kv);
    kv.set(kv.ctx, "bssid", "a0b1c2d3e4f");  /* 11 chars, missing last digit */
    kv.set(kv.ctx, "chan", "6");
    wifi_fast_join_load(&kv, &fj);
    CHECK_EQ_INT(fj.valid, 0);

    /* Malformed channel: valid BSSID + trailing garbage */
    fkv_init(&fk, &kv);
    kv.set(kv.ctx, "bssid", "a0b1c2d3e4f5");  /* valid 12-char BSSID */
    kv.set(kv.ctx, "chan", "6x");             /* trailing garbage */
    wifi_fast_join_load(&kv, &fj);
    CHECK_EQ_INT(fj.valid, 0);

    /* Malformed BSSID: all non-hex chars + valid channel */
    fkv_init(&fk, &kv);
    kv.set(kv.ctx, "bssid", "zzzzzzzzzzzz");  /* 12 chars but not hex */
    kv.set(kv.ctx, "chan", "6");
    wifi_fast_join_load(&kv, &fj);
    CHECK_EQ_INT(fj.valid, 0);

    return HARNESS_REPORT();
}
