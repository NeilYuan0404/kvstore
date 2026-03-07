#include "../include/kvs_base.h"
#include "../include/kvs_persist.h"
#include "../include/kvs_hash.h"
#include "../include/kvs_configure.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

bool g_is_loading = false;
persist_runtime_t g_persist_runtime;

extern kvs_hash_t global_hash;
extern int kvs_protocol(char *msg, int length, char *response, int resp_size, int *processed, int *needed);

/* 二进制安全 RESP 编码（内部使用） */
static int resp_encode(char *buffer, size_t buf_size,
                       const char *cmd,
                       const void *key, size_t key_len,
                       const void *val, size_t val_len) {
    int pos = 0;
    int argc = val ? 3 : 2;
    size_t cmd_len = strlen(cmd);

    if (buf_size < 64) return -1;

    pos += snprintf(buffer + pos, buf_size - pos, "*%d\r\n", argc);
    pos += snprintf(buffer + pos, buf_size - pos, "$%zu\r\n", cmd_len);
    if (pos + cmd_len + 2 > buf_size) return -1;
    memcpy(buffer + pos, cmd, cmd_len);
    pos += cmd_len;
    buffer[pos++] = '\r';
    buffer[pos++] = '\n';

    pos += snprintf(buffer + pos, buf_size - pos, "$%zu\r\n", key_len);
    if (pos + key_len + 2 > buf_size) return -1;
    memcpy(buffer + pos, key, key_len);
    pos += key_len;
    buffer[pos++] = '\r';
    buffer[pos++] = '\n';

    if (val) {
        pos += snprintf(buffer + pos, buf_size - pos, "$%zu\r\n", val_len);
        if (pos + val_len + 2 > buf_size) return -1;
        memcpy(buffer + pos, val, val_len);
        pos += val_len;
        buffer[pos++] = '\r';
        buffer[pos++] = '\n';
    }

    return pos;
}

/* 真正的二进制安全 AOF 追加函数 */
static void kvs_aof_append_bin(const char *cmd, const void *key, size_t key_len,
                               const void *val, size_t val_len) {
    if (g_is_loading) return;
    if (g_config.persist_mode != PERSIST_AOF_ONLY &&
        g_config.persist_mode != PERSIST_MIXED)
        return;

    FILE *fp = fopen(g_config.aof_file, "ab");
    if (!fp) {
        LOG_WARN("[Persist] Failed to open AOF file: %s\n", g_config.aof_file);
        return;
    }

    char buffer[KVS_MAX_MSG_LEN];
    int len = resp_encode(buffer, sizeof(buffer), cmd, key, key_len, val, val_len);
    if (len > 0) {
        fwrite(buffer, 1, len, fp);
    } else {
        LOG_WARN("[Persist] AOF encode failed\n");
    }
    fclose(fp);
}

/* 对外接口：兼容旧调用（三个字符串参数） */
void kvs_aof_append(const char *cmd, const char *key, const char *value) {
    size_t key_len = key ? strlen(key) : 0;
    size_t val_len = value ? strlen(value) : 0;
    kvs_aof_append_bin(cmd, key, key_len, value, val_len);
}

void kvs_persist_init(void) {
    memset(&g_persist_runtime, 0, sizeof(g_persist_runtime));
    g_persist_runtime.last_save_time = time(NULL);
    g_is_loading = false;
    LOG_INFO("[Persist] Initialized, mode=%d\n", g_config.persist_mode);
}

static int save_item(FILE *fp, const void *key, size_t key_len,
                     const void *val, size_t val_len) {
    if (fwrite(&key_len, sizeof(size_t), 1, fp) != 1) return -1;
    if (fwrite(key, 1, key_len, fp) != key_len) return -1;
    if (fwrite(&val_len, sizeof(size_t), 1, fp) != 1) return -1;
    if (fwrite(val, 1, val_len, fp) != val_len) return -1;
    return 0;
}

void kvs_rdb_save(void) {
    FILE *fp = fopen(g_config.rdb_file, "wb");
    if (!fp) {
        LOG_WARN("[Persist] Failed to open RDB file for write: %s\n", g_config.rdb_file);
        return;
    }

    for (int i = 0; i < global_hash.max_slots; i++) {
        hashnode_t *node = global_hash.nodes[i];
        while (node) {
            if (save_item(fp, node->key, node->key_len,
                          node->value, node->value_len) < 0) {
                LOG_WARN("[Persist] Error writing RDB entry\n");
                fclose(fp);
                return;
            }
            node = node->next;
        }
    }

    fflush(fp);
    fclose(fp);
    g_persist_runtime.last_save_time = time(NULL);
    LOG_INFO("[Persist] RDB snapshot saved to %s\n", g_config.rdb_file);
}

