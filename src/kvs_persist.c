#include "../include/kvs_base.h"
#include "../include/kvs_persist.h"
#include "../include/kvs_hash.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

bool g_is_loading = false;
persist_config_t g_persist_config;

extern kvs_hash_t global_hash;

void kvs_persist_init(const char *aof_filename, const char *rdb_filename) {
    memset(&g_persist_config, 0, sizeof(g_persist_config));
    
    /* RDB config */
    g_persist_config.rdb.save_interval = 300;      /* 5 minutes */
    g_persist_config.rdb.min_changes = 100;        /* at least 100 writes */
    g_persist_config.rdb.save_on_shutdown = true;
    g_persist_config.rdb.last_save_time = time(NULL);
    g_persist_config.rdb.dirty = 0; // however, dirty is not used

    if (rdb_filename)
        strncpy(g_persist_config.rdb.filename, rdb_filename, sizeof(g_persist_config.rdb.filename)-1);
    else
        strcpy(g_persist_config.rdb.filename, "../data/kvstore.rdb");

    /* AOF config */
    if (aof_filename)
        strncpy(g_persist_config.aof.filename, aof_filename, sizeof(g_persist_config.aof.filename)-1);
    else
        strcpy(g_persist_config.aof.filename, "../data/kvstore.aof");

    g_persist_config.aof.rewrite_size = 1;          /* MB */
    g_persist_config.aof.auto_rewrite = true;
    g_is_loading = false;
}

/* 二进制安全的 RESP 编码函数 */
static int resp_encode(char *buffer, size_t buf_size,
                       const char *cmd,
                       const void *key, size_t key_len,
                       const void *value, size_t val_len) {
    int pos = 0;
    int argc = value ? 3 : 2;
    size_t cmd_len = strlen(cmd);  // cmd 是命令字符串，安全使用 strlen
    
    // 检查缓冲区是否足够
    if (buf_size < 64) return -1;  // 最小安全缓冲区
    
    // *<argc>\r\n
    pos += snprintf(buffer + pos, buf_size - pos, "*%d\r\n", argc);
    
    // $<cmd_len>\r\n<cmd>\r\n
    pos += snprintf(buffer + pos, buf_size - pos, "$%zu\r\n", cmd_len);
    if (pos + cmd_len + 2 > buf_size) return -1;
    memcpy(buffer + pos, cmd, cmd_len);
    pos += cmd_len;
    buffer[pos++] = '\r';
    buffer[pos++] = '\n';
    
    // $<key_len>\r\n<key>\r\n
    pos += snprintf(buffer + pos, buf_size - pos, "$%zu\r\n", key_len);
    if (pos + key_len + 2 > buf_size) return -1;
    memcpy(buffer + pos, key, key_len);
    pos += key_len;
    buffer[pos++] = '\r';
    buffer[pos++] = '\n';
    
    // $<val_len>\r\n<val>\r\n (if value exists)
    if (value) {
        pos += snprintf(buffer + pos, buf_size - pos, "$%zu\r\n", val_len);
        if (pos + val_len + 2 > buf_size) return -1;
        memcpy(buffer + pos, value, val_len);
        pos += val_len;
        buffer[pos++] = '\r';
        buffer[pos++] = '\n';
    }
    
    return pos;
}

/* AOF 追加 - 需要获取 key 和 value 的长度 */
void kvs_aof_append(const char *cmd, const char *key, const char *value) {
    if (g_is_loading) return;
    
    // key 和 value 是字符串，可以使用 strlen
    size_t key_len = strlen(key);
    size_t val_len = value ? strlen(value) : 0;
    
    FILE *fp = fopen(AOF_FILE, "ab");
    if (!fp) return;
    
    char buffer[KVS_MAX_MSG_LEN];
    int len = resp_encode(buffer, sizeof(buffer), cmd, key, key_len, value, val_len);
    
    if (len > 0) {
        fwrite(buffer, 1, len, fp);
    }
    
    fclose(fp);
}

/* RDB 保存项 - 二进制安全 */
static int save_item(FILE *fp, const void *key, size_t key_len, const void *val, size_t val_len) {
    if (fwrite(&key_len, sizeof(size_t), 1, fp) != 1) return -1;
    if (fwrite(key, 1, key_len, fp) != key_len) return -1;
    if (fwrite(&val_len, sizeof(size_t), 1, fp) != 1) return -1;
    if (fwrite(val, 1, val_len, fp) != val_len) return -1;
    return 0;
}

/* RDB 保存 */
void kvs_rdb_save() {
    FILE *fp = fopen(RDB_FILE, "wb");
    if (!fp) {
        perror("fopen RDB");
        return;
    }
    
    for (int i = 0; i < global_hash.max_slots; i++) {
        hashnode_t *node = global_hash.nodes[i];
        while (node) {
            save_item(fp, node->key, node->key_len, node->value, node->value_len);
            node = node->next;
        }
    }
    
    fflush(fp);
    fclose(fp);
    
    printf("[Persist] RDB snapshot saved to %s\n", RDB_FILE);
}

