#include "../include/kvs_base.h"
#include "../include/kvs_replication.h"
#include "../include/kvs_hash.h"
#include "../include/kvs_configure.h"
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

extern kvs_hash_t global_hash;
extern void event_register_read(int fd, int (*handler)(int));
extern void event_unregister_read(int fd);
extern int kvs_protocol(char *msg, int length, char *response, int resp_size, int *processed, int *needed);

kvs_replication_t g_repl = {
    .role = KVS_ROLE_MASTER,
    .master_fd = -1,
    .slave_count = 0
};

typedef struct slave_buffer {
    char data[8192];
    int len;
} slave_buffer_t;
static slave_buffer_t slave_buf = {0};

static void kvs_replication_send_full_sync(int slave_fd);
static void kvs_replication_send_key_value(const void *key, size_t key_len,
                                          const void *val, size_t val_len, void *arg);
static void kvs_replication_reconnect(void);
static int kvs_connect_master(const char *ip, int port);

void kvs_replication_init(void) {
    if (g_config.repl_switch == REPL_ON && g_config.repl_role == ROLE_SLAVE) {
        kvs_slaveof(g_config.master_ip, g_config.master_port);
    } else {
        g_repl.role = KVS_ROLE_MASTER;
        g_repl.master_fd = -1;
        g_repl.slave_count = 0;
        memset(g_repl.slave_fds, 0, sizeof(g_repl.slave_fds));
        LOG_INFO("[REPL] Initialized as MASTER\n");
    }
}

int kvs_replication_accept_master(int fd) {
    char buf[16];
    ssize_t n = recv(fd, buf, sizeof(buf)-1, MSG_PEEK);
    if (n <= 0) {
        close(fd);
        return -1;
    }
    buf[n] = '\0';
    if (strstr(buf, "PSYNC") == buf) {
        recv(fd, buf, strlen("PSYNC\r\n"), 0);
        kvs_replication_add_slave(fd);
        return 0;
    }
    return -1;
}

void kvs_replication_add_slave(int fd) {
    if (g_repl.role != KVS_ROLE_MASTER) {
        close(fd);
        return;
    }
    if (g_repl.slave_count >= KVS_MAX_SLAVES) {
        close(fd);
        return;
    }
    for (int i = 0; i < g_repl.slave_count; i++) {
        if (g_repl.slave_fds[i] == fd) {
            close(fd);
            return;
        }
    }
    g_repl.slave_fds[g_repl.slave_count++] = fd;
    LOG_INFO("[REPL] Slave added, fd=%d, total=%d\n", fd, g_repl.slave_count);
    kvs_replication_send_full_sync(fd);
}

static void kvs_replication_send_key_value(const void *key, size_t key_len,
                                          const void *val, size_t val_len, void *arg) {
    int fd = *(int*)arg;
    char buf[8192];
    int pos = 0;

    pos += snprintf(buf + pos, sizeof(buf) - pos, "*3\r\n$3\r\nSET\r\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "$%zu\r\n", key_len);
    if (pos + key_len + 2 <= sizeof(buf)) {
        memcpy(buf + pos, key, key_len);
        pos += key_len;
        buf[pos++] = '\r';
        buf[pos++] = '\n';
    } else {
        LOG_DEBUG("[REPL] Key too large for buffer\n");
        return;
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos, "$%zu\r\n", val_len);
    if (pos + val_len + 2 <= sizeof(buf)) {
        memcpy(buf + pos, val, val_len);
        pos += val_len;
        buf[pos++] = '\r';
        buf[pos++] = '\n';
    } else {
        LOG_DEBUG("[REPL] Value too large for buffer\n");
        return;
    }

    if (send(fd, buf, pos, 0) < 0) {
        LOG_DEBUG("[REPL] Failed to send key-value to fd=%d\n", fd);
    }
}

static void kvs_replication_send_full_sync(int slave_fd) {
    LOG_INFO("[REPL] Starting full sync for fd=%d\n", slave_fd);

    if (send(slave_fd, "+FULLSYNC\r\n", 12, 0) < 0) {
        LOG_DEBUG("[REPL] Failed to send FULLSYNC marker\n");
        return;
    }

    kvs_hash_foreach(&global_hash,
                     (void (*)(const void *, size_t, const void *, size_t, void *))kvs_replication_send_key_value,
                     &slave_fd);

    if (send(slave_fd, "+OK\r\n", 5, 0) < 0) {
        LOG_DEBUG("[REPL] Failed to send OK marker\n");
    }

    LOG_INFO("[REPL] Full sync completed for fd=%d\n", slave_fd);
}

void kvs_replication_feed_slaves(char *cmd, char *key, char *value) {
    if (g_repl.role != KVS_ROLE_MASTER) return;
    if (g_repl.slave_count == 0) return;

    char buf[8192];
    int pos = 0;
    size_t key_len = strlen(key);
    size_t val_len = value ? strlen(value) : 0;

    if (value == NULL) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "*2\r\n");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "$%zu\r\n%s\r\n", strlen(cmd), cmd);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "$%zu\r\n%s\r\n", key_len, key);
    } else {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "*3\r\n");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "$%zu\r\n%s\r\n", strlen(cmd), cmd);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "$%zu\r\n%s\r\n", key_len, key);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "$%zu\r\n%s\r\n", val_len, value);
    }

    if (pos <= 0 || (size_t)pos >= sizeof(buf)) return;

    LOG_DEBUG("[REPL] Feeding %d slaves: %.*s", g_repl.slave_count, pos, buf);

    for (int i = 0; i < g_repl.slave_count; i++) {
        int fd = g_repl.slave_fds[i];
        if (send(fd, buf, pos, 0) < 0) {
            LOG_DEBUG("[REPL] Slave fd=%d disconnected\n", fd);
            close(fd);
            g_repl.slave_fds[i] = g_repl.slave_fds[--g_repl.slave_count];
            i--;
        }
    }
}

