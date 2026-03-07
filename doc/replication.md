

## 一、内容概要

试想，如果你的服务器由于意外因素产生了宕机，而这个故障原因将需要相当一段时间才能找到并修复，何以最小化降低宕机所带来的损失呢？

最直接了当的方式是开一台仆从服务器，作为主服务器的“备份”，一旦主服务器下线，仆从服务器立即充当主服务器的角色继续服务客户端。

实现功能如下：

- 从主转换
-  全量同步
- 增量同步


## 二、使用方法

主机启动

```shell
./kvstore master端口
```
启动一台从机，启动时带--slaveof等参数，触发**握手流程**，成功后立即**从RDB文件中触发全量同步**。
```shell
./kvstore slave端口 --slaveof masterIP master端口
```
客户端向主机发送指令时，主**机向从机广播指令，实现增量同步**。

主机宕机，向从机手动**输入SLAVEOF NO ONE**指令，从机立即升格为主机

## 三、接口设计
kvs_replication.h：
- 除非使用kvs_slaveof()函数设置为从机，否则kvs_replication_init()函数供main函数中直接调用，默认为master角色。
- 网络层（即reactor）调用kvs_replication_accept_master()函数，**处理PSYNC握手请求**，分配fd，同时提供是否是普通的客户端请求的判断依据。
- 在KV存储核心功能模块中（kvstore.c）调用kvs_replication_add_slave()和kvs_replication_feed_slaves()函数，**以对从机进行全量同步和增量同步**。
```c
/* 初始化复制模块（默认角色为 MASTER） */
void kvs_replication_init(void);

/* 设置主从关系：SLAVEOF ip port 或 SLAVEOF NO ONE */
void kvs_slaveof(char *ip, int port);

/* Master：接受新 Slave 连接（已在 accept 后读取握手数据） */
int kvs_replication_accept_master(int fd);   // 返回 0 成功，-1 非 PSYNC

/* Master：添加一个 Slave 并立即进行全量同步 */
void kvs_replication_add_slave(int fd);

/* Master：广播写命令给所有已连接的 Slave */
void kvs_replication_feed_slaves(char *cmd, char *key, char *value);
```


## 四、关键功能实现

### 4.1 全局变量与从主转换功能
设计全局变量管理同步（复制）模块，从而初始化部分、添加从机、从机-主机角色转换部分都依赖于该全局变量的维护。 

```c
typedef struct {
    int role;                 // KVS_ROLE_MASTER / KVS_ROLE_SLAVE
    int master_fd;            // Slave：与 Master 的连接
    char master_ip[64];
    int master_port;
    int slave_fds[KVS_MAX_SLAVES]; // Master：所有 Slave 的连接
    int slave_count;
} kvs_replication_t;

extern kvs_replication_t g_repl;
```

进一步解释，请考虑如下函数，根据传入参数的不同，主机（初始化默认主机）可降级为从机，而从机可以晋升为master（从主转换）

```c
void kvs_slaveof(char *ip, int port) {
    /* 晋升为 Master (SLAVEOF NO ONE) */
    if (strcasecmp(ip, "NO") == 0) {
        if (g_repl.role == KVS_ROLE_SLAVE && g_repl.master_fd != -1) {
            event_unregister_read(g_repl.master_fd);
            close(g_repl.master_fd);
            g_repl.master_fd = -1;
        }
        g_repl.role = KVS_ROLE_MASTER;
        return;
    }

    /* 降级为 Slave */
    g_repl.role = KVS_ROLE_SLAVE;
    strncpy(g_repl.master_ip, ip, sizeof(g_repl.master_ip)-1);
    g_repl.master_ip[sizeof(g_repl.master_ip)-1] = '\0';
    g_repl.master_port = port;

    if (g_repl.master_fd != -1) {
        event_unregister_read(g_repl.master_fd);
        close(g_repl.master_fd);
        g_repl.master_fd = -1;
    }

    if (g_repl.master_fd == -1) {
        return;
    }
    send(g_repl.master_fd, "PSYNC\r\n", 7, 0);
    event_register_read(g_repl.master_fd, kvs_replication_handle_master_read);
}

```

### 4.2 PSYNC握手与全量同步功能
在网络层（reactor）调用kvs_replication_accept_master：

```c
int accept_cb(int fd) {
    struct sockaddr_in clientaddr;
    socklen_t len = sizeof(clientaddr);
    int clientfd = accept(fd, (struct sockaddr*)&clientaddr, &len);
    if (clientfd < 0) {
        printf("accept errno: %d --> %s\n", errno, strerror(errno));
        return -1;
    }
//这里开始是主从同步的握手逻辑
    int ret = kvs_replication_accept_master(clientfd);
    if (ret == 0) {
        printf("[ACCEPT] PSYNC handshake succeeded, fd=%d taken over by replication\n", clientfd);
    } else {
        event_register(clientfd, EPOLLIN);
        printf("[ACCEPT] Normal client connected, fd=%d\n", clientfd);
    }

    if ((clientfd % 1000) == 0) {
        struct timeval current;
        gettimeofday(&current, NULL);
        int time_used = TIME_SUB_MS(current, begin);
        memcpy(&begin, &current, sizeof(struct timeval));
        printf("accept finished: %d, time_used: %d\n", clientfd, time_used);
    }
    return 0;
}
```

