#ifndef APP_PORTS_H
#define APP_PORTS_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    void *ctx;
    int  (*read)(void *ctx, const char *path, void *buf, size_t cap, size_t *len);
    int  (*write)(void *ctx, const char *path, const void *data, size_t len);
    int  (*append)(void *ctx, const char *path, const void *data, size_t len);
    int  (*remove)(void *ctx, const char *path);
    int  (*exists)(void *ctx, const char *path);
    long long (*free_bytes)(void *ctx);
    int  (*list)(void *ctx, const char *dir,
                 int (*cb)(const char *name, void *u), void *u);
} port_storage_t;

typedef struct {
    void *ctx;
    long long (*epoch_s)(void *ctx);
    unsigned  (*mono_ms)(void *ctx);
    void      (*sleep_ms)(void *ctx, unsigned ms);
} port_clock_t;

typedef struct {
    void *ctx;
    void (*fill)(void *ctx, uint8_t *buf, size_t n);
} port_rng_t;

typedef struct {
    void *ctx;
    int (*get)(void *ctx, const char *key, char *buf, size_t cap);
    int (*set)(void *ctx, const char *key, const char *val);
} port_kv_t;

#endif
