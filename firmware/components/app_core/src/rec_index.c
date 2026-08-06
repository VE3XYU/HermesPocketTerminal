#include "rec_index.h"
#include "util.h"
#include <stdio.h>
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

    /* Read the entire index file. If it doesn't exist, that's OK (empty index). */
    if (st->read(st->ctx, REC_INDEX_PATH, buf, sizeof buf - 1, &len) != 0) {
        /* File doesn't exist; empty index is valid. */
        return 0;
    }

    buf[len] = 0;

    /* Split lines and collect them in a forward array. */
    char *lines[256];
    int count = 0;
    char *line_start = buf;
    for (size_t i = 0; i < len && count < (int)(sizeof(lines) / sizeof(lines[0])); i++) {
        if (buf[i] == '\n') {
            buf[i] = 0;
            if (line_start[0] != 0) {  /* Skip empty lines */
                lines[count++] = line_start;
            }
            line_start = buf + i + 1;
        }
    }
    /* Handle the last line if it doesn't end with \n */
    if (line_start < buf + len && line_start[0] != 0 && count < (int)(sizeof(lines) / sizeof(lines[0]))) {
        lines[count++] = line_start;
    }

    /* Copy the last 'max' entries in reverse order into ids (newest first) */
    int start = count > max ? count - max : 0;
    int out_count = 0;
    for (int i = count - 1; i >= start && out_count < max; i--) {
        str_copy(ids[out_count], 64, lines[i]);
        out_count++;
    }

    return out_count;
}
