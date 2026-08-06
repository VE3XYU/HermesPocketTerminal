#include "harness.h"
#include "htp_client.h"
#include "capture_flow.h"
#include "sidecar.h"
#include "rec_index.h"
#include "wav.h"
#include "fakes/posix_transport.h"
#include "fakes/fake_storage.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

size_t str_copy(char *, size_t, const char *);

static long long clk_epoch(void *c) { (void)c; return 1785838502LL; }
static unsigned clk_mono(void *c) { (void)c; static unsigned t; return t += 50; }
static void clk_sleep(void *c, unsigned ms) { (void)c;
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }

int main(void) {
    posix_transport_t pt; htp_transport_t tr;
    posix_transport_init(&pt, &tr, "127.0.0.1", 18787);
    htp_client_t cl; htp_client_init(&cl, &tr, "any-token-works-on-mock");
    cl.battery_pct = 55;

    /* a real (tiny) WAV on the real filesystem */
    uint8_t hdr[44]; wav_write_header(hdr, 16000, 16, 1, 320);
    uint8_t wav[364]; memcpy(wav, hdr, 44); memset(wav + 44, 0, 320);
    FILE *f = fopen("/tmp/htp_it.wav", "wb"); fwrite(wav, 1, sizeof wav, f); fclose(f);

    /* direct client calls against the live mock */
    htp_upload_params_t p = { .capture_id = "c-it-0001", .wav_path = "/tmp/htp_it.wav",
                              .recorded_at = 1785838502LL };
    CHECK_EQ_INT(htp_upload_capture(&cl, &p), HTP_OK);

    const char *ids[] = { "c-it-0001" };
    htp_capture_status_t st[1]; long long t;
    CHECK_EQ_INT(htp_poll_captures(&cl, ids, 1, st, 1, &t), 1);
    CHECK_EQ_INT(st[0].state, HTP_ST_REPLY_READY);       /* mock always replies */
    CHECK_EQ_STR(st[0].conversation_id, "v-mock");

    CHECK_EQ_INT(htp_download_reply(&cl, "c-it-0001", "/tmp/htp_it_reply.wav"), HTP_OK);
    FILE *rf = fopen("/tmp/htp_it_reply.wav", "rb");
    CHECK(rf != NULL);
    uint8_t rh[64]; size_t rn = fread(rh, 1, sizeof rh, rf); fclose(rf);
    wav_info_t inf;
    CHECK_EQ_INT(wav_parse_header(rh, rn, &inf), 0);     /* mock WAV is playable */

    htp_dashboard_t d;
    CHECK_EQ_INT(htp_get_dashboard(&cl, "", &d), HTP_OK);
    CHECK_EQ_STR(d.rev, "mockrev1");
    CHECK_EQ_INT(d.item_count, 2);
    CHECK_EQ_INT(d.sync_interval, 600);
    CHECK_EQ_INT(htp_get_dashboard(&cl, "mockrev1", &d), HTP_OK);
    CHECK_EQ_INT(d.unchanged, 1);

    htp_notifications_t nn;
    CHECK_EQ_INT(htp_get_notifications(&cl, &nn), HTP_OK);
    char rev[24];
    CHECK_EQ_INT(htp_complete_item(&cl, "t-9f2", rev), HTP_OK);

    /* the full capture flow, fake storage + real HTTP */
    fake_storage_t fs; port_storage_t stg; fstore_init(&fs, &stg);
    port_clock_t ck = { NULL, clk_epoch, clk_mono, clk_sleep };
    /* flow uploads sidecar_wav_path("/rec/<id>.wav") — the posix transport reads
     * the REAL fs, so pre-create the file there for this one test */
    FILE *g = fopen("/rec/c-it-flow.wav", "wb");
    int have_rec_dir = g != NULL;
    if (have_rec_dir) { fwrite(wav, 1, sizeof wav, g); fclose(g); }
    if (have_rec_dir) {
        capture_ctx_t cx = { .client = &cl, .storage = &stg, .clock = &ck,
            .poll_interval_ms = 100, .poll_window_ms = 10000,
            .reply_path = "/tmp/htp_it_reply2.wav" };
        sidecar_t sc; sidecar_init(&sc, "c-it-flow");
        sc.recorded_at = 1785838502LL; sidecar_save(&stg, &sc);
        CHECK_EQ_INT(capture_run(&cx, &sc), CAPTURE_REPLY_READY);
    } else {
        fprintf(stderr, "note: /rec not writable on this host, flow leg skipped\n");
    }
    return HARNESS_REPORT();
}
