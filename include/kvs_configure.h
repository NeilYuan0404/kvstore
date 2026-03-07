#ifndef KVS_CONFIGURE_H
#define KVS_CONFIGURE_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    LOG_LEVEL_INFO = 1,
    LOG_LEVEL_WARN = 2,
    LOG_LEVEL_DEBUG = 3
} log_level_t;

typedef enum {
    PERSIST_OFF = 0,
    PERSIST_AOF_ONLY = 1,
    PERSIST_RDB_ONLY = 2,
    PERSIST_MIXED = 3
} persist_mode_t;

typedef enum {
    ROLE_MASTER = 0,
    ROLE_SLAVE = 1
} server_role_t;

typedef enum {
    REPL_OFF = 0,
    REPL_ON = 1
} repl_switch_t;

typedef struct {
    int port;
    log_level_t log_level;

    persist_mode_t persist_mode;
    char rdb_file[256];
    int rdb_save_interval;
    int rdb_min_changes;
    bool rdb_save_on_shutdown;
    char aof_file[256];
    int aof_rewrite_size;
    bool aof_auto_rewrite;

    repl_switch_t repl_switch;
    server_role_t repl_role;
    char master_ip[64];
    int master_port;
} kvs_config_t;

extern kvs_config_t g_config;

void kvs_config_set_default(void);
int kvs_config_load(const char *filename);
const char* kvs_config_find(void);
void kvs_config_print(void);
void kvs_log(log_level_t level, const char *format, ...);

#define LOG_INFO(fmt, ...)   kvs_log(LOG_LEVEL_INFO, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)   kvs_log(LOG_LEVEL_WARN, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...)  kvs_log(LOG_LEVEL_DEBUG, fmt, ##__VA_ARGS__)

#endif