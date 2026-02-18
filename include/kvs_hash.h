#ifndef KVS_HASH_H
#define KVS_HASH_H

#include <pthread.h>

#define MAX_KEY_LEN     128
#define MAX_VALUE_LEN   512
#define MAX_TABLE_SIZE  65536

typedef struct hashnode_s {
#if ENABLE_KEY_POINTER
    char *key;
    char *value;
#else
    char key[MAX_KEY_LEN];
    char value[MAX_VALUE_LEN];
#endif
    struct hashnode_s *next;
} hashnode_t;

typedef struct hashtable_s {
    hashnode_t **nodes;
    int max_slots;
    int count;
} kvs_hash_t;

/* operation */
int  kvs_hash_create(kvs_hash_t *T);
void kvs_hash_destory(kvs_hash_t *T);
int  kvs_hash_set(kvs_hash_t *T, char *key, char *value);
char *kvs_hash_get(kvs_hash_t *T, char *key);
int  kvs_hash_del(kvs_hash_t *T, char *key);
int  kvs_hash_mod(kvs_hash_t *T, char *key, char *value);
int  kvs_hash_exist(kvs_hash_t *T, char *key);

/* replication recover */
void kvs_hash_foreach(kvs_hash_t *T,
                      void (*cb)(const char *key, const char *value, void *arg),
                      void *arg);


int kvs_hash_save(kvs_hash_t *hash, const char *filename);

/* RDB persist support interface */
int kvs_hash_load_rdb(kvs_hash_t *hash, const char *filename);

#endif
