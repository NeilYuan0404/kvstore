#ifndef __SERVER_H__
#define __SERVER_H__

#define INIT_BUFFER_SIZE    (64 * 1024)
#define CONNECTION_SIZE      256

#define ENABLE_HTTP          0
#define ENABLE_WEBSOCKET     0
#define ENABLE_KVSTORE       1

typedef int (*RCALLBACK)(int fd);

struct conn {
    int fd;
    char *rbuffer;
    int rlength;
    int rcapacity;
    char *wbuffer;
    int wlength;
    int wcapacity;
    RCALLBACK send_callback;
    union {
        RCALLBACK recv_callback;
        RCALLBACK accept_callback;
    } r_action;
    int status;
#if 1
    char *payload;
    char mask[4];
#endif
};

#if ENABLE_HTTP
int http_request(struct conn *c);
int http_response(struct conn *c);
#endif

#if ENABLE_WEBSOCKET
int ws_request(struct conn *c);
int ws_response(struct conn *c);
#endif

#if ENABLE_KVSTORE
int kvs_request(struct conn *c);
int kvs_response(struct conn *c);
#endif

#endif