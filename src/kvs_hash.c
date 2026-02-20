#include "../include/kvstore.h"   // 可能不需要，根据实际调整
#include "../include/kvs_hash.h"
#include "../include/kvs_base.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

kvs_hash_t global_hash;  // 全局实例，根据需要定义位置

// 哈希函数（二进制安全）
static int _hash(const void *key, size_t len, int size) {
    unsigned int hash = 5381;
    const unsigned char *p = key;
    for (size_t i = 0; i < len; i++) {
        hash = ((hash << 5) + hash) + p[i];
    }
    return hash % size;
}

// 比较两个键是否相等（二进制安全）
static int key_equal(const void *k1, size_t len1, const void *k2, size_t len2) {
    return (len1 == len2) && (memcmp(k1, k2, len1) == 0);
}

// 创建节点（内部使用，传入长度）
static hashnode_t *_create_node(const void *key, size_t key_len, const void *val, size_t val_len) {
    hashnode_t *node = (hashnode_t*)kvs_malloc(sizeof(hashnode_t));
    if (!node) return NULL;

    node->key = kvs_malloc(key_len);
    if (!node->key) {
        kvs_free(node);
        return NULL;
    }
    memcpy(node->key, key, key_len);
    node->key_len = key_len;

    node->value = kvs_malloc(val_len);
    if (!node->value) {
        kvs_free(node->key);
        kvs_free(node);
        return NULL;
    }
    memcpy(node->value, val, val_len);
    node->value_len = val_len;

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

void kvs_hash_destroy(kvs_hash_t *hash) {
    if (!hash || !hash->nodes) return;
    for (int i = 0; i < hash->max_slots; i++) {
        hashnode_t *node = hash->nodes[i];
        while (node) {
            hashnode_t *tmp = node;
            node = node->next;
            kvs_free(tmp->key);
            kvs_free(tmp->value);
            kvs_free(tmp);
        }
    }
    kvs_free(hash->nodes);
    hash->nodes = NULL;
    hash->count = 0;
}

int kvs_hash_set(kvs_hash_t *hash, const void *key, size_t key_len, const void *val, size_t val_len) {
    if (!hash || !key || !val) return -1;
    int idx = _hash(key, key_len, hash->max_slots);

    // 查找是否存在
    hashnode_t *node = hash->nodes[idx];
    while (node) {
        if (key_equal(node->key, node->key_len, key, key_len)) {
            // 更新值
            kvs_free(node->value);
            node->value = kvs_malloc(val_len);
            if (!node->value) return -1;
            memcpy(node->value, val, val_len);
            node->value_len = val_len;
            return 0;  // 更新成功
        }
        node = node->next;
    }

    // 不存在，创建新节点
    hashnode_t *new_node = _create_node(key, key_len, val, val_len);
    if (!new_node) return -1;
    new_node->next = hash->nodes[idx];
    hash->nodes[idx] = new_node;
    hash->count++;
    return 0;  // 插入成功
}

void *kvs_hash_get(kvs_hash_t *hash, const void *key, size_t key_len, size_t *val_len) {
    if (!hash || !key) return NULL;
    int idx = _hash(key, key_len, hash->max_slots);
    hashnode_t *node = hash->nodes[idx];
    while (node) {
        if (key_equal(node->key, node->key_len, key, key_len)) {
            *val_len = node->value_len;
            return node->value;
        }
        node = node->next;
    }
    *val_len = 0;
    return NULL;
}

int kvs_hash_del(kvs_hash_t *hash, const void *key, size_t key_len) {
    if (!hash || !key) return -2;
    int idx = _hash(key, key_len, hash->max_slots);
    hashnode_t *head = hash->nodes[idx];
    if (!head) return -1;

    // 删除头节点
    if (key_equal(head->key, head->key_len, key, key_len)) {
        hash->nodes[idx] = head->next;
        kvs_free(head->key);
        kvs_free(head->value);
        kvs_free(head);
        hash->count--;
        return 0;
    }

    // 删除中间或尾部节点
    hashnode_t *prev = head;
    hashnode_t *curr = head->next;
    while (curr) {
        if (key_equal(curr->key, curr->key_len, key, key_len)) {
            prev->next = curr->next;
            kvs_free(curr->key);
            kvs_free(curr->value);
            kvs_free(curr);
            hash->count--;
            return 0;
        }
        prev = curr;
        curr = curr->next;
    }
    return -1;  // 未找到
}

int kvs_hash_mod(kvs_hash_t *hash, const void *key, size_t key_len, const void *val, size_t val_len) {
    if (!hash || !key || !val) return -1;
    int idx = _hash(key, key_len, hash->max_slots);
    hashnode_t *node = hash->nodes[idx];
    while (node) {
        if (key_equal(node->key, node->key_len, key, key_len)) {
            // 修改值
            kvs_free(node->value);
            node->value = kvs_malloc(val_len);
            if (!node->value) return -1;
            memcpy(node->value, val, val_len);
            node->value_len = val_len;
            return 0;
        }
        node = node->next;
    }
    return 1;  // 键不存在
}

int kvs_hash_exist(kvs_hash_t *hash, const void *key, size_t key_len) {
    size_t dummy;
    return (kvs_hash_get(hash, key, key_len, &dummy) != NULL) ? 0 : 1;
}

void kvs_hash_foreach(kvs_hash_t *T,
                      void (*cb)(const void *key, size_t key_len, const void *val, size_t val_len, void *arg),
                      void *arg) {
    if (!T || !cb) return;
    for (int i = 0; i < T->max_slots; i++) {
        hashnode_t *node = T->nodes[i];
        while (node) {
            cb(node->key, node->key_len, node->value, node->value_len, arg);
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
            // 写入 key 长度和数据
            if (fwrite(&node->key_len, sizeof(size_t), 1, fp) != 1) goto error;
            if (fwrite(node->key, 1, node->key_len, fp) != node->key_len) goto error;
            // 写入 value 长度和数据
            if (fwrite(&node->value_len, sizeof(size_t), 1, fp) != 1) goto error;
            if (fwrite(node->value, 1, node->value_len, fp) != node->value_len) goto error;
            node = node->next;
        }
    }
    fclose(fp);
    return 0;

error:
    fclose(fp);
    return -1;
}

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

        void *key = kvs_malloc(klen);
        if (!key) { err_pos = ftell(fp); goto error; }
        if (fread(key, 1, klen, fp) != klen) {
            err_pos = ftell(fp); kvs_free(key); goto error;
        }

        if (fread(&vlen, sizeof(size_t), 1, fp) != 1) {
            err_pos = ftell(fp); kvs_free(key); goto error;
        }
        if (vlen > 10*1024*1024) {
            err_pos = ftell(fp); kvs_free(key); goto error;
        }

        void *val = kvs_malloc(vlen);
        if (!val) { err_pos = ftell(fp); kvs_free(key); goto error; }
        if (fread(val, 1, vlen, fp) != vlen) {
            err_pos = ftell(fp); kvs_free(key); kvs_free(val); goto error;
        }

        kvs_hash_set(hash, key, klen, val, vlen);
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
