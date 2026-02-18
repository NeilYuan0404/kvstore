#ifndef __KVS_REPLICATION_H__
#define __KVS_REPLICATION_H__

#define KVS_MAX_SLAVES 128
#define KVS_ROLE_MASTER 0
#define KVS_ROLE_SLAVE  1

typedef struct {
    int role;
    int master_fd;
    char master_ip[64];
    int master_port;
    int slave_fds[KVS_MAX_SLAVES];
    int slave_count;
} kvs_replication_t;

extern kvs_replication_t g_repl;

void kvs_replication_init(void);
void kvs_slaveof(char *ip, int port);
int kvs_replication_accept_master(int fd);
void kvs_replication_add_slave(int fd);
void kvs_replication_feed_slaves(char *cmd, char *key, char *value);

#endif
