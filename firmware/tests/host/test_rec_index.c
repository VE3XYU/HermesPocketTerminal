#include "harness.h"
#include "rec_index.h"
#include "fakes/fake_storage.h"
#include <stdio.h>

int main(void) {
    fake_storage_t fs; port_storage_t st; fstore_init(&fs, &st);
    char ids[8][64];

    CHECK_EQ_INT(rec_index_list(&st, ids, 8), 0);   /* empty index is fine */

    CHECK_EQ_INT(rec_index_append(&st, "c-oldest"), 0);
    CHECK_EQ_INT(rec_index_append(&st, "c-middle"), 0);
    CHECK_EQ_INT(rec_index_append(&st, "c-newest"), 0);

    int n = rec_index_list(&st, ids, 8);
    CHECK_EQ_INT(n, 3);
    CHECK_EQ_STR(ids[0], "c-newest");    /* newest first */
    CHECK_EQ_STR(ids[1], "c-middle");
    CHECK_EQ_STR(ids[2], "c-oldest");

    /* max caps the result at the newest entries */
    n = rec_index_list(&st, ids, 2);
    CHECK_EQ_INT(n, 2);
    CHECK_EQ_STR(ids[0], "c-newest");
    CHECK_EQ_STR(ids[1], "c-middle");

    /* Test with 300 entries to verify no fixed-array truncation.
     * Reuse fs instead of creating fs2 to avoid stack overflow. */
    fstore_init(&fs, &st);  /* reinit with fresh storage */
    for (int i = 0; i < 300; i++) {
        char id[64];
        snprintf(id, sizeof id, "id-%d", i);
        CHECK_EQ_INT(rec_index_append(&st, id), 0);
    }

    /* Retrieve the last 8 entries (newest first) */
    char ids_300[8][64];
    n = rec_index_list(&st, ids_300, 8);
    CHECK_EQ_INT(n, 8);
    CHECK_EQ_STR(ids_300[0], "id-299");  /* newest */
    CHECK_EQ_STR(ids_300[1], "id-298");
    CHECK_EQ_STR(ids_300[2], "id-297");
    CHECK_EQ_STR(ids_300[3], "id-296");
    CHECK_EQ_STR(ids_300[4], "id-295");
    CHECK_EQ_STR(ids_300[5], "id-294");
    CHECK_EQ_STR(ids_300[6], "id-293");
    CHECK_EQ_STR(ids_300[7], "id-292");  /* oldest in window */

    /* ---- Paged listing (final review, Important 1): skip_newest steps
     * past the newest entries so a 32-slot buffer can walk the whole
     * retained window. ---- */
    n = rec_index_list_page(&st, ids_300, 8, 0);      /* page 0 == plain list */
    CHECK_EQ_INT(n, 8);
    CHECK_EQ_STR(ids_300[0], "id-299");
    CHECK_EQ_STR(ids_300[7], "id-292");
    n = rec_index_list_page(&st, ids_300, 8, 8);      /* next page continues */
    CHECK_EQ_INT(n, 8);
    CHECK_EQ_STR(ids_300[0], "id-291");
    CHECK_EQ_STR(ids_300[7], "id-284");
    n = rec_index_list_page(&st, ids_300, 8, 296);    /* short final page */
    CHECK_EQ_INT(n, 4);
    CHECK_EQ_STR(ids_300[0], "id-3");
    CHECK_EQ_STR(ids_300[3], "id-0");                 /* the oldest entry is reachable */
    CHECK_EQ_INT(rec_index_list_page(&st, ids_300, 8, 300), 0);   /* past the end */
    CHECK_EQ_INT(rec_index_list_page(&st, ids_300, 8, 1000), 0);
    CHECK_EQ_INT(rec_index_list_page(&st, ids_300, 8, -1), 0);    /* rejected */

    /* A full page walk (the retry scan's loop shape) visits every entry
     * exactly once, newest first, and terminates on the short page. */
    {
        char page[32][64];
        int seen = 0, pn = 0;
        for (int skip = 0; ; skip += 32) {
            pn = rec_index_list_page(&st, page, 32, skip);
            if (pn <= 0) break;
            if (seen == 0) CHECK_EQ_STR(page[0], "id-299");
            seen += pn;
            if (pn < 32) { CHECK_EQ_STR(page[pn - 1], "id-0"); break; }
        }
        CHECK_EQ_INT(seen, 300);
    }

    /* ---- Task 19 compaction: crossing the 8 KB threshold must keep the
     * NEWEST-first listing correct. 60-char ids make each line 61 bytes,
     * so 400 appends are ~24 KB raw -- far past the 16 KB read cap that,
     * before compaction, silently hid every id appended after ~byte
     * 16384 (the append-only file's newest entries). ---- */
    {
        fstore_init(&fs, &st);
        char id[64];
        for (int i = 0; i < 400; i++) {
            snprintf(id, sizeof id,
                     "c-%04d-padpadpadpadpadpadpadpadpadpadpadpadpadpadpadpadpa", i);
            CHECK_EQ_INT(rec_index_append(&st, id), 0);
        }
        n = rec_index_list(&st, ids, 3);
        CHECK_EQ_INT(n, 3);
        CHECK_EQ_STR(ids[0], "c-0399-padpadpadpadpadpadpadpadpadpadpadpadpadpadpadpadpa");
        CHECK_EQ_STR(ids[1], "c-0398-padpadpadpadpadpadpadpadpadpadpadpadpadpadpadpadpa");
        CHECK_EQ_STR(ids[2], "c-0397-padpadpadpadpadpadpadpadpadpadpadpadpadpadpadpadpa");
        /* the on-disk file stayed under the read cap (compaction ran) */
        {
            const char *raw = fstore_get(&fs, "/rec/index");
            CHECK(raw != NULL);
            CHECK(strlen(raw) < 16384);
        }
        /* ...and the page walk still covers the WHOLE post-compaction
         * window: at least the newest 128 survive, contiguous down to the
         * oldest retained id (final review, Important 1). */
        {
            char page[32][64];
            int seen = 0, pn = 0;
            char last[64] = "";
            for (int skip = 0; ; skip += 32) {
                pn = rec_index_list_page(&st, page, 32, skip);
                if (pn <= 0) break;
                seen += pn;
                snprintf(last, sizeof last, "%s", page[pn - 1]);
                if (pn < 32) break;
            }
            CHECK(seen >= 128);
            char expect_oldest[64];
            snprintf(expect_oldest, sizeof expect_oldest,
                     "c-%04d-padpadpadpadpadpadpadpadpadpadpadpadpadpadpadpadpa",
                     400 - seen);
            CHECK_EQ_STR(last, expect_oldest);
        }
    }

    /* ---- append-failure propagation (final review, Important 2 relies on
     * it): record_capture() in main.c now checks this return to avoid
     * promising "will upload later" for a capture that never entered the
     * queue. ---- */
    {
        fstore_init(&fs, &st);
        fs.fail_writes = 1;
        CHECK_EQ_INT(rec_index_append(&st, "c-unqueued") == 0, 0);   /* must report failure */
        fs.fail_writes = 0;
    }

    /* ---- compaction-failure fallback: when the atomic rewrite cannot
     * happen, appends must keep working and the index must never be
     * destroyed. 200 x 61-byte lines cross the 8 KB threshold (so
     * compaction is attempted and fails on every later append) while
     * staying under the 16 KB read cap -- listing still sees the newest
     * id, exactly the pre-compaction behavior. ---- */
    {
        fstore_init(&fs, &st);
        fs.fail_replace_writes = 1;
        char id[64];
        for (int i = 0; i < 200; i++) {
            snprintf(id, sizeof id,
                     "c-%04d-padpadpadpadpadpadpadpadpadpadpadpadpadpadpadpadpa", i);
            CHECK_EQ_INT(rec_index_append(&st, id), 0);   /* plain append still lands */
        }
        n = rec_index_list(&st, ids, 2);
        CHECK_EQ_INT(n, 2);
        CHECK_EQ_STR(ids[0], "c-0199-padpadpadpadpadpadpadpadpadpadpadpadpadpadpadpadpa");
        CHECK_EQ_STR(ids[1], "c-0198-padpadpadpadpadpadpadpadpadpadpadpadpadpadpadpadpa");
        fs.fail_replace_writes = 0;
    }

    return HARNESS_REPORT();
}
