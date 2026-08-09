#include "harness.h"
#include "capture_flow.h"
#include "sidecar.h"
#include "rec_index.h"
#include "wav.h"
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

/* Design §10 torn-file rule (Task 19): power lost mid-recording leaves the
 * placeholder all-zero header (patched only on clean stop) -- upload must
 * REFUSE it, not stream it to the bridge, and must not keep refusing it on
 * every future sync. */
static void test_torn_wav_refused(void) {
    fresh("c-torn");
    uint8_t zeroed[44];
    memset(zeroed, 0, sizeof zeroed);
    CHECK_EQ_INT(st.write(st.ctx, "/rec/c-torn.wav", zeroed, sizeof zeroed), 0);
    rec_index_append(&st, "c-torn");

    CHECK_EQ_INT(capture_run(&cx, &sc), CAPTURE_FAILED);
    CHECK_EQ_INT(ft.req_count, 0);               /* never reached the network */
    CHECK(status_count > 0);                     /* the clear note was surfaced */
    CHECK_EQ_STR(status_log[0], "Recording unreadable - not uploaded");
    sidecar_t back; sidecar_load(&st, "c-torn", &back);
    CHECK_EQ_STR(back.state, "failed");
    CHECK_EQ_STR(back.error, "bad_wav_header");
    CHECK_EQ_INT(capture_pending_count(&st), 0); /* out of the pending queue */
}

/* ...while a truncated-but-VALID-header file (clean stop, data cut short
 * afterwards) is fine and uploads normally, per the same design row. */
static void test_truncated_valid_header_uploads(void) {
    fresh("c-trunc");
    uint8_t hdr[44];
    wav_write_header(hdr, 16000, 16, 1, 32000);  /* claims 32000 data bytes... */
    CHECK_EQ_INT(st.write(st.ctx, "/rec/c-trunc.wav", hdr, sizeof hdr), 0);
    /* ...but the file ends right after the header */
    ft_push(&ft, 0, 200, "{\"id\":\"c-trunc\",\"state\":\"received\"}");
    ft_push(&ft, 0, 200, "{\"server_time\":1,\"captures\":[{\"id\":\"c-trunc\",\"state\":\"done\"}]}");
    CHECK_EQ_INT(capture_run(&cx, &sc), CAPTURE_DONE);
    CHECK_EQ_INT(ft.req_count, 2);               /* the upload happened */
}

/* The sync retry path refuses a torn WAV the same way: it drops out of the
 * queue without a network call while healthy pending captures still go. */
static void test_retry_skips_torn_wav(void) {
    fresh("c-torn2");                            /* not_uploaded, zeroed header */
    uint8_t zeroed[44];
    memset(zeroed, 0, sizeof zeroed);
    CHECK_EQ_INT(st.write(st.ctx, "/rec/c-torn2.wav", zeroed, sizeof zeroed), 0);
    sidecar_t good; sidecar_init(&good, "c-good");   /* not_uploaded, valid header */
    sidecar_save(&st, &good);
    uint8_t hdr[44];
    wav_write_header(hdr, 16000, 16, 1, 8000);
    CHECK_EQ_INT(st.write(st.ctx, "/rec/c-good.wav", hdr, sizeof hdr), 0);
    rec_index_append(&st, "c-torn2");
    rec_index_append(&st, "c-good");

    CHECK_EQ_INT(capture_pending_count(&st), 2);
    ft_push(&ft, 0, 200, "{\"id\":\"c-good\",\"state\":\"received\"}");
    CHECK_EQ_INT(capture_retry_pending(&cx), 1);
    CHECK_EQ_INT(ft.req_count, 1);               /* only c-good hit the network */
    sidecar_t back; sidecar_load(&st, "c-torn2", &back);
    CHECK_EQ_STR(back.state, "failed");
    CHECK_EQ_STR(back.error, "bad_wav_header");
    sidecar_load(&st, "c-good", &back);
    CHECK_EQ_STR(back.state, "uploaded");
    CHECK_EQ_INT(capture_pending_count(&st), 0); /* refusal drained it too */
}

/* Fix round (review Important 2): a capture the bridge rejects with a 4xx
 * must leave the pending queue -- upload_with_retry already never retries
 * HTP_ERR_CLIENT within one capture_run call (it would only get the same
 * rejection again), but leaving the sidecar untouched left it
 * "not_uploaded" forever, so every following sync re-streamed the whole
 * WAV. Trace: htp_upload_capture (client.c) has no local path that
 * returns HTP_ERR_CLIENT -- unlike htp_poll_captures/htp_complete_item/
 * htp_ack_notifications, its only HTP_ERR_CLIENT comes from status_to_err
 * mapping a real server 4xx -- so draining on it here can never discard a
 * capture over a local buffer-build failure. */
