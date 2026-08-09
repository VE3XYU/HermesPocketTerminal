#include "harness.h"
#include "sync.h"
#include "capture_flow.h"
#include "sidecar.h"
#include "rec_index.h"
#include "fakes/fake_transport.h"
#include "fakes/fake_storage.h"
#include "fakes/fake_kv.h"
#include <string.h>

size_t str_copy(char *, size_t, const char *);

static char events[24][32]; static int nevents;
static void ev(const char *e) { str_copy(events[nevents++], 32, e); }

static long long epoch_now = 1785838502LL;
static long long clk_epoch(void *c) { (void)c; return epoch_now; }
static unsigned mono = 0;
static unsigned clk_mono(void *c) { (void)c; return mono += 100; }
static void clk_sleep(void *c, unsigned ms) { (void)c; mono += ms; }

static int rend_dash_ok(void *u, const htp_dashboard_t *d) { (void)u; (void)d; ev("dashboard"); return 0; }
static int rend_notif_ok(void *u, const htp_notifications_t *n) { (void)u; (void)n; ev("notifications"); return 0; }
static int rend_notif_fail(void *u, const htp_notifications_t *n) { (void)u; (void)n; ev("notif_fail"); return -1; }
static void chime(void *u) { (void)u; ev("chime"); }
static long long rtc_set_to = 0;
static void set_rtc(void *c, long long e) { (void)c; rtc_set_to = e; ev("set_rtc"); }

static fake_transport_t ft; static htp_transport_t tr; static htp_client_t cl;
static fake_storage_t fs; static port_storage_t st;
static fake_kv_t fk; static port_kv_t kv;
static port_clock_t ck = { NULL, clk_epoch, clk_mono, clk_sleep };
static capture_ctx_t cap;
static sync_ctx_t sx;

static void fresh(void) {
    nevents = 0; rtc_set_to = 0; mono = 0; epoch_now = 1785838502LL;
    ft_init(&ft, &tr); htp_client_init(&cl, &tr, "tok");
    fstore_init(&fs, &st); fkv_init(&fk, &kv);
    cap = (capture_ctx_t){ .client = &cl, .storage = &st, .clock = &ck,
        .poll_interval_ms = 1000, .poll_window_ms = 60000, .reply_path = "/tmp/r.wav" };
    sx = (sync_ctx_t){ .client = &cl, .storage = &st, .kv = &kv, .clock = &ck,
        .capture = &cap, .render_dashboard = rend_dash_ok,
        .render_notifications = rend_notif_ok, .chime = chime,
        .set_rtc = set_rtc };
}

/* One pending upload; urgent notification; changed dashboard; one sidecar needing
 * transcript backfill. Asserts the exact §7.3 order. */