void load_aof_file(const char *filename) {
    if (!filename) return;
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        LOG_INFO("[Persist] AOF file not found: %s\n", filename);
        return;
    }

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fsize <= 0) {
        fclose(fp);
        return;
    }

    char *buffer = kvs_malloc(fsize + 1);
    if (!buffer) {
        fclose(fp);
        return;
    }
    size_t bytes_read = fread(buffer, 1, fsize, fp);
    fclose(fp);
    if (bytes_read != (size_t)fsize) {
        kvs_free(buffer);
        return;
    }
    buffer[fsize] = '\0';

    g_is_loading = 1;
    char dummy[8192];
    int offset = 0;
    int cmd_count = 0;

    while (offset < fsize) {
        int processed = 0;
        int needed = 0;
        int ret = kvs_protocol(buffer + offset, fsize - offset,
                               dummy, sizeof(dummy), &processed, &needed);
        if (ret == -2) {
            LOG_WARN("[Persist] Response too large (%d) at offset %d\n", needed, offset);
            char *next = memchr(buffer + offset, '\n', fsize - offset);
            if (next) {
                offset = next - buffer + 1;
                continue;
            } else {
                break;
            }
        } else if (ret < 0) {
            LOG_WARN("[Persist] Protocol error at offset %d\n", offset);
            break;
        }
        if (processed == 0) break;
        offset += processed;
        cmd_count++;
    }

    LOG_INFO("[Persist] AOF replay completed: %d commands\n", cmd_count);
    kvs_free(buffer);
    g_is_loading = 0;
}

void kvs_rdb_check_and_save(void) {
    time_t now = time(NULL);
    if (now - g_persist_runtime.last_save_time >= g_config.rdb_save_interval) {
        LOG_INFO("[Persist] Auto RDB snapshot triggered\n");
        kvs_rdb_save();
    }
}

static int kvs_aof_needs_rewrite(void) {
    struct stat st;
    if (!g_config.aof_auto_rewrite) return 0;
    if (stat(g_config.aof_file, &st) == 0) {
        long threshold = g_config.aof_rewrite_size * 1024L * 1024L;
        return st.st_size > threshold;
    }
    return 0;
}

void kvs_aof_rewrite(void) {
    static int rewrite_in_progress = 0;
    if (rewrite_in_progress) return;
    rewrite_in_progress = 1;

    LOG_INFO("[Persist] Starting AOF rewrite...\n");

    char tmpfile[512];
    snprintf(tmpfile, sizeof(tmpfile), "%s.tmp", g_config.aof_file);

    FILE *fp = fopen(tmpfile, "w");
    if (!fp) {
        perror("[Persist] Failed to create temp AOF");
        rewrite_in_progress = 0;
        return;
    }

    char buf[KVS_MAX_MSG_LEN];
    for (int i = 0; i < global_hash.max_slots; i++) {
        hashnode_t *node = global_hash.nodes[i];
        while (node) {
            int pos = 0;
            int remaining = sizeof(buf) - pos;
            pos += snprintf(buf + pos, remaining, "*3\r\n$4\r\nHSET\r\n");

            remaining = sizeof(buf) - pos;
            pos += snprintf(buf + pos, remaining, "$%zu\r\n", node->key_len);
            if (pos + node->key_len + 2 > sizeof(buf)) {
                LOG_WARN("[Persist] Buffer too small for key\n");
                break;
            }
            memcpy(buf + pos, node->key, node->key_len);
            pos += node->key_len;
            buf[pos++] = '\r';
            buf[pos++] = '\n';

            remaining = sizeof(buf) - pos;
            pos += snprintf(buf + pos, remaining, "$%zu\r\n", node->value_len);
            if (pos + node->value_len + 2 > sizeof(buf)) {
                LOG_WARN("[Persist] Buffer too small for value\n");
                break;
            }
            memcpy(buf + pos, node->value, node->value_len);
            pos += node->value_len;
            buf[pos++] = '\r';
            buf[pos++] = '\n';

            fwrite(buf, 1, pos, fp);
            node = node->next;
        }
    }

    fflush(fp);
    fclose(fp);

    if (rename(tmpfile, g_config.aof_file) == 0) {
        LOG_INFO("[Persist] AOF rewrite completed\n");
    } else {
        perror("[Persist] Failed to replace AOF");
        unlink(tmpfile);
    }

    rewrite_in_progress = 0;
}

void kvs_aof_check_and_rewrite(void) {
    if (kvs_aof_needs_rewrite()) {
        kvs_aof_rewrite();
    }
}