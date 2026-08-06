#include "harness.h"
#include "rec_index.h"
#include "fakes/fake_storage.h"

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
    return HARNESS_REPORT();
}