static int kvs_connect_master(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);

    int ret = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }

    return fd;
}

int kvs_replication_handle_master_read(int fd) {
    if (fd != g_repl.master_fd) return -1;

    char buf[4096];
    ssize_t n = recv(fd, buf, sizeof(buf)-1, 0);
    if (n <= 0) {
        LOG_INFO("[REPL] Master connection closed\n");
        event_unregister_read(fd);
        close(fd);
        g_repl.master_fd = -1;
        kvs_replication_reconnect();
        return -1;
    }

    LOG_DEBUG("[REPL] Received %ld bytes from master\n", n);

    if (slave_buf.len + n > (ssize_t)sizeof(slave_buf.data)) {
        LOG_DEBUG("[REPL] Slave buffer full, resetting\n");
        slave_buf.len = 0;
    }

    memcpy(slave_buf.data + slave_buf.len, buf, n);
    slave_buf.len += n;

    int processed = 0;
    int needed = 0;
    char dummy[1];

    while (slave_buf.len > 0) {
        int ret = kvs_protocol(slave_buf.data, slave_buf.len, dummy, 0, &processed, &needed);

        if (ret == -2) {
            LOG_DEBUG("[REPL] Warning: Response too large (%d bytes)\n", needed);
            break;
        }
        if (ret < 0) {
            LOG_DEBUG("[REPL] Protocol error, resetting buffer\n");
            slave_buf.len = 0;
            break;
        }
        if (processed == 0) break;

        if (processed < slave_buf.len) {
            memmove(slave_buf.data, slave_buf.data + processed, slave_buf.len - processed);
            slave_buf.len -= processed;
        } else {
            slave_buf.len = 0;
        }
    }

    return 0;
}

void kvs_slaveof(char *ip, int port) {
    if (strcasecmp(ip, "NO") == 0) {
        if (g_repl.role == KVS_ROLE_SLAVE && g_repl.master_fd != -1) {
            event_unregister_read(g_repl.master_fd);
            close(g_repl.master_fd);
            g_repl.master_fd = -1;
        }
        g_repl.role = KVS_ROLE_MASTER;
        LOG_INFO("[REPL] Switched to MASTER\n");
        return;
    }

    LOG_INFO("[REPL] Connecting to master %s:%d\n", ip, port);

    g_repl.role = KVS_ROLE_SLAVE;

    size_t len = strlen(ip);
    if (len >= sizeof(g_repl.master_ip)) {
        len = sizeof(g_repl.master_ip) - 1;
    }
    memmove(g_repl.master_ip, ip, len);
    g_repl.master_ip[len] = '\0';
    g_repl.master_port = port;

    if (g_repl.master_fd != -1) {
        event_unregister_read(g_repl.master_fd);
        close(g_repl.master_fd);
        g_repl.master_fd = -1;
    }

    g_repl.master_fd = kvs_connect_master(ip, port);
    if (g_repl.master_fd == -1) {
        LOG_DEBUG("[REPL] Failed to connect to master\n");
        return;
    }

    send(g_repl.master_fd, "PSYNC\r\n", 7, 0);
    event_register_read(g_repl.master_fd, kvs_replication_handle_master_read);
    slave_buf.len = 0;

    LOG_INFO("[REPL] Connected to master, fd=%d\n", g_repl.master_fd);
}

static void kvs_replication_reconnect(void) {
    if (g_repl.role != KVS_ROLE_SLAVE) return;
    if (strlen(g_repl.master_ip) == 0 || g_repl.master_port == 0) return;

    LOG_INFO("[REPL] Reconnecting to master %s:%d\n", g_repl.master_ip, g_repl.master_port);

    char ip_copy[64];
    strncpy(ip_copy, g_repl.master_ip, sizeof(ip_copy) - 1);
    ip_copy[sizeof(ip_copy) - 1] = '\0';

    kvs_slaveof(ip_copy, g_repl.master_port);
}