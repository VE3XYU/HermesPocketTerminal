#ifndef FAKE_STORAGE_H
#define FAKE_STORAGE_H
#include "ports.h"
#define FS_MAX_FILES 64
#define FS_MAX_BYTES 65536
typedef struct {
    struct { char path[96]; char data[FS_MAX_BYTES]; size_t len; int used; } files[FS_MAX_FILES];
    long long free_bytes_value;   /* settable by tests; default 1<<30 */
    int fail_writes;              /* when 1, write/append return -1 (SD-full simulation) */
} fake_storage_t;
void fstore_init(fake_storage_t *f, port_storage_t *out);
const char *fstore_get(fake_storage_t *f, const char *path);   /* NULL if absent */
void fstore_put(fake_storage_t *f, const char *path, const char *data);
#endif
