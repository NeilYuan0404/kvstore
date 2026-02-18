#include "../include/kvstore.h"   
#include "../include/kvs_hash.h"
#include "../include/kvs_base.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

kvs_hash_t global_hash;

static int _hash(char *key, int size) {
    if (!key) return -1;
    int sum = 0;
    for (int i = 0; key[i]; i++) sum += key[i];
    return sum % size;
}

static hashnode_t *_create_node(char *key, char *value) {
    hashnode_t *node = (hashnode_t*)kvs_malloc(sizeof(hashnode_t));
    if (!node) return NULL;

#if ENABLE_KEY_POINTER
    char *kcopy = kvs_malloc(strlen(key) + 1);
    if (!kcopy) { kvs_free(node); return NULL; }
    strcpy(kcopy, key);

    char *vcopy = kvs_malloc(strlen(value) + 1);
    if (!vcopy) {
        kvs_free(kcopy);
        kvs_free(node);
        return NULL;
    }
    strcpy(vcopy, value);

    node->key = kcopy;
    node->value = vcopy;
#else
    strncpy(node->key, key, MAX_KEY_LEN - 1);
    node->key[MAX_KEY_LEN - 1] = '\0';
    strncpy(node->value, value, MAX_VALUE_LEN - 1);
    node->value[MAX_VALUE_LEN - 1] = '\0';
#endif
    node->next = NULL;
    return node;
}

int kvs_hash_create(kvs_hash_t *hash) {
    if (!hash) return -1;
    hash->nodes = (hashnode_t**)kvs_malloc(sizeof(hashnode_t*) * MAX_TABLE_SIZE);
    if (!hash->nodes) return -1;
    for (int i = 0; i < MAX_TABLE_SIZE; i++) hash->nodes[i] = NULL;
    hash->max_slots = MAX_TABLE_SIZE;
    hash->count = 0;
    return 0;
}

void kvs_hash_destory(kvs_hash_t *hash) {
    if (!hash || !hash->nodes) return;
    for (int i = 0; i < hash->max_slots; i++) {
        hashnode_t *node = hash->nodes[i];
        while (node) {
            hashnode_t *tmp = node;
            node = node->next;
#if ENABLE_KEY_POINTER
            kvs_free(tmp->key);
            kvs_free(tmp->value);
#endif
            kvs_free(tmp);
        }
    }
    kvs_free(hash->nodes);
    hash->nodes = NULL;
    hash->count = 0;
}

int kvs_hash_set(kvs_hash_t *hash, char *key, char *value) {
    if (!hash || !key || !value) return -1;
    if (!hash->nodes) {
        printf("[ERROR] hash->nodes is NULL\n");
        return -1;
    }
    if (hash->max_slots <= 0) {
        printf("[ERROR] invalid max_slots: %d\n", hash->max_slots);
        return -1;
    }
    
    int idx = _hash(key, hash->max_slots);
    if (idx < 0 || idx >= hash->max_slots) {
        printf("[ERROR] invalid hash index: %d\n", idx);
        return -1;
    }

    
    hashnode_t *node = hash->nodes[idx];
    while (node) {
        if (strcmp(node->key, key) == 0) {
            return 1;
        }
        node = node->next;
    }

    hashnode_t *new_node = _create_node(key, value);
    if (!new_node) return -1;
    new_node->next = hash->nodes[idx];
    hash->nodes[idx] = new_node;
    hash->count++;
    return 0;
}

char *kvs_hash_get(kvs_hash_t *hash, char *key) {
    if (!hash || !key) return NULL;
    int idx = _hash(key, hash->max_slots);
    hashnode_t *node = hash->nodes[idx];
    while (node) {
        if (strcmp(node->key, key) == 0) {
            return node->value;
        }
        node = node->next;
    }
    return NULL;
}

int kvs_hash_mod(kvs_hash_t *hash, char *key, char *value) {
    if (!hash || !key || !value) return -1;
    int idx = _hash(key, hash->max_slots);
    hashnode_t *node = hash->nodes[idx];
    while (node) {
        if (strcmp(node->key, key) == 0) {
#if ENABLE_KEY_POINTER
            kvs_free(node->value);
            node->value = kvs_malloc(strlen(value) + 1);
            if (!node->value) return -1;
            strcpy(node->value, value);
#else
            strncpy(node->value, value, MAX_VALUE_LEN - 1);
            node->value[MAX_VALUE_LEN - 1] = '\0';
#endif
            return 0;
        }
        node = node->next;
    }
    return 1;
}