/* 加载 AOF 文件 */
void load_aof_file(const char *filename) {
    if (!filename) return;
    
    FILE *fp = fopen(filename, "r");
    if (!fp) { 
        printf("[Persist] File %s not found\n", filename); 
        return; 
    }
    
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fsize <= 0) { fclose(fp); return; }
    
    char *buffer = kvs_malloc(fsize + 1);
    if (!buffer) { fclose(fp); return; }
    size_t bytes_read = fread(buffer, 1, fsize, fp);
    fclose(fp);
    if (bytes_read != (size_t)fsize) { kvs_free(buffer); return; }
    buffer[fsize] = '\0';
    
    g_is_loading = 1;
    
    char dummy[8192];  // 响应缓冲区
    int offset = 0;
    int cmd_count = 0;
    
    while (offset < fsize) {
        int processed = 0;
        int needed = 0;
        int ret = kvs_protocol(buffer + offset, fsize - offset, 
                               dummy, sizeof(dummy), &processed, &needed);
        
        if (ret == -2) {
            printf("[Persist] Warning: Response too large (%d bytes) at offset %d\n", 
                   needed, offset);
            // 尝试跳过当前命令
            char *next = memchr(buffer + offset, '\n', fsize - offset);
            if (next) {
                offset = next - buffer + 1;
                continue;
            } else {
                break;
            }
        } else if (ret <= 0 || processed == 0) {
            if (ret < 0) {
                printf("[Persist] Protocol error at offset %d\n", offset);
            }
            break;
        }
        
        offset += processed;
        cmd_count++;
        
        if (cmd_count % 1000 == 0) {
            printf("[Persist] Replayed %d commands\r", cmd_count);
            fflush(stdout);
        }
    }
    
    printf("\n[Persist] AOF replay completed: %d commands, %d bytes\n", cmd_count, offset);
    
    kvs_free(buffer);
    g_is_loading = 0;
}

/* RDB 通知写入（未使用） */
void kvs_rdb_notify_write(void) {
    g_persist_config.rdb.dirty++;
}

/* RDB 定时检查保存 */
void kvs_rdb_check_and_save(void) {
    time_t now = time(NULL);
    
    if (now - g_persist_config.rdb.last_save_time >= g_persist_config.rdb.save_interval) {
        printf("[Persist] Auto RDB snapshot triggered (interval: %d seconds)\n",
               g_persist_config.rdb.save_interval);
        kvs_rdb_save();
        g_persist_config.rdb.last_save_time = now;
    }
}

/* AOF 重写检查 */
int kvs_aof_needs_rewrite(void) {
    struct stat st;
    if (!g_persist_config.aof.auto_rewrite) return 0;
    if (stat(AOF_FILE, &st) == 0) {
        long threshold = g_persist_config.aof.rewrite_size * 1024L * 1024L;
        return st.st_size > threshold;
    }
    return 0;
}

/* AOF 重写 */
void kvs_aof_rewrite(void) {
    static int rewrite_in_progress = 0;
    if (rewrite_in_progress) return;
    rewrite_in_progress = 1;

    printf("[Persist] Starting AOF rewrite...\n");

    char tmpfile[512];
    snprintf(tmpfile, sizeof(tmpfile), "%s.tmp", AOF_FILE);

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
            
            // 构造 RESP 格式的 HSET 命令
            int remaining = sizeof(buf) - pos;
            pos += snprintf(buf + pos, remaining, "*3\r\n$4\r\nHSET\r\n");
            
            // 添加 key
            remaining = sizeof(buf) - pos;
            pos += snprintf(buf + pos, remaining, "$%zu\r\n", node->key_len);
            if (pos + node->key_len + 2 > sizeof(buf)) {
                printf("[Persist] Buffer too small for key\n");
                break;
            }
            memcpy(buf + pos, node->key, node->key_len);
            pos += node->key_len;
            buf[pos++] = '\r';
            buf[pos++] = '\n';
            
            // 添加 value
            remaining = sizeof(buf) - pos;
            pos += snprintf(buf + pos, remaining, "$%zu\r\n", node->value_len);
            if (pos + node->value_len + 2 > sizeof(buf)) {
                printf("[Persist] Buffer too small for value\n");
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

    if (rename(tmpfile, AOF_FILE) == 0) {
        printf("[Persist] AOF rewrite completed\n");
    } else {
        perror("[Persist] Failed to replace AOF");
        unlink(tmpfile);
    }

    rewrite_in_progress = 0;
}

/* AOF 重写检查并执行 */
void kvs_aof_check_and_rewrite(void) {
    if (kvs_aof_needs_rewrite()) {
        kvs_aof_rewrite();
    }
}
