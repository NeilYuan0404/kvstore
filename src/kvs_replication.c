#include "../include/kvs_replication.h"
#include "../include/kvs_hash.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

extern kvs_hash_t global_hash;

extern void event_register_read(int fd, int (*handler)(int));
extern void event_unregister_read(int fd);

kvs_replication_t g_repl = {
    .role = KVS_ROLE_MASTER,
    .master_fd = -1,
    .slave_count = 0
};

int kvs_replication_handle_master_read(int fd);
static void kvs_replication_send_full_sync(int slave_fd);
static void kvs_replication_send_key_value(const void *key, size_t key_len, 
                                          const void *val, size_t val_len, void *arg);
static void kvs_replication_execute_command(const char *cmdline);
static void kvs_replication_reconnect(void);
static int  kvs_connect_master(const char *ip, int port);

void kvs_replication_init(void) {
    g_repl.role = KVS_ROLE_MASTER;
    g_repl.master_fd = -1;
    g_repl.slave_count = 0;
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
    kvs_replication_send_full_sync(fd);
}

/* 全量同步回调：发送单个键值对（二进制安全） */
static void kvs_replication_send_key_value(const void *key, size_t key_len, 
                                          const void *val, size_t val_len, void *arg) {
    int fd = *(int*)arg;
    char buf[2048];
    int pos = 0;
    
    // 构造 *3\r\n$3\r\nSET\r\n
    pos += snprintf(buf + pos, sizeof(buf) - pos, "*3\r\n$3\r\nSET\r\n");
    
    // 构造 $<key_len>\r\n<key>\r\n
    pos += snprintf(buf + pos, sizeof(buf) - pos, "$%zu\r\n", key_len);
    if (pos + key_len + 2 <= sizeof(buf)) {
        memcpy(buf + pos, key, key_len);
        pos += key_len;
        buf[pos++] = '\r';
        buf[pos++] = '\n';
    }
    
    // 构造 $<val_len>\r\n<val>\r\n
    pos += snprintf(buf + pos, sizeof(buf) - pos, "$%zu\r\n", val_len);
    if (pos + val_len + 2 <= sizeof(buf)) {
        memcpy(buf + pos, val, val_len);
        pos += val_len;
        buf[pos++] = '\r';
        buf[pos++] = '\n';
    }
    
    if (pos > 0 && pos < (int)sizeof(buf)) {
        send(fd, buf, pos, 0);
    }
}

/* 全量同步：遍历哈希表并发送 SET 命令 */
static void kvs_replication_send_full_sync(int slave_fd) {
    send(slave_fd, "+FULLSYNC\r\n", 12, 0);
    kvs_hash_foreach(&global_hash, 
                     (void (*)(const void *, size_t, const void *, size_t, void *))kvs_replication_send_key_value, 
                     &slave_fd);
    send(slave_fd, "+OK\r\n", 5, 0);
}

/* 命令传播 - 保持原有格式，使用文本协议 */
void kvs_replication_feed_slaves(char *cmd, char *key, char *value) {
    if (g_repl.role != KVS_ROLE_MASTER) {
        return;
    }
    if (g_repl.slave_count == 0) {
        return;
    }
    char buf[2048];
    int len;
    if (value == NULL)
        len = snprintf(buf, sizeof(buf), "%s %s\r\n", cmd, key);
    else
        len = snprintf(buf, sizeof(buf), "%s %s %s\r\n", cmd, key, value);
    if (len < 0 || (size_t)len >= sizeof(buf)) {
        return;
    }
    for (int i = 0; i < g_repl.slave_count; i++) {
        int fd = g_repl.slave_fds[i];
        if (send(fd, buf, len, 0) < 0) {
            close(fd);
            g_repl.slave_fds[i] = g_repl.slave_fds[--g_repl.slave_count];
            i--;
        }
    }
}

static int kvs_connect_master(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

void kvs_slaveof(char *ip, int port) {
    if (strcasecmp(ip, "NO") == 0) {
        if (g_repl.role == KVS_ROLE_SLAVE && g_repl.master_fd != -1) {
            event_unregister_read(g_repl.master_fd);
            close(g_repl.master_fd);
            g_repl.master_fd = -1;
        }
        g_repl.role = KVS_ROLE_MASTER;
        return;
    }

    g_repl.role = KVS_ROLE_SLAVE;
    strncpy(g_repl.master_ip, ip, sizeof(g_repl.master_ip)-1);
    g_repl.master_ip[sizeof(g_repl.master_ip)-1] = '\0';
    g_repl.master_port = port;

    if (g_repl.master_fd != -1) {
        event_unregister_read(g_repl.master_fd);
        close(g_repl.master_fd);
        g_repl.master_fd = -1;
    }

    g_repl.master_fd = kvs_connect_master(ip, port);
    if (g_repl.master_fd == -1) {
        return;
    }
    send(g_repl.master_fd, "PSYNC\r\n", 7, 0);
    event_register_read(g_repl.master_fd, kvs_replication_handle_master_read);
}

int kvs_replication_handle_master_read(int fd) {
    if (fd != g_repl.master_fd) {
        return -1;
    }
    char buf[4096];
    ssize_t n = recv(fd, buf, sizeof(buf)-1, 0);
    if (n <= 0) {
        event_unregister_read(fd);
        close(fd);
        g_repl.master_fd = -1;
        kvs_replication_reconnect();
        return -1;
    }
    buf[n] = '\0';
    char *saveptr;
    char *line = strtok_r(buf, "\r\n", &saveptr);
    while (line) {
        if (line[0] != '+') {
            kvs_replication_execute_command(line);
        }
        line = strtok_r(NULL, "\r\n", &saveptr);
    }
    return 0;
}

/* 从机执行写命令（支持二进制安全） */
static void kvs_replication_execute_command(const char *cmdline) {
    char copy[256];
    strncpy(copy, cmdline, sizeof(copy)-1);
    copy[sizeof(copy)-1] = '\0';
    char *cmd = strtok(copy, " ");
    char *key = strtok(NULL, " ");
    char *val = strtok(NULL, " ");
    if (!cmd || !key) {
        return;
    }
    if (strcasecmp(cmd, "SET") == 0) {
        kvs_hash_set(&global_hash, key, strlen(key), val ? val : "", val ? strlen(val) : 0);
    } else if (strcasecmp(cmd, "DEL") == 0) {
        kvs_hash_del(&global_hash, key, strlen(key));
    }
}

static void kvs_replication_reconnect(void) {
    if (g_repl.role != KVS_ROLE_SLAVE) {
        return;
    }
    if (strlen(g_repl.master_ip) == 0 || g_repl.master_port == 0) {
        return;
    }
    kvs_slaveof(g_repl.master_ip, g_repl.master_port);
}
