#include "htp_backoff.h"

void htp_backoff_init(htp_backoff_t *b, unsigned base_ms, unsigned cap_ms) {
    b->attempt = 0; b->base_ms = base_ms; b->cap_ms = cap_ms;
}

unsigned htp_backoff_next(htp_backoff_t *b) {
    unsigned v = b->base_ms;
    for (int i = 0; i < b->attempt && v < b->cap_ms; i++) v *= 2;
    if (v > b->cap_ms) v = b->cap_ms;
    b->attempt++;
    return v;
}
