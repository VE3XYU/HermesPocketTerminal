#include "harness.h"
#include "htp_client.h"
#include "fakes/fake_transport.h"
#include <string.h>
#include <stdio.h>

static fake_transport_t ft;
static htp_transport_t tr;
static htp_client_t cl;

static void fresh(void) { ft_init(&ft, &tr); htp_client_init(&cl, &tr, "tok-abc"); cl.battery_pct = 78; }

static void test_upload_request_shape(void) {
    fresh();
    ft_push_fixture(&ft, 200, "captures_post.json");
    htp_upload_params_t p = { .capture_id = "c-20260804-101502-3fa9",
        .wav_path = "/rec/c-20260804-101502-3fa9.wav", .recorded_at = 1785838502LL,
        .conversation_id = NULL };
    CHECK_EQ_INT(htp_upload_capture(&cl, &p), HTP_OK);
    CHECK_EQ_STR(ft.req[0].method, "POST");
    CHECK_EQ_STR(ft.req[0].path, "/htp/v1/captures");
    CHECK_EQ_STR(ft.req[0].content_type, "audio/wav");
    CHECK_EQ_STR(ft.req[0].body_file, "/rec/c-20260804-101502-3fa9.wav");
    CHECK(ft.req[0].timeout_ms > 0);
    CHECK_EQ_STR(ft_find_header(&ft, 0, "Authorization"), "Bearer tok-abc");
    CHECK_EQ_STR(ft_find_header(&ft, 0, "X-Capture-Id"), "c-20260804-101502-3fa9");
    CHECK_EQ_STR(ft_find_header(&ft, 0, "X-Capture-Mode"), "auto");
    CHECK_EQ_STR(ft_find_header(&ft, 0, "X-Recorded-At"), "1785838502");
    CHECK_EQ_STR(ft_find_header(&ft, 0, "X-Battery"), "78");
    CHECK(ft_find_header(&ft, 0, "X-Conversation-Id") == NULL);
}

static void test_upload_omits_recorded_at_when_clockless(void) {
    fresh();
    ft_push(&ft, 0, 200, "{\"id\":\"c-b17-4523-3fa9\",\"state\":\"received\"}");
    htp_upload_params_t p = { .capture_id = "c-b17-4523-3fa9",
        .wav_path = "/rec/c-b17-4523-3fa9.wav", .recorded_at = 0,
        .conversation_id = "v-4b81" };
    CHECK_EQ_INT(htp_upload_capture(&cl, &p), HTP_OK);
    CHECK(ft_find_header(&ft, 0, "X-Recorded-At") == NULL);
    CHECK_EQ_STR(ft_find_header(&ft, 0, "X-Conversation-Id"), "v-4b81");
}

static void test_upload_id_mismatch_is_proto_error(void) {
    fresh();
    ft_push(&ft, 0, 200, "{\"id\":\"c-other\",\"state\":\"received\"}");
    htp_upload_params_t p = { .capture_id = "c-mine", .wav_path = "/x.wav" };
    CHECK_EQ_INT(htp_upload_capture(&cl, &p), HTP_ERR_PROTO);
}

static void test_poll_parses_fixture(void) {
    fresh();
    ft_push_fixture(&ft, 200, "captures_get.json");
    const char *ids[] = { "c-20260804-101502-3fa9", "c-missing" };
    htp_capture_status_t st[4]; long long server_time = 0;
    int n = htp_poll_captures(&cl, ids, 2, st, 4, &server_time);
    CHECK_EQ_INT(n, 2);
    CHECK_EQ_INT(server_time, 1000000);
    CHECK(strstr(ft.req[0].path, "/htp/v1/captures?ids=c-20260804-101502-3fa9,c-missing") != NULL);
    CHECK_EQ_STR(st[0].id, "c-20260804-101502-3fa9");
    CHECK_EQ_INT(st[0].state, HTP_ST_DONE);
    CHECK_EQ_STR(st[0].transcript, "Add milk to the shopping list");
    CHECK_EQ_INT(st[1].state, HTP_ST_UNKNOWN);
}

