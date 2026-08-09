#include "rec_index.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REC_INDEX_PATH "/rec/index"
#define REC_INDEX_CAP 16384

/* Compaction (Task 19, closing the limitation accepted in Task 5): the
 * index is append-only and rec_index_list() can only read the FIRST
 * REC_INDEX_CAP bytes (the storage port has no seek), so an index past
 * 16 KB silently stops listing the NEWEST captures -- at ~21 bytes per
 * real id that is roughly 780 recordings. Once an append sees the file
 * above REC_INDEX_COMPACT_AT, it atomically rewrites it down to the
 * newest REC_INDEX_KEEP complete lines first. 8 KB / 128 keeps the file
 * permanently under the read cap with wide margin, and 128 retained ids
 * is 4x the 32 that any consumer actually reads (rec_index_list caps max
 * at 32; retry/backfill scan the newest 32). Trade-off, documented in
 * firmware/README.md: ids older than the retained window are forgotten
 * for listing and retry -- the WAV/sidecar files themselves stay on the
 * card. Compacting a legacy index whose unread tail already exceeds the
 * old 16 KB read cap is a one-way door: that tail was already unreachable
 * to every reader either way, but compaction deletes it outright, so a
 * future firmware build that raised REC_INDEX_CAP could no longer recover
 * it -- today's compaction is written against today's cap.
 *
 * Safety rule: the index is NEVER destroyed here -- if the read or
 * the rewrite fails, this falls through to the plain append and the
 * worst case is yesterday's behavior (oversized index, newest entries
 * unlistable), not a lost index. One narrow edge in that guarantee: before
 * this change /rec/index was only ever appended to, so /rec/index.tmp
 * could never exist; the rewrite below goes through st_write(), whose
 * tmp-then-rename replace now creates one. If the process dies between
 * that write and its rename, the compacted content is left stranded in
 * the .tmp file while /rec/index itself still holds its pre-compaction
 * (still valid) content -- normal reads are unaffected. But st_read()'s
 * documented orphan-recovery contract promotes a "<path>.tmp" back to
 * "<path>" whenever "<path>" becomes unreadable, so if /rec/index were to
 * fail a read for an unrelated reason before the next successful
 * compaction, recovery would resurrect the stale compacted snapshot and
 * silently roll back every append made since. The next successful
 * compaction overwrites the orphan and closes the window; it is narrow
 * (requires two independent failures) but real. */
#define REC_INDEX_COMPACT_AT 8192
#define REC_INDEX_KEEP       128

/* Index read buffer: static, not stack. As a local this was a 16.4 KB
 * frame (`entry a1, 0x40b0` in the C5 binary) -- allocated in the
 * prologue before a single byte is read, it alone consumed 80% of the
 * 20 KB main-task stack and blew it on the first hardware sync session.
 * Same pure-C single-task ruling as sync.c's batch statics: no OS or
 * allocator calls, every caller (sync/capture/UI flows) is the one main
 * task, and the buffer is fully re-read before each use. */
static char s_index_buf[REC_INDEX_CAP];

/* Best-effort compaction; see the constants above. `len` is what the read
 * returned (<= REC_INDEX_CAP - 1). Works on COMPLETE lines only: a capped
 * read of a legacy >16 KB index ends mid-line, and retaining that torn
 * fragment would concatenate it with the next appended id -- everything
 * after the last '\n' is dropped along with the invisible tail (both were
 * already unreachable to every reader). */
static void rec_index_compact(port_storage_t *st, size_t len) {
    const char *buf = s_index_buf;

    size_t end = len;                     /* end of the last complete line */
    while (end > 0 && buf[end - 1] != '\n') end--;
    if (end == 0) return;

    int lines = 0;
    for (size_t i = 0; i < end; i++)
        if (buf[i] == '\n') lines++;
    if (lines <= REC_INDEX_KEEP) return;  /* nothing old enough to drop */

    int drop = lines - REC_INDEX_KEEP;
    size_t cut = 0;
    while (cut < end && drop > 0)
        if (buf[cut++] == '\n') drop--;

    /* Atomic replace by the storage-port contract (sidecar.c relies on the
     * same guarantee). A failure is deliberately ignored: the old index is
     * still intact on the card and the caller's append proceeds. */
    (void)st->write(st->ctx, REC_INDEX_PATH, buf + cut, end - cut);
}

int rec_index_append(port_storage_t *st, const char *capture_id) {
    char line[96];
    snprintf(line, sizeof line, "%s\n", capture_id);

    size_t len = 0;
    if (st->read != NULL && st->write != NULL &&
        st->read(st->ctx, REC_INDEX_PATH, s_index_buf, REC_INDEX_CAP - 1, &len) == 0 &&
        len > REC_INDEX_COMPACT_AT)
        rec_index_compact(st, len);

    return st->append(st->ctx, REC_INDEX_PATH, line, strlen(line));
}

int rec_index_list(port_storage_t *st, char ids[][64], int max) {
    char *const buf = s_index_buf;
    size_t len = 0;

    if (st == NULL || ids == NULL) return 0;
    if (st->read == NULL) return 0;
    if (st->read(st->ctx, REC_INDEX_PATH, buf, REC_INDEX_CAP - 1, &len) != 0) {
        return 0;
    }

    if (len > REC_INDEX_CAP - 1) len = REC_INDEX_CAP - 1;
    buf[len] = 0;

    if (max <= 0 || max > 32) return 0;

    size_t offsets[32];
    memset(offsets, 0, sizeof(offsets));
    int offset_count = 0;

    /* Scan through buffer collecting line offsets (last max only) */
    for (size_t i = 0; i < len; ) {
        size_t line_start = i;

        /* Find next newline */
        while (i < len && buf[i] != '\n') i++;

        /* Process this line */
        if (i > line_start) {
            /* Non-empty line */
            if (offset_count < max) {
                offsets[offset_count] = line_start;
                offset_count++;
            } else {
                /* Shift and add */
                for (int j = 0; j < max - 1; j++) {
                    offsets[j] = offsets[j + 1];
                }
                offsets[max - 1] = line_start;
            }
        }

        /* Move past newline */
        if (i < len) i++;  /* skip '\n' */
    }

    /* Output in reverse (newest first) */
    int out_count = 0;
    for (int i = offset_count - 1; i >= 0 && out_count < max; i--) {
        size_t off = offsets[i];
        if (off >= len) break;

        /* Find end of line */
        size_t end = off;
        while (end < len && buf[end] != '\n') end++;

        /* Extract line */
        size_t line_len = end - off;
        if (line_len > 63) line_len = 63;

        if (line_len > 0) {
            for (size_t k = 0; k < line_len; k++) {
                ids[out_count][k] = buf[off + k];
            }
            ids[out_count][line_len] = 0;
            out_count++;
        }
    }

    return out_count;
}
