#ifndef __SERVER_H__
#define __SERVER_H__

#define INIT_BUFFER_SIZE    (64 * 1024)    // 初始64KB，可动态扩容
#define CONNECTION_SIZE      256             // 最大连接数

#define ENABLE_HTTP          0
#define ENABLE_WEBSOCKET     0
#define ENABLE_KVSTORE       1

typedef int (*RCALLBACK)(int fd);

struct conn {
    int fd;
    
    char *rbuffer;      // 动态分配的读缓冲区
    int rlength;        // 当前数据长度
    int rcapacity;      // 缓冲区总容量
    
    char *wbuffer;      // 动态分配的写缓冲区
    int wlength;        // 待发送数据长度
    int wcapacity;      // 缓冲区总容量

    RCALLBACK send_callback;

    union {
        RCALLBACK recv_callback;
        RCALLBACK accept_callback;
    } r_action;

    int status;
#if 1 // websocket
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
