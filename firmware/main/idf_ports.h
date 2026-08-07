#ifndef IDF_PORTS_H
#define IDF_PORTS_H
#include "ports.h"

/* Concrete ESP-IDF bindings for Task 1's port interfaces (app_core/ports.h).
 *
 * storage: logical paths ("/x") map to the SD card mount point
 *   ("/sdcard/x"); board_sd_mount() (Task 14, board.h) must have already
 *   succeeded. write() is tmp-file-then-rename; every app_core caller only
 *   ever sees logical paths, never "/sdcard" directly.
 * kv: backed by the NVS namespace "htp".
 * clock: epoch_s() reads time(), returning 0 (clockless) until Task 17
 *   wires the RTC via settimeofday(); mono_ms() is esp_timer, always live;
 *   sleep_ms() is vTaskDelay.
 * rng: esp_fill_random.
 *
 * All four ctx pointers are unused (NULL) — state lives in ESP-IDF globals
 * (the FATFS/NVS drivers), not caller-owned structs, so there is nothing
 * for idf_ports_init()'s caller to allocate or free.
 */
void idf_ports_init(port_storage_t *st, port_kv_t *kv, port_clock_t *ck, port_rng_t *rng);

/* The logical-path -> "/sdcard/..." mapping the storage port applies,
 * exported so the one file that cannot go through the port shares the same
 * convention instead of hard-coding a second copy of the prefix.
 *
 * That file is idf_transport.c: it streams multi-megabyte uploads and reply
 * downloads in 4 KB chunks, and port_storage_t is a whole-file interface
 * (read/write/append take a single buffer) with no seek or handle. Rather
 * than widen the port -- which app_core and the host fakes both implement --
 * the transport does its own stdio and borrows only the path rule.
 * Returns 0, or -1 when the result would not fit in cap. */
#define IDF_SD_PATH_MAX 160
int idf_ports_sd_path(const char *logical, char *out, size_t cap);

/* Diagnostic detail for the most recent st_write() failure. Device-only
 * side channel, deliberately outside the port_storage_t contract (that
 * stays a bare 0/-1 -- app_core and the host test suite's fake_storage.c
 * both depend on that signature, unchanged). Only meaningful right after
 * a port_storage_t write() call returned -1; step/err are stale (but
 * harmless to read) otherwise. */
typedef struct {
    const char *step;   /* "fopen" | "fwrite" | "fwrite short" | "fclose" | "rename" | "path" */
    int err;             /* errno at the point of failure */
} idf_write_fail_t;
void idf_ports_last_write_fail(idf_write_fail_t *out);

#endif
