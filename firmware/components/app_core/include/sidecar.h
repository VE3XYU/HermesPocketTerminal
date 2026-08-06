#ifndef SIDECAR_H
#define SIDECAR_H
#include "ports.h"

typedef struct {
    char id[64];
    char state[16];          /* "not_uploaded" | "uploaded" | "done" | "reply_ready" | "failed" */
    char transcript[1024];
    char conversation_id[32];
    char error[48];
    long long recorded_at;   /* 0 = unknown */
    long long uploaded_at;   /* 0 = never confirmed */
} sidecar_t;

void sidecar_init(sidecar_t *sc, const char *capture_id);        /* state=not_uploaded */
int  sidecar_load(port_storage_t *st, const char *capture_id, sidecar_t *out);
int  sidecar_save(port_storage_t *st, const sidecar_t *sc);      /* atomic via st->write */
void sidecar_path(char out[96], const char *capture_id);         /* "/rec/<id>.json" */
void sidecar_wav_path(char out[96], const char *capture_id);     /* "/rec/<id>.wav"  */

#endif
