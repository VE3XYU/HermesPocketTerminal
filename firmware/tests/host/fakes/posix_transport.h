#ifndef POSIX_TRANSPORT_H
#define POSIX_TRANSPORT_H
#include "htp_client.h"
typedef struct { char host[64]; int port; char body[262144]; } posix_transport_t;
void posix_transport_init(posix_transport_t *pt, htp_transport_t *out,
                          const char *host, int port);
#endif
