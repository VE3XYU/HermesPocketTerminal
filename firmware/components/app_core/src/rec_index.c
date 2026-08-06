#include "rec_index.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REC_INDEX_PATH "/rec/index"
#define REC_INDEX_CAP 16384

int rec_index_append(port_storage_t *st, const char *capture_id) {
    char line[96];
    snprintf(line, sizeof line, "%s\n", capture_id);
    return st->append(st->ctx, REC_INDEX_PATH, line, strlen(line));
}

int rec_index_list(port_storage_t *st, char ids[][64], int max) {
    char buf[REC_INDEX_CAP];
    size_t len = 0;

    if (st == NULL || ids == NULL) return 0;
    if (st->read == NULL) return 0;
    if (st->read(st->ctx, REC_INDEX_PATH, buf, sizeof(buf) - 1, &len) != 0) {
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
