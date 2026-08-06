#ifndef FAKE_TRANSPORT_H
#define FAKE_TRANSPORT_H
#include "htp_client.h"

#define FT_MAX 128
typedef struct {
    /* scripted responses, consumed in order */
    struct { int transport_err; int status; char body[4096]; } resp[FT_MAX];
    int resp_count, resp_next;
    /* captured requests (headers flattened to "Name: value" lines) */
    struct {
        char method[8]; char path[256]; char content_type[40];
        char headers[8][160]; int header_count;
        char body[1024]; size_t body_len;
        char body_file[128]; char sink_file[128];
        int timeout_ms;
    } req[FT_MAX];
    int req_count;
} fake_transport_t;

void ft_init(fake_transport_t *ft, htp_transport_t *out);
void ft_push(fake_transport_t *ft, int transport_err, int status, const char *body);
void ft_push_fixture(fake_transport_t *ft, int status, const char *fixture_name); /* FIXDIR/<name> */
const char *ft_find_header(fake_transport_t *ft, int req_idx, const char *name);  /* NULL if absent */
#endif
