#ifndef REC_INDEX_H
#define REC_INDEX_H
#include "ports.h"

int rec_index_append(port_storage_t *st, const char *capture_id);
int rec_index_list(port_storage_t *st, char ids[][64], int max); /* newest first; returns count */

/* Paged variant: skip the newest `skip_newest` entries, then return up to
 * `max` (<= 32) of the next-newest, newest first. Walking skip_newest = 0,
 * 32, 64... until a call returns less than `max` visits every entry the
 * index retains while reusing one 32-slot id buffer -- how the upload-retry
 * scan and the pending count cover the whole retained window without a
 * 128-wide array (final review, Important 1). Each call re-reads the index,
 * so the walk is only coherent while nothing appends between pages (true
 * for every caller: one main task, no appends inside a scan). */
int rec_index_list_page(port_storage_t *st, char ids[][64], int max, int skip_newest);

#endif
