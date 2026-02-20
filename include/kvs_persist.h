#ifndef __KVS_PERSIST_H__
#define __KVS_PERSIST_H__

#include "kvs_hash.h"
#include <stdbool.h>

// extern int kvs_protocol(char *msg, int length, char *response, int *processed);

extern int kvs_protocol(char *msg, int length, char *response, int resp_size, int *processed, int *needed);

/* configuration of aof and rdb */
typedef struct {
    struct {
        char filename[256];
        int save_interval;    /* in seconds */
        int min_changes;
        bool save_on_shutdown;
        time_t last_save_time;   /* timestamp of last RDB save */
        int dirty;                /* number of write operations since last save */
    } rdb;

    struct {
        char filename[256];
        size_t rewrite_size;
        bool auto_rewrite;
    } aof;
} persist_config_t;

extern bool g_is_loading;
extern persist_config_t g_persist_config;
extern kvs_hash_t global_hash;

#define AOF_FILE g_persist_config.aof.filename
#define RDB_FILE g_persist_config.rdb.filename

void kvs_persist_init(const char *aof_filename, const char *rdb_filename);
void kvs_aof_append(const char *cmd, const char *key, const char *value);
void load_aof_file(const char *filename);
void kvs_rdb_save(void);
int kvs_hash_save(kvs_hash_t *hash, const char *filename);
int kvs_hash_load(kvs_hash_t *hash, const char *filename);

/* RDB snapshot check and trigger */
void kvs_rdb_check_and_save(void);
//void kvs_rdb_notify_write(void);  /* to be called on each write operation */
void kvs_aof_check_and_rewrite(void);
#endif