static void test_full_cycle_order(void) {
    fresh();
    sidecar_t pending; sidecar_init(&pending, "c-pend"); sidecar_save(&st, &pending);
    rec_index_append(&st, "c-pend");
    sidecar_t up; sidecar_init(&up, "c-up");
    str_copy(up.state, sizeof up.state, "uploaded"); sidecar_save(&st, &up);
    rec_index_append(&st, "c-up");

    ft_push(&ft, 0, 200, "{\"id\":\"c-pend\",\"state\":\"received\"}");         /* 1 retry   */
    ft_push_fixture(&ft, 200, "notifications_get.json");                         /* 2 notifs  */
    ft_push_fixture(&ft, 200, "dashboard_get.json");                             /* 3 dash    */
    ft_push(&ft, 0, 200, "{\"server_time\":1785838509,\"captures\":["            /* 4 backfill*/
        "{\"id\":\"c-up\",\"state\":\"done\",\"transcript\":\"backfilled words\"},"
        "{\"id\":\"c-pend\",\"state\":\"transcribing\"}]}");
    ft_push_fixture(&ft, 200, "ack_post.json");                                  /* 5 ack     */

    sync_report_t rep;
    CHECK_EQ_INT(sync_cycle(&sx, &rep), 0);
    CHECK_EQ_INT(rep.uploads_retried, 1);
    CHECK_EQ_INT(rep.notifs_fetched, 1);
    CHECK_EQ_INT(rep.notifs_acked, 1);
    CHECK_EQ_INT(rep.dashboard_changed, 1);
    CHECK_EQ_INT(rep.sync_interval_s, 600);

    /* request order on the wire */
    CHECK(strstr(ft.req[0].path, "/captures") != NULL);           /* upload retry */
    CHECK(strstr(ft.req[1].path, "/notifications") != NULL);
    CHECK(strstr(ft.req[2].path, "/dashboard") != NULL);
    CHECK(strstr(ft.req[3].path, "/captures?ids=") != NULL);
    CHECK(strstr(ft.req[4].path, "/notifications/ack") != NULL);
    /* chime fired for the urgent fixture notification */
    int chimed = 0;
    for (int i = 0; i < nevents; i++) if (!strcmp(events[i], "chime")) chimed = 1;
    CHECK(chimed);
    /* transcript backfilled into the sidecar */
    sidecar_t back; sidecar_load(&st, "c-up", &back);
    CHECK_EQ_STR(back.transcript, "backfilled words");
    CHECK_EQ_STR(back.state, "done");
    /* kv updated */
    char v[64];
    CHECK_EQ_INT(kv.get(kv.ctx, "dash_rev", v, sizeof v), 0);
    CHECK_EQ_STR(v, "44818d09");
    CHECK_EQ_INT(kv.get(kv.ctx, "sync_s", v, sizeof v), 0);
    CHECK_EQ_STR(v, "600");
    /* server_time 1000000 differs wildly from epoch → RTC corrected */
    CHECK(rtc_set_to != 0);
}

static void test_unchanged_dashboard_no_render(void) {
    fresh();
    epoch_now = 1000000LL;   /* fixtures carry server_time 1000000; no drift here */
    kv.set(kv.ctx, "dash_rev", "1f6aab9c");
    ft_push(&ft, 0, 200, "{\"server_time\":1000000,\"notifications\":[]}");
    ft_push_fixture(&ft, 200, "dashboard_unchanged.json");
    sync_report_t rep;
    CHECK_EQ_INT(sync_cycle(&sx, &rep), 0);
    CHECK_EQ_INT(rep.dashboard_changed, 0);
    for (int i = 0; i < nevents; i++) CHECK(strcmp(events[i], "dashboard") != 0);
    /* no notifications → no ack request, no backfill needed → 2 requests total */
    CHECK_EQ_INT(ft.req_count, 2);
    /* rev passed on the request line */
    CHECK(strstr(ft.req[1].path, "rev=1f6aab9c") != NULL);
    /* server_time within 2 s of RTC → no correction */
    CHECK_EQ_INT(rtc_set_to, 0);
}

static void test_failed_render_not_acked(void) {
    fresh();
    sx.render_notifications = rend_notif_fail;
    ft_push_fixture(&ft, 200, "notifications_get.json");
    ft_push_fixture(&ft, 200, "dashboard_get.json");
    sync_report_t rep;
    CHECK_EQ_INT(sync_cycle(&sx, &rep), 0);
    CHECK_EQ_INT(rep.notifs_acked, 0);
    for (int i = 0; i < ft.req_count; i++)
        CHECK(strstr(ft.req[i].path, "/ack") == NULL);
}

static void test_acked_ring_dedup_no_rechime(void) {
    fresh();
    kv.set(kv.ctx, "acked", "n-1,n-9");
    ft_push_fixture(&ft, 200, "notifications_get.json");   /* redelivers n-1 */
    ft_push_fixture(&ft, 200, "dashboard_get.json");
    ft_push_fixture(&ft, 200, "ack_post.json");            /* still re-acked */
    sync_report_t rep;
    CHECK_EQ_INT(sync_cycle(&sx, &rep), 0);
    for (int i = 0; i < nevents; i++) CHECK(strcmp(events[i], "chime") != 0);
    CHECK_EQ_INT(rep.notifs_acked, 1);
}

int main(void) {
    test_full_cycle_order();
    test_unchanged_dashboard_no_render();
    test_failed_render_not_acked();
    test_acked_ring_dedup_no_rechime();
    return HARNESS_REPORT();
}
