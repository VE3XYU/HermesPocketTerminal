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
