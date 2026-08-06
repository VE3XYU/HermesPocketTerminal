#ifndef HTP_BACKOFF_H
#define HTP_BACKOFF_H

typedef struct { int attempt; unsigned base_ms, cap_ms; } htp_backoff_t;

void htp_backoff_init(htp_backoff_t *b, unsigned base_ms, unsigned cap_ms);
unsigned htp_backoff_next(htp_backoff_t *b);   // base, 2*base, 4*base ... capped

#endif
