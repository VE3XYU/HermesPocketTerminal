#include "harness.h"
#include "capture_flow.h"
#include "sidecar.h"
#include "rec_index.h"
#include "fakes/fake_transport.h"
#include "fakes/fake_storage.h"
#include <string.h>

static struct { unsigned now; unsigned slept[64]; int nslept; } CLK;
static long long clk_epoch(void *c) { (void)c; return 1785838502LL; }
static unsigned clk_mono(void *c) { (void)c; return CLK.now += 250; }
static void clk_sleep(void *c, unsigned ms) { (void)c; CLK.now += ms; CLK.slept[CLK.nslept++] = ms; }

#include "util.h"

static char status_log[16][64]; static int status_count;
static void on_status(void *u, const char *line) { (void)u; str_copy(status_log[status_count++], 64, line); }

static fake_transport_t ft; static htp_transport_t tr; static htp_client_t cl;
static fake_storage_t fs; static port_storage_t st;
static port_clock_t clock_port = { NULL, clk_epoch, clk_mono, clk_sleep };
static capture_ctx_t cx;
static sidecar_t sc;

static void fresh(const char *capture_id) {
    memset(&CLK, 0, sizeof CLK); status_count = 0;
    ft_init(&ft, &tr); htp_client_init(&cl, &tr, "tok"); cl.battery_pct = 50;
    fstore_init(&fs, &st);
    cx = (capture_ctx_t){ .client = &cl, .storage = &st, .clock = &clock_port,
        .on_status = on_status, .poll_interval_ms = 1000, .poll_window_ms = 60000,
        .reply_path = "/tmp/htp_flow_reply.wav" };
    sidecar_init(&sc, capture_id);
    sc.recorded_at = 1785838502LL;
    sidecar_save(&st, &sc);
    fstore_put(&fs, "/rec/dummy.wav", "RIFF");   /* wav presence not checked by flow */
}

static void test_note_done(void) {
    fresh("c-note");
    ft_push(&ft, 0, 200, "{\"id\":\"c-note\",\"state\":\"received\"}");
    ft_push(&ft, 0, 200, "{\"server_time\":1,\"captures\":[{\"id\":\"c-note\",\"state\":\"transcribing\"}]}");
    ft_push(&ft, 0, 200, "{\"server_time\":2,\"captures\":[{\"id\":\"c-note\",\"state\":\"done\","
                          "\"transcript\":\"Buy milk\"}]}");
    CHECK_EQ_INT(capture_run(&cx, &sc), CAPTURE_DONE);
    sidecar_t back; sidecar_load(&st, "c-note", &back);
    CHECK_EQ_STR(back.state, "done");
    CHECK_EQ_STR(back.transcript, "Buy milk");
    CHECK(back.uploaded_at > 0);
    CHECK_EQ_INT(CLK.slept[0], 1000);            /* polled at 1 Hz */
}

static void test_conversation_reply(void) {
    fresh("c-conv");
    ft_push(&ft, 0, 200, "{\"id\":\"c-conv\",\"state\":\"received\"}");
    ft_push(&ft, 0, 200, "{\"server_time\":1,\"captures\":[{\"id\":\"c-conv\",\"state\":\"reply_ready\","
                          "\"transcript\":\"Hey Hermes hi\",\"conversation_id\":\"v-1\"}]}");
    ft_push(&ft, 0, 200, "RIFF-reply-bytes");     /* reply.wav download */
    CHECK_EQ_INT(capture_run(&cx, &sc), CAPTURE_REPLY_READY);
    sidecar_t back; sidecar_load(&st, "c-conv", &back);
    CHECK_EQ_STR(back.state, "reply_ready");
    CHECK_EQ_STR(back.conversation_id, "v-1");
    CHECK(strstr(ft.req[2].path, "/htp/v1/captures/c-conv/reply.wav") != NULL);
}

static void test_offline_after_three_upload_attempts(void) {
    fresh("c-off");
    ft_push(&ft, 1, 0, ""); ft_push(&ft, 0, 500, "{\"error\":\"x\"}"); ft_push(&ft, 1, 0, "");
    CHECK_EQ_INT(capture_run(&cx, &sc), CAPTURE_OFFLINE);
    CHECK_EQ_INT(ft.req_count, 3);
    CHECK_EQ_INT(CLK.slept[0], 1000);            /* backoff between attempts: 1 s, 2 s */
    CHECK_EQ_INT(CLK.slept[1], 2000);
    sidecar_t back; sidecar_load(&st, "c-off", &back);
    CHECK_EQ_STR(back.state, "not_uploaded");
}

static void test_auth_error_stops_immediately(void) {
    fresh("c-auth");
    ft_push(&ft, 0, 401, "{\"error\":\"unauthorized\"}");
    CHECK_EQ_INT(capture_run(&cx, &sc), CAPTURE_AUTH_ERROR);
    CHECK_EQ_INT(ft.req_count, 1);               /* no retries on 401 */
}

static void test_poll_window_timeout(void) {
    fresh("c-slow");
    ft_push(&ft, 0, 200, "{\"id\":\"c-slow\",\"state\":\"received\"}");
    for (int i = 0; i < 80; i++)
        ft_push(&ft, 0, 200, "{\"server_time\":1,\"captures\":[{\"id\":\"c-slow\",\"state\":\"processing\"}]}");
    CHECK_EQ_INT(capture_run(&cx, &sc), CAPTURE_TIMEOUT);
    CHECK(ft.req_count < 70);                    /* window bounded the polls */
    sidecar_t back; sidecar_load(&st, "c-slow", &back);
    CHECK_EQ_STR(back.state, "uploaded");
}

static void test_failed_capture(void) {
    fresh("c-bad");
    ft_push(&ft, 0, 200, "{\"id\":\"c-bad\",\"state\":\"received\"}");
    ft_push(&ft, 0, 200, "{\"server_time\":1,\"captures\":[{\"id\":\"c-bad\",\"state\":\"failed\","
                          "\"error\":\"transcription_failed\"}]}");
    CHECK_EQ_INT(capture_run(&cx, &sc), CAPTURE_FAILED);
    sidecar_t back; sidecar_load(&st, "c-bad", &back);
    CHECK_EQ_STR(back.state, "failed");
    CHECK_EQ_STR(back.error, "transcription_failed");
}

static void test_retry_pending(void) {
    fresh("c-one");                               /* c-one: not_uploaded */
    sidecar_t two; sidecar_init(&two, "c-two");   /* c-two: already uploaded */
    str_copy(two.state, sizeof two.state, "uploaded");
    sidecar_save(&st, &two);
    sidecar_t three; sidecar_init(&three, "c-three");
    sidecar_save(&st, &three);
    rec_index_append(&st, "c-one"); rec_index_append(&st, "c-two"); rec_index_append(&st, "c-three");

    ft_push(&ft, 0, 200, "{\"id\":\"c-three\",\"state\":\"received\"}");  /* newest first */
    ft_push(&ft, 0, 200, "{\"id\":\"c-one\",\"state\":\"received\"}");
    CHECK_EQ_INT(capture_retry_pending(&cx), 2);
    CHECK_EQ_INT(ft.req_count, 2);                /* c-two untouched */
    sidecar_t back; sidecar_load(&st, "c-one", &back);
    CHECK_EQ_STR(back.state, "uploaded");
}

int main(void) {
    test_note_done();
    test_conversation_reply();
    test_offline_after_three_upload_attempts();
    test_auth_error_stops_immediately();
    test_poll_window_timeout();
    test_failed_capture();
    test_retry_pending();
    return HARNESS_REPORT();
}
