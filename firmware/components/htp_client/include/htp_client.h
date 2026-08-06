#ifndef HTP_CLIENT_H
#define HTP_CLIENT_H
#include <stddef.h>
#include <stdint.h>

#define HTP_OK           0
#define HTP_ERR_NETWORK -1   /* transport failure / timeout: retry with backoff */
#define HTP_ERR_AUTH    -2   /* 401: display auth error, never retry */
#define HTP_ERR_CLIENT  -3   /* other 4xx: never retry */
#define HTP_ERR_SERVER  -4   /* 5xx: retry with backoff */
#define HTP_ERR_PROTO   -5   /* malformed response */

typedef struct { const char *name; const char *value; } htp_header_t;

typedef struct {
    const char *method;            /* "GET" | "POST" */
    const char *path;              /* "/htp/v1/...", may embed query */
    htp_header_t headers[8];       /* protocol headers; client adds Authorization + X-Battery */
    int header_count;
    const char *content_type;      /* NULL when no body */
    const uint8_t *body; size_t body_len;   /* inline body, or */
    const char *body_file;         /* stream this file when non-NULL */
    const char *sink_file;         /* write response body to this path when non-NULL */
    int timeout_ms;                /* always > 0 */
} htp_request_t;

typedef struct {
    int transport_err;   /* nonzero = network-level failure; status invalid */
    int status;
    const char *body;    /* NUL-terminated; owned by transport until next perform() */
    size_t body_len;
} htp_response_t;

typedef struct {
    int (*perform)(void *ctx, const htp_request_t *req, htp_response_t *resp);
    void *ctx;
} htp_transport_t;

typedef struct {
    htp_transport_t *transport;
    const char *token;
    int battery_pct;               /* refreshed by caller; -1 omits the header */
} htp_client_t;

typedef enum { HTP_ST_RECEIVED, HTP_ST_TRANSCRIBING, HTP_ST_PROCESSING, HTP_ST_DONE,
               HTP_ST_REPLY_READY, HTP_ST_FAILED, HTP_ST_UNKNOWN } htp_capture_state_t;

typedef struct {
    char id[64];
    htp_capture_state_t state;
    char transcript[1024];         /* "" when absent; safe-truncated */
    char conversation_id[32];      /* "" when absent */
    char error[48];                /* failed reason slug */
} htp_capture_status_t;

typedef struct { char id[32]; char text[64]; int done; char style[12]; } htp_dash_item_t;
typedef struct {
    int unchanged;
    char rev[24]; char title[48];
    htp_dash_item_t items[32]; int item_count;
    long long server_time; int sync_interval;
} htp_dashboard_t;

typedef struct { char id[24]; char text[200]; int urgent; long long created; } htp_notification_t;
typedef struct { htp_notification_t items[16]; int count; long long server_time; } htp_notifications_t;

typedef struct {
    const char *capture_id;
    const char *wav_path;
    long long recorded_at;         /* <= 0 -> omit X-Recorded-At */
    const char *conversation_id;   /* NULL or "" -> omit X-Conversation-Id */
} htp_upload_params_t;

void htp_client_init(htp_client_t *c, htp_transport_t *t, const char *token);
int  htp_upload_capture(htp_client_t *c, const htp_upload_params_t *p);
int  htp_poll_captures(htp_client_t *c, const char *const ids[], int n,
                       htp_capture_status_t out[], int max_out, long long *server_time);
                       /* returns count >= 0, or HTP_ERR_* */
int  htp_download_reply(htp_client_t *c, const char *capture_id, const char *dest_path);
int  htp_get_dashboard(htp_client_t *c, const char *rev, htp_dashboard_t *out);
int  htp_complete_item(htp_client_t *c, const char *item_id, char rev_out[24]);
int  htp_get_notifications(htp_client_t *c, htp_notifications_t *out);
int  htp_ack_notifications(htp_client_t *c, const char *const ids[], int n);

#endif
