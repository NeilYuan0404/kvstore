#ifndef __KVS_PERSIST_H__
#define __KVS_PERSIST_H__

#include "kvs_hash.h"
#include <stdbool.h>
#include <time.h>

typedef struct {
    time_t last_save_time;
    int dirty;
} persist_runtime_t;

extern bool g_is_loading;
extern persist_runtime_t g_persist_runtime;

void kvs_persist_init(void);
void kvs_aof_append(const char *cmd, const char *key, const char *value);
void load_aof_file(const char *filename);
void kvs_rdb_save(void);
void kvs_rdb_check_and_save(void);
void kvs_aof_check_and_rewrite(void);

#endif