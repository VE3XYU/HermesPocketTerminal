#ifndef APP_CONFIG_H
#define APP_CONFIG_H
#include <stdint.h>
#include <stddef.h>
#include "ports.h"

typedef struct {
    char bridge_url[128];        /* e.g. "https://htp.example.net" or "http://192.0.2.10:8787" */
    char token[128];
    int  sync_interval_s;        /* default 600; superseded at runtime by dashboard sync_interval */
    int  silence_timeout_s;      /* 0 = disabled (default) */
    char log_level[8];           /* "error"|"warn"|"info"|"debug", default "info" */
} app_config_t;
int app_config_parse(const char *json, size_t len, app_config_t *out); /* -1 when url/token missing */

typedef struct {
    char ssid[33]; char password[65];
    int has_static; char ip[16], gateway[16], netmask[16];
} wifi_profile_t;
typedef struct { wifi_profile_t nets[8]; int count; } wifi_profiles_t;
int wifi_profiles_parse(const char *json, size_t len, wifi_profiles_t *out);

/* wifi_select.c */
typedef struct { char ssid[33]; int rssi; } wifi_scan_hit_t;
int wifi_select_profile(const wifi_profiles_t *p, const wifi_scan_hit_t hits[], int nhits);
    /* index of highest-priority profile present in scan, or -1 */
typedef struct { uint8_t bssid[6]; uint8_t channel; int valid; } wifi_fast_join_t;
void wifi_fast_join_load(port_kv_t *kv, wifi_fast_join_t *out);   /* kv keys "bssid" hex12, "chan" */
void wifi_fast_join_store(port_kv_t *kv, const uint8_t bssid[6], uint8_t channel);

#endif
