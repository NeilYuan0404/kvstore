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
#include <sys/timerfd.h>           // 新增：用于定时器

#include "../include/server.h"
#include "../include/kvs_replication.h"
#include "../include/kvs_persist.h" // 新增：用于 AOF 重写检查


#define CONNECTION_SIZE			1024
#define MAX_PORTS			1
#define TIME_SUB_MS(tv1, tv2)  ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)

static struct conn conn_list[CONNECTION_SIZE] = {0};

#if ENABLE_KVSTORE
typedef int (*msg_handler)(char *msg, int length, char *response, int *processed);
static msg_handler kvs_handler;

int kvs_request(struct conn *c) {
    int processed = 0;
    c->wlength = kvs_handler(c->rbuffer, c->rlength, c->wbuffer, &processed);
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
static int timer_cb(int fd);      // 新增：定时器回调声明

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

int event_register(int fd, int event) {
    if (fd < 0) return -1;

    conn_list[fd].fd = fd;
    conn_list[fd].r_action.recv_callback = recv_cb;
    conn_list[fd].send_callback = send_cb;

    memset(conn_list[fd].rbuffer, 0, BUFFER_LENGTH);
    conn_list[fd].rlength = 0;
    memset(conn_list[fd].wbuffer, 0, BUFFER_LENGTH);
    conn_list[fd].wlength = 0;

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
    int cur_len = conn_list[fd].rlength;
    int remaining = BUFFER_LENGTH - cur_len;
    if (remaining <= 0) {
        printf("Buffer full, closing fd=%d\n", fd);
        close(fd);
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
        return -1;
    }

    int count = recv(fd, conn_list[fd].rbuffer + cur_len, remaining, 0);
    if (count == 0) {
#ifdef DEBUG
        printf("client disconnect: %d\n", fd);
#endif
        close(fd);
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
        return 0;
    } else if (count < 0) {
        printf("recv error: %d, %s\n", errno, strerror(errno));
        close(fd);
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
        return -1;
    }

    cur_len += count;
    conn_list[fd].rlength = cur_len;

    int total_processed = 0;
    while (1) {
        int processed = 0;
        int resp_len = kvs_handler(conn_list[fd].rbuffer + total_processed,
                                    cur_len - total_processed,
                                    conn_list[fd].wbuffer,
                                    &processed);
        if (resp_len < 0) {
            // 修复：去掉多余的一层 if
            printf("[ERROR] Protocol error on fd=%d, resetting buffer\n", fd);
            conn_list[fd].rlength = 0;
            break;
        }
        if (processed == 0) {
            break;
        }
        conn_list[fd].wlength = resp_len;
        set_event(fd, EPOLLOUT, 0);
        total_processed += processed;
    }

    if (total_processed > 0) {
        if (total_processed < cur_len) {
            memmove(conn_list[fd].rbuffer,
                    conn_list[fd].rbuffer + total_processed,
                    cur_len - total_processed);
            conn_list[fd].rlength = cur_len - total_processed;
        } else {
            conn_list[fd].rlength = 0;
        }
    }
    return count;
}

int send_cb(int fd) {
#if ENABLE_HTTP
    http_response(&conn_list[fd]);
#elif ENABLE_WEBSOCKET
    ws_response(&conn_list[fd]);
#elif ENABLE_KVSTORE
    kvs_response(&conn_list[fd]);
#endif

    int count = 0;
    if (conn_list[fd].wlength != 0) {
        count = send(fd, conn_list[fd].wbuffer, conn_list[fd].wlength, 0);
    }
    set_event(fd, EPOLLIN, 0);
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

/*--------------------------------------------------------------------------
 * 供复制模块使用的读事件注册/注销接口
 *--------------------------------------------------------------------------*/
void event_register_read(int fd, int (*handler)(int)) {
    if (fd < 0 || fd >= CONNECTION_SIZE) {
        fprintf(stderr, "[EVENT] event_register_read: invalid fd %d\n", fd);
        return;
    }

    ensure_epfd();

    conn_list[fd].fd = fd;
    conn_list[fd].r_action.recv_callback = handler;
    conn_list[fd].send_callback = NULL;

    memset(conn_list[fd].rbuffer, 0, BUFFER_LENGTH);
    conn_list[fd].rlength = 0;
    memset(conn_list[fd].wbuffer, 0, BUFFER_LENGTH);
    conn_list[fd].wlength = 0;

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

    conn_list[fd].fd = -1;
    conn_list[fd].r_action.recv_callback = NULL;
    conn_list[fd].send_callback = NULL;

#ifdef DEBUG
    printf("[EVENT] Unregistered read event on fd=%d\n", fd);
#endif
}

/*--------------------------------------------------------------------------
 * 新增：定时器回调函数，每秒触发一次，检查 AOF 是否需要重写
 *--------------------------------------------------------------------------*/
static int timer_cb(int fd) {
    uint64_t exp;
    // 读取定时器事件，必须消耗掉，否则会一直触发
    ssize_t s = read(fd, &exp, sizeof(exp));
    if (s != sizeof(exp)) {
        // 可能出错，忽略
    }
    // 调用持久化模块的检查函数
    kvs_aof_check_and_rewrite();
    kvs_rdb_check_and_save();
    return 0;
}

int reactor_start(unsigned short port, msg_handler handler) {
    // 忽略 SIGPIPE，防止向已关闭的 socket 写入时进程退出
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

    // 创建定时器，用于定期检查 AOF 重写
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (tfd < 0) {
        perror("timerfd_create");
    } else {
        struct itimerspec its;
        its.it_interval.tv_sec = 1;    // 间隔 1 秒
        its.it_interval.tv_nsec = 0;
        its.it_value.tv_sec = 1;        // 第一次触发在 1 秒后
        its.it_value.tv_nsec = 0;
        if (timerfd_settime(tfd, 0, &its, NULL) == 0) {
            // 将定时器 fd 加入 epoll 监听
            event_register_read(tfd, timer_cb);
#ifdef DEBUG
            printf("[EVENT] Timer fd=%d registered for AOF rewrite check\n", tfd);
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
