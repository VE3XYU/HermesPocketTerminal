#ifndef IDF_TRANSPORT_H
#define IDF_TRANSPORT_H
#include "htp_client.h"

/* esp_http_client implementation of Task 3's htp_transport_t contract.
 *
 * - Request URL is base_url + req->path. An "https://" base validates the
 *   server against the built-in certificate bundle; an "http://" base is
 *   honored as-is (LAN development against a mock bridge, design §5.5).
 * - req->body_file is streamed from the SD card in 4 KB chunks, so a 3.8 MB
 *   capture never has to fit in RAM.
 * - req->sink_file is streamed the other way into "<sink>.part" and renamed
 *   into place only after the whole body arrived -- a torn download can
 *   never masquerade as a complete reply.
 * - req->timeout_ms is mandatory and is enforced as the network timeout; a
 *   request that arrives without one is rejected as a transport error
 *   rather than silently getting a default.
 * - JSON responses are buffered in a single static 16 KB buffer, always
 *   NUL-terminated. resp->body points into it and stays valid until the
 *   next perform() call (exactly what htp_client.h promises).
 *
 * File paths follow the storage-port convention ("/rec/x.wav" ->
 * "/sdcard/rec/x.wav", see idf_ports_sd_path()).
 *
 * base_url is copied. Single-caller/single-task by design: one static
 * buffer set, one request in flight.
 */
void idf_transport_init(htp_transport_t *out, const char *base_url);

#endif
