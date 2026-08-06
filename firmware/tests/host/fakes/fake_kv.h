#ifndef FAKE_KV_H
#define FAKE_KV_H
#include "ports.h"
typedef struct { char keys[16][32]; char vals[16][64]; int count; } fake_kv_t;
void fkv_init(fake_kv_t *f, port_kv_t *out);
#endif
