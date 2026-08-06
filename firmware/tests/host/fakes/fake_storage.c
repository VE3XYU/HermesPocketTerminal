#include "fake_storage.h"
#include "util.h"
#include <string.h>

static int slot(fake_storage_t *f, const char *path, int create) {
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (f->files[i].used && !strcmp(f->files[i].path, path)) return i;
    if (!create) return -1;
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (!f->files[i].used) {
            f->files[i].used = 1; f->files[i].len = 0;
            str_copy(f->files[i].path, 96, path);
            return i;
        }
    return -1;
}
static int fs_read(void *ctx, const char *p, void *buf, size_t cap, size_t *len) {
    fake_storage_t *f = ctx; int i = slot(f, p, 0);
    if (i < 0) return -1;
    size_t n = f->files[i].len < cap ? f->files[i].len : cap;
    memcpy(buf, f->files[i].data, n); if (len) *len = n;
    return 0;
}
static int fs_write(void *ctx, const char *p, const void *d, size_t n) {
    fake_storage_t *f = ctx;
    if (f->fail_writes || n > FS_MAX_BYTES) return -1;
    int i = slot(f, p, 1); if (i < 0) return -1;
    memcpy(f->files[i].data, d, n); f->files[i].len = n;
    return 0;
}
static int fs_append(void *ctx, const char *p, const void *d, size_t n) {
    fake_storage_t *f = ctx;
    if (f->fail_writes) return -1;
    int i = slot(f, p, 1); if (i < 0 || f->files[i].len + n > FS_MAX_BYTES) return -1;
    memcpy(f->files[i].data + f->files[i].len, d, n); f->files[i].len += n;
    return 0;
}
static int fs_remove(void *ctx, const char *p) {
    fake_storage_t *f = ctx; int i = slot(f, p, 0);
    if (i < 0) return -1;
    f->files[i].used = 0; return 0;
}
static int fs_exists(void *ctx, const char *p) { return slot(ctx, p, 0) >= 0; }
static long long fs_free(void *ctx) { return ((fake_storage_t *)ctx)->free_bytes_value; }
static int fs_list(void *ctx, const char *dir, int (*cb)(const char *, void *), void *u) {
    fake_storage_t *f = ctx; size_t dl = strlen(dir);
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (f->files[i].used && !strncmp(f->files[i].path, dir, dl))
            if (cb(f->files[i].path + dl + 1, u)) return 0;   /* +1 skips '/' */
    return 0;
}
void fstore_init(fake_storage_t *f, port_storage_t *out) {
    memset(f, 0, sizeof *f);
    f->free_bytes_value = 1LL << 30;
    out->ctx = f; out->read = fs_read; out->write = fs_write; out->append = fs_append;
    out->remove = fs_remove; out->exists = fs_exists; out->free_bytes = fs_free; out->list = fs_list;
}
const char *fstore_get(fake_storage_t *f, const char *path) {
    int i = slot(f, path, 0);
    if (i < 0) return NULL;
    f->files[i].data[f->files[i].len] = 0;
    return f->files[i].data;
}
void fstore_put(fake_storage_t *f, const char *path, const char *data) {
    fs_write(f, path, data, strlen(data));
}
