#ifndef KVS_HASH_H
#define KVS_HASH_H

#include <pthread.h>
#include <stddef.h>

#define MAX_KEY_LEN     128
#define MAX_VALUE_LEN   512
#define MAX_TABLE_SIZE  65536

typedef struct hashnode_s {
    void *key;
    void *value;
    size_t key_len;
    size_t value_len;
    struct hashnode_s *next;
} hashnode_t;

typedef struct hashtable_s {
    hashnode_t **nodes;
    int max_slots;
    int count;
} kvs_hash_t;

int  kvs_hash_create(kvs_hash_t *T);
void kvs_hash_destroy(kvs_hash_t *T);
int  kvs_hash_set(kvs_hash_t *T, const void *key, size_t key_len, const void *val, size_t val_len);
void *kvs_hash_get(kvs_hash_t *T, const void *key, size_t key_len, size_t *val_len);
int  kvs_hash_del(kvs_hash_t *T, const void *key, size_t key_len);
int  kvs_hash_mod(kvs_hash_t *T, const void *key, size_t key_len, const void *val, size_t val_len);
int  kvs_hash_exist(kvs_hash_t *T, const void *key, size_t key_len);

void kvs_hash_foreach(kvs_hash_t *T,
                      void (*cb)(const void *key, size_t key_len, const void *val, size_t val_len, void *arg),
                      void *arg);

int kvs_hash_save(kvs_hash_t *hash, const char *filename);
int kvs_hash_load_rdb(kvs_hash_t *hash, const char *filename);

#endif