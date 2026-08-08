#include "app_config.h"
#include "util.h"
#include "cJSON.h"
#include <string.h>

static void get_str(cJSON *obj, const char *key, char *dst, size_t cap) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    str_copy(dst, cap, cJSON_IsString(v) ? v->valuestring : "");
}

static long long get_num(cJSON *obj, const char *key) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? (long long)v->valuedouble : 0;
}

int app_config_parse(const char *json, size_t len, app_config_t *out) {
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return -1;

    memset(out, 0, sizeof *out);

    /* Required fields */
    get_str(root, "bridge_url", out->bridge_url, sizeof out->bridge_url);
    get_str(root, "token", out->token, sizeof out->token);

    if (out->bridge_url[0] == '\0' || out->token[0] == '\0') {
        cJSON_Delete(root);
        return -1;
    }

    /* Optional fields with defaults */
    out->sync_interval_s = (int)get_num(root, "sync_interval_s");
    if (out->sync_interval_s == 0) out->sync_interval_s = 600;

    out->silence_timeout_s = (int)get_num(root, "silence_timeout_s");

    get_str(root, "log_level", out->log_level, sizeof out->log_level);
    if (out->log_level[0] == '\0') str_copy(out->log_level, sizeof out->log_level, "info");

    /* Optional POSIX TZ string (C7 finding D), e.g. "EST5EDT,M3.2.0,M11.1.0";
     * empty = UTC. An overlong value is ignored (stays UTC) rather than
     * truncated: a truncated TZ rule is a different rule, not an
     * approximation of the intended one. */
    cJSON *tz = cJSON_GetObjectItemCaseSensitive(root, "timezone");
    if (cJSON_IsString(tz) && strlen(tz->valuestring) < sizeof out->timezone)
        str_copy(out->timezone, sizeof out->timezone, tz->valuestring);

    cJSON_Delete(root);
    return 0;
}

int wifi_profiles_parse(const char *json, size_t len, wifi_profiles_t *out) {
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return -1;

    memset(out, 0, sizeof *out);

    cJSON *networks = cJSON_GetObjectItemCaseSensitive(root, "networks");
    if (!cJSON_IsArray(networks)) {
        cJSON_Delete(root);
        return -1;
    }

    out->count = 0;
    cJSON *net = NULL;
    cJSON_ArrayForEach(net, networks) {
        if (out->count >= 8) break;

        wifi_profile_t *p = &out->nets[out->count];
        memset(p, 0, sizeof *p);

        get_str(net, "ssid", p->ssid, sizeof p->ssid);
        get_str(net, "password", p->password, sizeof p->password);

        cJSON *static_obj = cJSON_GetObjectItemCaseSensitive(net, "static");
        if (cJSON_IsObject(static_obj)) {
            char ip[16] = {0}, gateway[16] = {0}, netmask[16] = {0};
            get_str(static_obj, "ip", ip, sizeof ip);
            get_str(static_obj, "gateway", gateway, sizeof gateway);
            get_str(static_obj, "netmask", netmask, sizeof netmask);

            /* has_static only if all three fields are present and non-empty */
            if (ip[0] != '\0' && gateway[0] != '\0' && netmask[0] != '\0') {
                p->has_static = 1;
                str_copy(p->ip, sizeof p->ip, ip);
                str_copy(p->gateway, sizeof p->gateway, gateway);
                str_copy(p->netmask, sizeof p->netmask, netmask);
            }
        }

        out->count++;
    }

    cJSON_Delete(root);
    return 0;
}