static void test_poll_reply_ready_and_failed(void) {
    fresh();
    ft_push(&ft, 0, 200,
        "{\"server_time\":1000005,\"captures\":["
        "{\"id\":\"c-a\",\"state\":\"reply_ready\",\"transcript\":\"Hey\",\"conversation_id\":\"v-mock\"},"
        "{\"id\":\"c-b\",\"state\":\"failed\",\"error\":\"transcription_failed\"},"
        "{\"id\":\"c-c\",\"state\":\"someday_new_state\",\"novel_field\":true}]}");
    const char *ids[] = { "c-a", "c-b", "c-c" };
    htp_capture_status_t st[4]; long long t;
    CHECK_EQ_INT(htp_poll_captures(&cl, ids, 3, st, 4, &t), 3);
    CHECK_EQ_INT(st[0].state, HTP_ST_REPLY_READY);
    CHECK_EQ_STR(st[0].conversation_id, "v-mock");
    CHECK_EQ_INT(st[1].state, HTP_ST_FAILED);
    CHECK_EQ_STR(st[1].error, "transcription_failed");
    CHECK_EQ_INT(st[2].state, HTP_ST_UNKNOWN);   /* unknown state string tolerated */
}

static void test_dashboard_fixture(void) {
    fresh();
    ft_push_fixture(&ft, 200, "dashboard_get.json");
    htp_dashboard_t d;
    CHECK_EQ_INT(htp_get_dashboard(&cl, "7c1a", &d), HTP_OK);
    CHECK(strstr(ft.req[0].path, "/htp/v1/dashboard?rev=7c1a") != NULL);
    CHECK_EQ_INT(d.unchanged, 0);
    CHECK_EQ_STR(d.rev, "44818d09");
    CHECK_EQ_STR(d.title, "Today");
    CHECK_EQ_INT(d.item_count, 2);
    CHECK_EQ_STR(d.items[0].id, "t-9f2");
    CHECK_EQ_STR(d.items[0].text, "Buy milk");
    CHECK_EQ_INT(d.items[0].done, 0);
    CHECK_EQ_INT(d.items[1].done, 1);
    CHECK_EQ_STR(d.items[1].style, "dim");
    CHECK_EQ_INT(d.sync_interval, 600);
    CHECK_EQ_INT(d.server_time, 1000000);
}

static void test_dashboard_unchanged_fixture(void) {
    fresh();
    ft_push_fixture(&ft, 200, "dashboard_unchanged.json");
    htp_dashboard_t d;
    CHECK_EQ_INT(htp_get_dashboard(&cl, "1f6aab9c", &d), HTP_OK);
    CHECK_EQ_INT(d.unchanged, 1);
    CHECK_EQ_STR(d.rev, "1f6aab9c");
    CHECK_EQ_INT(d.sync_interval, 600);
    /* empty rev omits the query parameter */
    fresh();
    ft_push_fixture(&ft, 200, "dashboard_get.json");
    CHECK_EQ_INT(htp_get_dashboard(&cl, "", &d), HTP_OK);
    CHECK_EQ_STR(ft.req[0].path, "/htp/v1/dashboard");
}

static void test_notifications_and_ack(void) {
    fresh();
    ft_push_fixture(&ft, 200, "notifications_get.json");
    ft_push_fixture(&ft, 200, "ack_post.json");
    htp_notifications_t nn;
    CHECK_EQ_INT(htp_get_notifications(&cl, &nn), HTP_OK);
    CHECK_EQ_INT(nn.count, 1);
    CHECK_EQ_STR(nn.items[0].id, "n-1");
    CHECK_EQ_INT(nn.items[0].urgent, 1);
    CHECK_EQ_INT(nn.items[0].created, 1000000);
    const char *ids[] = { "n-1" };
    CHECK_EQ_INT(htp_ack_notifications(&cl, ids, 1), HTP_OK);
    CHECK_EQ_STR(ft.req[1].method, "POST");
    CHECK_EQ_STR(ft.req[1].path, "/htp/v1/notifications/ack");
    CHECK_EQ_STR(ft.req[1].body, "{\"ids\":[\"n-1\"]}");
    CHECK_EQ_STR(ft.req[1].content_type, "application/json");
}

static void test_complete(void) {
    fresh();
    ft_push_fixture(&ft, 200, "complete_post.json");
    char rev[24];
    CHECK_EQ_INT(htp_complete_item(&cl, "t-9f2", rev), HTP_OK);
    CHECK_EQ_STR(rev, "62d2e25e");
    CHECK_EQ_STR(ft.req[0].body, "{\"item_id\":\"t-9f2\"}");
}

