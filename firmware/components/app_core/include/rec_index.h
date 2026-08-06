#ifndef REC_INDEX_H
#define REC_INDEX_H
#include "ports.h"

int rec_index_append(port_storage_t *st, const char *capture_id);
int rec_index_list(port_storage_t *st, char ids[][64], int max); /* newest first; returns count */

#endif
