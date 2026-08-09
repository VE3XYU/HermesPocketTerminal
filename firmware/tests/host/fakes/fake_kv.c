#include "fake_kv.h"
#include "util.h"
#include <string.h>

static int fkv_get(void *ctx, const char *key, char *buf, size_t cap) {
    fake_kv_t *f = ctx;
    for (int i = 0; i < f->count; i++)
        if (!strcmp(f->keys[i], key)) { str_copy(buf, cap, f->vals[i]); return 0; }
    return -1;
}
static int fkv_set(void *ctx, const char *key, const char *val) {
    fake_kv_t *f = ctx;
    for (int i = 0; i < f->count; i++)
        if (!strcmp(f->keys[i], key)) { str_copy(f->vals[i], 64, val); return 0; }
    if (f->count >= 16) return -1;
    str_copy(f->keys[f->count], 32, key);
    str_copy(f->vals[f->count], 64, val);
    f->count++;
    return 0;
}
void fkv_init(fake_kv_t *f, port_kv_t *out) {
    memset(f, 0, sizeof *f);
    out->ctx = f; out->get = fkv_get; out->set = fkv_set;
}