static void test_download_reply(void) {
    fresh();
    ft_push(&ft, 0, 200, "RIFFxxxxWAVE");    /* content passthrough, not parsed here */
    CHECK_EQ_INT(htp_download_reply(&cl, "c-a", "/tmp/htp_test_reply.wav"), HTP_OK);
    CHECK_EQ_STR(ft.req[0].sink_file, "/tmp/htp_test_reply.wav");
    CHECK(strstr(ft.req[0].path, "/htp/v1/captures/c-a/reply.wav") != NULL);
}

static void test_error_mapping(void) {
    fresh();
    ft_push_fixture(&ft, 401, "error_unauthorized.json");
    ft_push_fixture(&ft, 400, "error_invalid_capture_id.json");
    ft_push(&ft, 0, 500, "{\"error\":\"speech_provider_error\"}");
    ft_push(&ft, 1, 0, "");                       /* network failure */
    ft_push_fixture(&ft, 404, "error_reply_not_found.json");
    ft_push(&ft, 0, 200, "this is not json");
    htp_dashboard_t d;
    CHECK_EQ_INT(htp_get_dashboard(&cl, "", &d), HTP_ERR_AUTH);
    CHECK_EQ_INT(htp_get_dashboard(&cl, "", &d), HTP_ERR_CLIENT);
    CHECK_EQ_INT(htp_get_dashboard(&cl, "", &d), HTP_ERR_SERVER);
    CHECK_EQ_INT(htp_get_dashboard(&cl, "", &d), HTP_ERR_NETWORK);
    CHECK_EQ_INT(htp_download_reply(&cl, "c-x", "/tmp/htp_test_reply.wav"), HTP_ERR_CLIENT);
    CHECK_EQ_INT(htp_get_dashboard(&cl, "", &d), HTP_ERR_PROTO);
}

static void fill(char *buf, size_t len, char c) {
    memset(buf, c, len);
    buf[len] = 0;
}

/* Oversized request bodies/paths must never reach the transport: the client
 * has to detect the overflow itself and fail closed with HTP_ERR_CLIENT
 * before calling perform(). ft.req_count staying at 0 is the proof that no
 * request was sent (fake_transport captures every request it receives,
 * regardless of whether a scripted response is available). */

static void test_ack_rejects_oversized_batch(void) {
    fresh();
    char id24[25];
    fill(id24, 24, 'a');
    const char *ids[60];
    for (int i = 0; i < 60; i++) ids[i] = id24;
    CHECK_EQ_INT(htp_ack_notifications(&cl, ids, 60), HTP_ERR_CLIENT);
    CHECK_EQ_INT(ft.req_count, 0);
}

static void test_complete_rejects_oversized_item_id(void) {
    fresh();
    char big_id[200];
    fill(big_id, 199, 'x');
    char rev[24];
    CHECK_EQ_INT(htp_complete_item(&cl, big_id, rev), HTP_ERR_CLIENT);
    CHECK_EQ_INT(ft.req_count, 0);
}

static void test_poll_rejects_oversized_batch(void) {
    fresh();
    char id63[64];
    fill(id63, 63, 'b');
    const char *ids[40];
    for (int i = 0; i < 40; i++) ids[i] = id63;
    htp_capture_status_t st[4]; long long t;
    CHECK_EQ_INT(htp_poll_captures(&cl, ids, 40, st, 4, &t), HTP_ERR_CLIENT);
    CHECK_EQ_INT(ft.req_count, 0);
}

int main(void) {
    test_upload_request_shape();
    test_upload_omits_recorded_at_when_clockless();
    test_upload_id_mismatch_is_proto_error();
    test_poll_parses_fixture();
    test_poll_reply_ready_and_failed();
    test_dashboard_fixture();
    test_dashboard_unchanged_fixture();
    test_notifications_and_ack();
    test_complete();
    test_download_reply();
    test_error_mapping();
    test_ack_rejects_oversized_batch();
    test_complete_rejects_oversized_item_id();
    test_poll_rejects_oversized_batch();
    return HARNESS_REPORT();
}