```c
int kvs_replication_accept_master(int fd) {   // 返回值改为 int
    char buf[16];
    ssize_t n = recv(fd, buf, sizeof(buf)-1, MSG_PEEK);
    if (n <= 0) {
        close(fd);
        return -1;
    }
    buf[n] = '\0';
    if (strstr(buf, "PSYNC") == buf) {
        recv(fd, buf, strlen("PSYNC\r\n"), 0); // 消费握手数据
        kvs_replication_add_slave(fd);
        return 0;   // 成功接管
    }
    // 非 PSYNC 连接：不关闭，返回 -1 让上层处理
    return -1;
}
```
接受accept之后立即触发全量同步，遍历红黑树，并且以SET命令的形式发送到从机
```c
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
    kvs_replication_send_full_sync(fd); //触发全量恢复，依赖于红黑树的遍历
}
```
### 4.3 广播机制与增量同步功能

当客户端发送命令时，识别到非PSYNC连接，进普通命令处理的逻辑，但是现在多一条处理----调用kvs_replication_feed_slaves()。


```c
/* 命令传播 */
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
    if (len < 0 || len >= sizeof(buf)) {
        return;
    }
    for (int i = 0; i < g_repl.slave_count; i++) {
        int fd = g_repl.slave_fds[i];
        if (send(fd, buf, len, 0) < 0) {
            close(fd);
            g_repl.slave_fds[i] = g_repl.slave_fds[--g_repl.slave_count];
            i--;
        } else {
            printf("[REPL] feed_slaves: command sent to fd=%d\n", fd);
        }
    }
}

```
命令执行器现在是这样：
```c
int kvs_executor(char **tokens, int count, char *response) {
    if (!tokens || !tokens[0] || count < 1 || !response) return -1;

    int cmd;
    for (cmd = 0; cmd < CMD_COUNT; cmd++)
        if (strcmp(tokens[0], command[cmd]) == 0) break;
    if (cmd >= CMD_COUNT)
        return sprintf(response, "UNKNOWN COMMAND\r\n");

    char *key = count > 1 ? tokens[1] : NULL;
    char *val = count > 2 ? tokens[2] : NULL;
    int ret, len = 0;

    switch (cmd) {
    case CMD_RSET:
        ret = kvs_rbtree_set(&global_rbtree, key, val);
        if (ret < 0)      len = sprintf(response, "ERROR\r\n");
        else if (ret == 0) {
            len = sprintf(response, "OK\r\n");
            kvs_aof_append("RSET", key, val);
            if (g_repl.role == KVS_ROLE_MASTER)
                kvs_replication_feed_slaves("SET", key, val);
        } else            len = sprintf(response, "EXIST\r\n");
        break;
    case CMD_RGET: {
        char *res = kvs_rbtree_get(&global_rbtree, key);
        len = res ? sprintf(response, "%s\r\n", res) :
                    sprintf(response, "NO EXIST\r\n");
        break;
    }
    case CMD_RDEL:
        ret = kvs_rbtree_del(&global_rbtree, key);
        if (ret < 0)      len = sprintf(response, "ERROR\r\n");
        else if (ret == 0) {
            len = sprintf(response, "OK\r\n");
            kvs_aof_append("RDEL", key, NULL);
            if (g_repl.role == KVS_ROLE_MASTER)
                kvs_replication_feed_slaves("DEL", key, NULL);
        } else            len = sprintf(response, "NO EXIST\r\n");
        break;
    case CMD_RMOD:
        ret = kvs_rbtree_mod(&global_rbtree, key, val);
        if (ret < 0)      len = sprintf(response, "ERROR\r\n");
        else if (ret == 0) {
            len = sprintf(response, "OK\r\n");
            kvs_aof_append("RMOD", key, val);
            if (g_repl.role == KVS_ROLE_MASTER)
                kvs_replication_feed_slaves("MOD", key, val);
        } else            len = sprintf(response, "NO EXIST\r\n");
        break;
    case CMD_REXIST:
        ret = kvs_rbtree_exist(&global_rbtree, key);
        len = (ret == 0) ? sprintf(response, "EXIST\r\n") :
                           sprintf(response, "NO EXIST\r\n");
        break;
    case CMD_SAVE:
        kvs_rdb_save();
        len = sprintf(response, "OK\r\n");
        break;
    }
    return len;
}
```

