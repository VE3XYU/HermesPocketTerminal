#include "harness.h"
#include "app_config.h"
#include <string.h>

int main(void) {
    app_config_t c;
    const char *full =
        "{\"bridge_url\":\"http://192.0.2.10:8787\",\"token\":\"tok-abc\","
        "\"sync_interval_s\":300,\"silence_timeout_s\":8,\"log_level\":\"debug\","
        "\"future_field\":42}";
    CHECK_EQ_INT(app_config_parse(full, strlen(full), &c), 0);
    CHECK_EQ_STR(c.bridge_url, "http://192.0.2.10:8787");
    CHECK_EQ_STR(c.token, "tok-abc");
    CHECK_EQ_INT(c.sync_interval_s, 300);
    CHECK_EQ_INT(c.silence_timeout_s, 8);
    CHECK_EQ_STR(c.log_level, "debug");

    const char *minimal = "{\"bridge_url\":\"https://htp.example.net\",\"token\":\"t\"}";
    CHECK_EQ_INT(app_config_parse(minimal, strlen(minimal), &c), 0);
    CHECK_EQ_INT(c.sync_interval_s, 600);        /* defaults */
    CHECK_EQ_INT(c.silence_timeout_s, 0);
    CHECK_EQ_STR(c.log_level, "info");
    CHECK_EQ_STR(c.timezone, "");                /* absent -> empty -> UTC */

    /* timezone (C7 finding D): optional POSIX TZ string */
    const char *with_tz =
        "{\"bridge_url\":\"https://htp.example.net\",\"token\":\"t\","
        "\"timezone\":\"EST5EDT,M3.2.0,M11.1.0\"}";
    CHECK_EQ_INT(app_config_parse(with_tz, strlen(with_tz), &c), 0);
    CHECK_EQ_STR(c.timezone, "EST5EDT,M3.2.0,M11.1.0");

    /* overlong (>= 64 chars): ignored entirely -> UTC, never truncated (a
     * truncated TZ rule is a different rule, not an approximation); the
     * config as a whole still parses */
    {
        char overlong[256];
        char tz[80];
        memset(tz, 'X', sizeof tz - 1);
        tz[sizeof tz - 1] = '\0';
        snprintf(overlong, sizeof overlong,
                 "{\"bridge_url\":\"https://htp.example.net\",\"token\":\"t\","
                 "\"timezone\":\"%s\"}", tz);
        CHECK_EQ_INT(app_config_parse(overlong, strlen(overlong), &c), 0);
        CHECK_EQ_STR(c.timezone, "");
    }
    /* boundary: exactly 63 chars fits char[64] */
    {
        char overlong[256];
        char tz[64];
        memset(tz, 'Y', sizeof tz - 1);
        tz[sizeof tz - 1] = '\0';
        snprintf(overlong, sizeof overlong,
                 "{\"bridge_url\":\"https://htp.example.net\",\"token\":\"t\","
                 "\"timezone\":\"%s\"}", tz);
        CHECK_EQ_INT(app_config_parse(overlong, strlen(overlong), &c), 0);
        CHECK_EQ_STR(c.timezone, tz);
    }

    const char *no_token = "{\"bridge_url\":\"https://x\"}";
    CHECK_EQ_INT(app_config_parse(no_token, strlen(no_token), &c), -1);
    CHECK_EQ_INT(app_config_parse("not json", 8, &c), -1);

    wifi_profiles_t w;
    const char *nets =
        "{\"networks\":[{\"ssid\":\"home\",\"password\":\"pw1\"},"
        "{\"ssid\":\"office\",\"password\":\"pw2\","
        "\"static\":{\"ip\":\"192.0.2.20\",\"gateway\":\"192.0.2.1\",\"netmask\":\"255.255.255.0\"}}]}";
    CHECK_EQ_INT(wifi_profiles_parse(nets, strlen(nets), &w), 0);
    CHECK_EQ_INT(w.count, 2);
    CHECK_EQ_STR(w.nets[0].ssid, "home");
    CHECK_EQ_INT(w.nets[0].has_static, 0);
    CHECK_EQ_INT(w.nets[1].has_static, 1);
    CHECK_EQ_STR(w.nets[1].ip, "192.0.2.20");
    return HARNESS_REPORT();
}
