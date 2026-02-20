#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/timerfd.h>

#include "../include/server.h"
#include "../include/kvs_replication.h"
#include "../include/kvs_persist.h"
#include "../include/kvs_base.h"

#define MAX_PORTS			1
#define TIME_SUB_MS(tv1, tv2)  ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)

static struct conn conn_list[CONNECTION_SIZE] = {0};

#if ENABLE_KVSTORE
// 修改消息处理函数类型，支持缓冲区大小和需要空间
/* typedef int (*msg_handler)(char *msg, int length, char *response, int resp_size,  */
/*                           int *processed, int *needed); */
static msg_handler kvs_handler;

/* 扩容读缓冲区 */
static int expand_rbuffer(struct conn *c, int needed) {
    int new_capacity = c->rcapacity;
    
    // 确保至少能容纳 needed 字节
    while (new_capacity - c->rlength < needed) {
        new_capacity *= 2;
        // 防止无限循环（最大限制）
        if (new_capacity > 128 * 1024 * 1024) {  // 128MB 上限
            printf("Read buffer too large for fd=%d\n", c->fd);
            return -1;
        }
    }
    
    char *new_buf = (char*)kvs_realloc(c->rbuffer, new_capacity);
    if (!new_buf) {
        printf("Failed to expand read buffer for fd=%d to %d bytes\n", 
               c->fd, new_capacity);
        return -1;
    }
    
    c->rbuffer = new_buf;
    c->rcapacity = new_capacity;
    
#ifdef DEBUG
    printf("Expanded read buffer for fd=%d from %d to %d bytes\n", 
           c->fd, c->rcapacity/2, new_capacity);
#endif
    return 0;
}

/* 扩容写缓冲区 */
static int expand_wbuffer(struct conn *c, int needed) {
    int new_capacity = c->wcapacity;
    
    // 确保至少能容纳 needed 字节
    while (new_capacity - c->wlength < needed) {
        new_capacity *= 2;
        // 防止无限循环（最大限制）
        if (new_capacity > 128 * 1024 * 1024) {  // 128MB 上限
            printf("Write buffer too large for fd=%d\n", c->fd);
            return -1;
        }
    }
    
    char *new_buf = (char*)kvs_realloc(c->wbuffer, new_capacity);
    if (!new_buf) {
        printf("Failed to expand write buffer for fd=%d to %d bytes\n", 
               c->fd, new_capacity);
        return -1;
    }
    
    c->wbuffer = new_buf;
    c->wcapacity = new_capacity;
    
#ifdef DEBUG
    printf("Expanded write buffer for fd=%d from %d to %d bytes\n", 
           c->fd, c->wcapacity/2, new_capacity);
#endif
    return 0;
}

int kvs_request(struct conn *c) {
    int processed = 0;
    int needed = 0;
    
    // 调用协议处理函数
    int resp_len = kvs_handler(c->rbuffer, c->rlength, 
                                c->wbuffer + c->wlength,
                                c->wcapacity - c->wlength,
                                &processed, &needed);
    
    if (resp_len == -2) {
        // 需要更大的写缓冲区
#ifdef DEBUG
        printf("[DEBUG] Need to expand write buffer for fd=%d, needed=%d\n", c->fd, needed);
#endif
        if (expand_wbuffer(c, needed) < 0) {
            return -1;
        }
        // 扩容后重新尝试处理同一条命令
        return kvs_request(c);
    }
    
    if (resp_len > 0) {
        c->wlength += resp_len;
    }
    
    if (resp_len < 0 && resp_len != -2) {
        printf("[ERROR] Protocol error on fd=%d\n", c->fd);
        return -1;
    }
    
    return 0;
}

int kvs_response(struct conn *c) {
    (void)c;
    return 0;
}
#endif

int accept_cb(int fd);
int recv_cb(int fd);
int send_cb(int fd);
static int timer_cb(int fd);

int epfd = 0;
struct timeval begin;

/* 确保 epfd 已初始化 */
static void ensure_epfd(void) {
    if (epfd <= 0) {
        epfd = epoll_create(1);
        if (epfd < 0) {
            perror("[EVENT] epoll_create failed");
            exit(1);
        }
#ifdef DEBUG
        printf("[EVENT] epoll fd created: %d\n", epfd);
#endif
    }
}

int set_event(int fd, int event, int flag) {
    ensure_epfd();

    struct epoll_event ev;
    ev.events = event;
    ev.data.fd = fd;
    int op = flag ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;

    if (epoll_ctl(epfd, op, fd, &ev) < 0) {
        printf("[EVENT] epoll_ctl failed, fd=%d, op=%s, errno=%d (%s)\n",
               fd, flag ? "ADD" : "MOD", errno, strerror(errno));
        return -1;
    }
    return 0;
}