static void test_client_error_drains_pending_queue(void) {
    fresh("c-rej");
    rec_index_append(&st, "c-rej");
    uint8_t hdr[44];
    wav_write_header(hdr, 16000, 16, 1, 8000);
    CHECK_EQ_INT(st.write(st.ctx, "/rec/c-rej.wav", hdr, sizeof hdr), 0);

    ft_push(&ft, 0, 422, "{\"error\":\"malformed_capture\"}");
    CHECK_EQ_INT(capture_run(&cx, &sc), CAPTURE_FAILED);
    CHECK_EQ_INT(ft.req_count, 1);                  /* no retries on a 4xx */
    sidecar_t back; sidecar_load(&st, "c-rej", &back);
    CHECK_EQ_STR(back.state, "failed");
    CHECK_EQ_STR(back.error, "upload_rejected");
    CHECK_EQ_INT(capture_pending_count(&st), 0);    /* out of the pending queue */

    /* a following sync must not re-upload it */
    CHECK_EQ_INT(capture_retry_pending(&cx), 0);
    CHECK_EQ_INT(ft.req_count, 1);                  /* no new request made */
}

/* ...but a 401 is a token-configuration problem, not a rejection of this
 * capture -- it must NOT drain. Once the operator fixes the token, the
 * same capture must still be sitting there to upload. */
static void test_auth_error_leaves_capture_pending(void) {
    fresh("c-tok");
    rec_index_append(&st, "c-tok");
    uint8_t hdr[44];
    wav_write_header(hdr, 16000, 16, 1, 8000);
    CHECK_EQ_INT(st.write(st.ctx, "/rec/c-tok.wav", hdr, sizeof hdr), 0);

    ft_push(&ft, 0, 401, "{\"error\":\"unauthorized\"}");
    CHECK_EQ_INT(capture_run(&cx, &sc), CAPTURE_AUTH_ERROR);
    sidecar_t back; sidecar_load(&st, "c-tok", &back);
    CHECK_EQ_STR(back.state, "not_uploaded");       /* still pending, not drained */
    CHECK_EQ_INT(capture_pending_count(&st), 1);

    /* once the token is fixed, the next sync uploads it normally */
    ft_push(&ft, 0, 200, "{\"id\":\"c-tok\",\"state\":\"received\"}");
    CHECK_EQ_INT(capture_retry_pending(&cx), 1);
    sidecar_load(&st, "c-tok", &back);
    CHECK_EQ_STR(back.state, "uploaded");
    CHECK_EQ_INT(capture_pending_count(&st), 0);
}

static void test_retry_pending(void) {
    fresh("c-one");                               /* c-one: not_uploaded */
    sidecar_t two; sidecar_init(&two, "c-two");   /* c-two: already uploaded */
    str_copy(two.state, sizeof two.state, "uploaded");
    sidecar_save(&st, &two);
    sidecar_t three; sidecar_init(&three, "c-three");
    sidecar_save(&st, &three);
    rec_index_append(&st, "c-one"); rec_index_append(&st, "c-two"); rec_index_append(&st, "c-three");

    CHECK_EQ_INT(capture_pending_count(&st), 2);  /* c-one + c-three await upload */

    ft_push(&ft, 0, 200, "{\"id\":\"c-three\",\"state\":\"received\"}");  /* newest first */
    ft_push(&ft, 0, 200, "{\"id\":\"c-one\",\"state\":\"received\"}");
    CHECK_EQ_INT(capture_retry_pending(&cx), 2);
    CHECK_EQ_INT(ft.req_count, 2);                /* c-two untouched */
    sidecar_t back; sidecar_load(&st, "c-one", &back);
    CHECK_EQ_STR(back.state, "uploaded");
    CHECK_EQ_INT(capture_pending_count(&st), 0);  /* retry drained the queue */
}

int main(void) {
    test_note_done();
    test_conversation_reply();
    test_offline_after_three_upload_attempts();
    test_auth_error_stops_immediately();
    test_poll_window_timeout();
    test_failed_capture();
    test_torn_wav_refused();
    test_truncated_valid_header_uploads();
    test_retry_skips_torn_wav();
    test_client_error_drains_pending_queue();
    test_auth_error_leaves_capture_pending();
    test_retry_pending();
    return HARNESS_REPORT();
}