int kvs_hash_del(kvs_hash_t *hash, char *key) {
    if (!hash || !key) return -2;

    int idx = _hash(key, hash->max_slots);
    hashnode_t *head = hash->nodes[idx];
    if (!head) return -1;  // 键不存在

    // 情况1：要删除的是头节点
    if (strcmp(head->key, key) == 0) {
        hash->nodes[idx] = head->next;
#if ENABLE_KEY_POINTER
        if (head->key) kvs_free(head->key);
        if (head->value) kvs_free(head->value);
#endif
        kvs_free(head);
        hash->count--;
        return 0;
    }

    // 情况2：要删除的是中间或尾节点
    hashnode_t *prev = head;
    hashnode_t *curr = head->next;

    while (curr) {
        if (strcmp(curr->key, key) == 0) {
            prev->next = curr->next;  // 跳过当前节点
            
#if ENABLE_KEY_POINTER
            if (curr->key) kvs_free(curr->key);
            if (curr->value) kvs_free(curr->value);
#endif
            kvs_free(curr);
            hash->count--;
            return 0;
        }
        prev = curr;
        curr = curr->next;
    }

    return -1;  // 未找到键
}

int kvs_hash_exist(kvs_hash_t *hash, char *key) {
    return (kvs_hash_get(hash, key) != NULL) ? 0 : 1;
}

void kvs_hash_foreach(kvs_hash_t *T,
                      void (*cb)(const char *, const char *, void *),
                      void *arg) {
    if (!T || !cb) return;
    for (int i = 0; i < T->max_slots; i++) {
        hashnode_t *node = T->nodes[i];
        while (node) {
            cb(node->key, node->value, arg);
            node = node->next;
        }
    }
}

int kvs_hash_save(kvs_hash_t *hash, const char *filename) {
    FILE *fp = fopen(filename, "wb");
    if (!fp) return -1;

    for (int i = 0; i < hash->max_slots; i++) {
        hashnode_t *node = hash->nodes[i];
        while (node) {
            size_t key_len = strlen(node->key);
            size_t val_len = strlen(node->value);

            if (fwrite(&key_len, sizeof(size_t), 1, fp) != 1) goto error;
            if (fwrite(node->key, 1, key_len, fp) != key_len) goto error;

            if (fwrite(&val_len, sizeof(size_t), 1, fp) != 1) goto error;
            if (fwrite(node->value, 1, val_len, fp) != val_len) goto error;

            node = node->next;
        }
    }
    fclose(fp);
    return 0;

error:
    fclose(fp);
    return -1;
}


/* load rdb file into hash*/
int kvs_hash_load_rdb(kvs_hash_t *hash, const char *filename) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        printf("[RDB] Failed to open file: %s\n", filename);
        return -1;
    }

#ifdef DEBUG
    printf("[RDB] Loading from file: %s\n", filename);
#endif
    
    int loaded = 0;
    long err_pos = 0;

    while (1) {
        size_t klen, vlen;
        if (fread(&klen, sizeof(size_t), 1, fp) != 1) {
            if (feof(fp)) break;
            err_pos = ftell(fp);
            goto error;
        }
        if (klen > 1024*1024) { err_pos = ftell(fp); goto error; }

        char *key = kvs_malloc(klen + 1);
        if (!key) { err_pos = ftell(fp); goto error; }
        if (fread(key, 1, klen, fp) != klen) {
            err_pos = ftell(fp); kvs_free(key); goto error;
        }
        key[klen] = '\0';

        if (fread(&vlen, sizeof(size_t), 1, fp) != 1) {
            err_pos = ftell(fp); kvs_free(key); goto error;
        }
        if (vlen > 10*1024*1024) { 
            err_pos = ftell(fp); kvs_free(key); goto error; 
        }

        char *val = kvs_malloc(vlen + 1);
        if (!val) { err_pos = ftell(fp); kvs_free(key); goto error; }
        if (fread(val, 1, vlen, fp) != vlen) {
            err_pos = ftell(fp); kvs_free(key); kvs_free(val); goto error;
        }
        val[vlen] = '\0';

        kvs_hash_set(hash, key, val);
        loaded++;

        kvs_free(key);
        kvs_free(val);

#ifdef DEBUG
        if (loaded % 1000 == 0) printf("[RDB] Loaded %d keys\n", loaded);
#endif
    }

    fclose(fp);
    printf("[RDB] Loaded %d keys\n", loaded);
    return loaded;

error:
    fclose(fp);
    printf("[RDB] Load failed at %ld, loaded %d keys\n", err_pos, loaded);
    return -1;
}

