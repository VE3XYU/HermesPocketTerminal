#include "harness.h"
#include "sidecar.h"
#include "util.h"
#include "fakes/fake_storage.h"
#include <string.h>

int main(void) {
    fake_storage_t fs; port_storage_t st; fstore_init(&fs, &st);

    sidecar_t sc;
    sidecar_init(&sc, "c-20260804-101502-3fa9");
    CHECK_EQ_STR(sc.state, "not_uploaded");
    CHECK_EQ_STR(sc.id, "c-20260804-101502-3fa9");
    sc.recorded_at = 1785838502LL;
    CHECK_EQ_INT(sidecar_save(&st, &sc), 0);

    sidecar_t back;
    CHECK_EQ_INT(sidecar_load(&st, "c-20260804-101502-3fa9", &back), 0);
    CHECK_EQ_STR(back.state, "not_uploaded");
    CHECK_EQ_INT(back.recorded_at, 1785838502LL);
    CHECK_EQ_STR(back.transcript, "");

    /* Backfill preserves other fields */
    str_copy(back.transcript, sizeof back.transcript, "Add milk to the shopping list");
    str_copy(back.state, sizeof back.state, "done");
    CHECK_EQ_INT(sidecar_save(&st, &back), 0);
    sidecar_t again;
    CHECK_EQ_INT(sidecar_load(&st, "c-20260804-101502-3fa9", &again), 0);
    CHECK_EQ_STR(again.transcript, "Add milk to the shopping list");
    CHECK_EQ_INT(again.recorded_at, 1785838502LL);

    CHECK_EQ_INT(sidecar_load(&st, "c-nope", &again), -1);

    char p[96];
    sidecar_path(p, "c-x");     CHECK_EQ_STR(p, "/rec/c-x.json");
    sidecar_wav_path(p, "c-x"); CHECK_EQ_STR(p, "/rec/c-x.wav");

    /* SD-full: save fails loudly, never silently */
    fs.fail_writes = 1;
    CHECK_EQ_INT(sidecar_save(&st, &sc), -1);
    return HARNESS_REPORT();
}