/* 动态分配连接缓冲区 */
int event_register(int fd, int event) {
    if (fd < 0 || fd >= CONNECTION_SIZE) return -1;

    conn_list[fd].fd = fd;
    conn_list[fd].r_action.recv_callback = recv_cb;
    conn_list[fd].send_callback = send_cb;

    // 分配初始读缓冲区
    conn_list[fd].rbuffer = (char*)kvs_malloc(INIT_BUFFER_SIZE);
    conn_list[fd].rcapacity = INIT_BUFFER_SIZE;
    conn_list[fd].rlength = 0;
    memset(conn_list[fd].rbuffer, 0, INIT_BUFFER_SIZE);

    // 分配初始写缓冲区
    conn_list[fd].wbuffer = (char*)kvs_malloc(INIT_BUFFER_SIZE);
    conn_list[fd].wcapacity = INIT_BUFFER_SIZE;
    conn_list[fd].wlength = 0;
    memset(conn_list[fd].wbuffer, 0, INIT_BUFFER_SIZE);

    set_event(fd, event, 1);
    return 0;
}

int accept_cb(int fd) {
    struct sockaddr_in clientaddr;
    socklen_t len = sizeof(clientaddr);
    int clientfd = accept(fd, (struct sockaddr*)&clientaddr, &len);
    if (clientfd < 0) {
        printf("accept errno: %d --> %s\n", errno, strerror(errno));
        return -1;
    }

    int ret = kvs_replication_accept_master(clientfd);
    if (ret == 0) {
#ifdef DEBUG
        printf("[ACCEPT] PSYNC handshake succeeded, fd=%d taken over by replication\n", clientfd);
#endif
    } else {
#ifdef DEBUG
        printf("[ACCEPT] Normal client connected, fd=%d\n", clientfd);
#endif
        event_register(clientfd, EPOLLIN);
    }

    if ((clientfd % 1000) == 0) {
        struct timeval current;
        gettimeofday(&current, NULL);
        int time_used = TIME_SUB_MS(current, begin);
        memcpy(&begin, &current, sizeof(struct timeval));
#ifdef DEBUG
        printf("accept finished: %d, time_used: %d\n", clientfd, time_used);
#endif
    }
    return 0;
}




int recv_cb(int fd) {
    struct conn *c = &conn_list[fd];
    int remaining = c->rcapacity - c->rlength;
    
    // 如果剩余空间不足 4KB，尝试扩容
    if (remaining < 4096) {
        if (expand_rbuffer(c, 4096) < 0) {
            close(fd);
            epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
            return -1;
        }
        remaining = c->rcapacity - c->rlength;
    }

    int count = recv(fd, c->rbuffer + c->rlength, remaining, 0);
    if (count == 0) {
#ifdef DEBUG
        printf("client disconnect: %d\n", fd);
#endif
        close(fd);
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
        return 0;
    } else if (count < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;  // 非阻塞模式下无数据可读
        }
        printf("recv error: %d, %s\n", errno, strerror(errno));
        close(fd);
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
        return -1;
    }

    c->rlength += count;
    int total_processed = 0;

    while (1) {
        int processed = 0;
        int needed = 0;
        
        // 调用协议处理函数
        int resp_len = kvs_handler(c->rbuffer + total_processed,
                                   c->rlength - total_processed,
                                   c->wbuffer + c->wlength,
                                   c->wcapacity - c->wlength,
                                   &processed, &needed);
        
        if (resp_len == -2) {
            // 需要更大的写缓冲区
            if (expand_wbuffer(c, needed) < 0) {
                close(fd);
                epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                return -1;
            }
            // 扩容后重新处理同一条命令（不增加 total_processed）
            continue;
        }
        
        if (resp_len < 0) {
            printf("[ERROR] Protocol error on fd=%d, resetting buffer\n", fd);
            c->rlength = 0;
            break;
        }
        
        if (processed == 0) {
            break;  // 数据不足，等待更多
        }

        c->wlength += resp_len;
        total_processed += processed;
    }

    // 处理已处理的数据
    if (total_processed > 0) {
        if (total_processed < c->rlength) {
            memmove(c->rbuffer, c->rbuffer + total_processed, c->rlength - total_processed);
            c->rlength -= total_processed;
        } else {
            c->rlength = 0;
        }
    }

    // 如果有响应数据，注册写事件
    if (c->wlength > 0) {
        set_event(fd, EPOLLOUT, 0);
    }
    return count;
}

int send_cb(int fd) {
    struct conn *c = &conn_list[fd];

#if ENABLE_HTTP
    http_response(c);
#elif ENABLE_WEBSOCKET
    ws_response(c);
#elif ENABLE_KVSTORE
    kvs_response(c);
#endif

    int count = 0;
    if (c->wlength > 0) {
        count = send(fd, c->wbuffer, c->wlength, 0);
        if (count > 0) {
            // 移动未发送的数据到缓冲区开头
            if (count < c->wlength) {
                memmove(c->wbuffer, c->wbuffer + count, c->wlength - count);
                c->wlength -= count;
            } else {
                c->wlength = 0;
            }
        } else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            printf("send error on fd=%d: %s\n", fd, strerror(errno));
            close(fd);
            epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
            return -1;
        }
    }

    // 如果还有未发送数据，继续监听写事件；否则切回读事件
    if (c->wlength > 0) {
        set_event(fd, EPOLLOUT, 0);
    } else {
        set_event(fd, EPOLLIN, 0);
    }
    return count;
}

int r_init_server(unsigned short port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in servaddr;
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr*)&servaddr, sizeof(struct sockaddr)) < 0) {
        printf("bind failed: %s\n", strerror(errno));
        return -1;
    }
    listen(sockfd, 10);
#ifdef DEBUG
    printf("listen finished: %d\n", sockfd);
#endif
    return sockfd;
}

/* 供复制模块使用的读事件注册/注销接口 */
void event_register_read(int fd, int (*handler)(int)) {
    if (fd < 0 || fd >= CONNECTION_SIZE) {
        fprintf(stderr, "[EVENT] event_register_read: invalid fd %d\n", fd);
        return;
    }

    ensure_epfd();

    conn_list[fd].fd = fd;
    conn_list[fd].r_action.recv_callback = handler;
    conn_list[fd].send_callback = NULL;

    conn_list[fd].rbuffer = (char*)kvs_malloc(INIT_BUFFER_SIZE);
    conn_list[fd].rcapacity = INIT_BUFFER_SIZE;
    conn_list[fd].rlength = 0;
    memset(conn_list[fd].rbuffer, 0, INIT_BUFFER_SIZE);

    conn_list[fd].wbuffer = (char*)kvs_malloc(INIT_BUFFER_SIZE);
    conn_list[fd].wcapacity = INIT_BUFFER_SIZE;
    conn_list[fd].wlength = 0;
    memset(conn_list[fd].wbuffer, 0, INIT_BUFFER_SIZE);

    set_event(fd, EPOLLIN, 1);
#ifdef DEBUG
    printf("[EVENT] Registered read event on fd=%d, handler=%p\n", fd, handler);
#endif
}

void event_unregister_read(int fd) {
    if (fd < 0 || fd >= CONNECTION_SIZE) return;

    ensure_epfd();

    if (epfd > 0) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    }

    // 释放动态分配的缓冲区
    if (conn_list[fd].rbuffer) {
        kvs_free(conn_list[fd].rbuffer);
        conn_list[fd].rbuffer = NULL;
    }
    if (conn_list[fd].wbuffer) {
        kvs_free(conn_list[fd].wbuffer);
        conn_list[fd].wbuffer = NULL;
    }

    conn_list[fd].fd = -1;
    conn_list[fd].r_action.recv_callback = NULL;
    conn_list[fd].send_callback = NULL;

#ifdef DEBUG
    printf("[EVENT] Unregistered read event on fd=%d\n", fd);
#endif
}

/* 定时器回调 */
static int timer_cb(int fd) {
    uint64_t exp;
    ssize_t s = read(fd, &exp, sizeof(exp));
    if (s != sizeof(exp)) {
        // ignore
    }
    kvs_aof_check_and_rewrite();
    kvs_rdb_check_and_save();
    return 0;
}

int reactor_start(unsigned short port, msg_handler handler) {
    signal(SIGPIPE, SIG_IGN);

    kvs_handler = handler;
    ensure_epfd();

    int i;
    for (i = 0; i < MAX_PORTS; i++) {
        int sockfd = r_init_server(port + i);
        if (sockfd < 0) continue;
        conn_list[sockfd].fd = sockfd;
        conn_list[sockfd].r_action.recv_callback = accept_cb;
        set_event(sockfd, EPOLLIN, 1);
    }

    // 创建定时器
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (tfd < 0) {
        perror("timerfd_create");
    } else {
        struct itimerspec its;
        its.it_interval.tv_sec = 1;
        its.it_interval.tv_nsec = 0;
        its.it_value.tv_sec = 1;
        its.it_value.tv_nsec = 0;
        if (timerfd_settime(tfd, 0, &its, NULL) == 0) {
            event_register_read(tfd, timer_cb);
#ifdef DEBUG
            printf("[EVENT] Timer fd=%d registered\n", tfd);
#endif
        } else {
            perror("timerfd_settime");
            close(tfd);
        }
    }

    gettimeofday(&begin, NULL);

    while (1) {
        struct epoll_event events[1024] = {0};
        int nready = epoll_wait(epfd, events, 1024, -1);

        for (i = 0; i < nready; i++) {
            int connfd = events[i].data.fd;
            if (events[i].events & EPOLLIN) {
                conn_list[connfd].r_action.recv_callback(connfd);
            }
            if (events[i].events & EPOLLOUT) {
                if (conn_list[connfd].send_callback)
                    conn_list[connfd].send_callback(connfd);
            }
        }
    }
    return 0;
}
