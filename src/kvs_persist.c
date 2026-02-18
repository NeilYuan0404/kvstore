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
    g_persist_config.rdb.save_interval = 300;      /* 1 minutes */
    g_persist_config.rdb.min_changes = 100;        /* at least 100 writes */
    g_persist_config.rdb.save_on_shutdown = true;
    g_persist_config.rdb.last_save_time = time(NULL);
    g_persist_config.rdb.dirty = 0; //however, dirty is not used

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

static int resp_encode(char *buffer, const char *cmd, const char *key, const char *value) {
    int pos = 0;
    int argc = value ? 3 : 2;
    pos += sprintf(buffer + pos, "*%d\r\n", argc);
    pos += sprintf(buffer + pos, "$%d\r\n%s\r\n", (int)strlen(cmd), cmd);
    pos += sprintf(buffer + pos, "$%d\r\n%s\r\n", (int)strlen(key), key);
    if (value)
        pos += sprintf(buffer + pos, "$%d\r\n%s\r\n", (int)strlen(value), value);
    return pos;
}

void kvs_aof_append(const char *cmd, const char *key, const char *value) {
    if (g_is_loading) return;
    FILE *fp = fopen(AOF_FILE, "ab");
    if (!fp) return;
    char buffer[KVS_MAX_MSG_LEN];
    int len = resp_encode(buffer, cmd, key, value);
    fwrite(buffer, 1, len, fp);
    fclose(fp);
}

static int save_item(FILE *fp, const char *key, const char *value) {
    size_t key_len = strlen(key);
    size_t val_len = strlen(value);
    if (fwrite(&key_len, sizeof(size_t), 1, fp) != 1) return -1;
    if (fwrite(key, 1, key_len, fp) != key_len) return -1;
    if (fwrite(&val_len, sizeof(size_t), 1, fp) != 1) return -1;
    if (fwrite(value, 1, val_len, fp) != val_len) return -1;
    return 0;
}

void kvs_rdb_save() {
    FILE *fp = fopen(RDB_FILE, "wb");
    if (!fp) {
        perror("fopen RDB");
        return;
    }
    for (int i = 0; i < global_hash.max_slots; i++) {
        hashnode_t *node = global_hash.nodes[i];
        while (node) {
            save_item(fp, node->key, node->value);
            node = node->next;
        }
    }
    fflush(fp);
    fclose(fp);
    
    printf("[Persist] RDB snapshot saved to %s\n", RDB_FILE);
}

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
    
    // 临时缓冲区，每条命令的响应不会超过4KB
    char dummy[4096];
    int offset = 0;
    int cmd_count = 0;
    
    while (offset < fsize) {
        int processed = 0;
        int ret = kvs_protocol(buffer + offset, fsize - offset, dummy, &processed);
        if (ret <= 0 || processed == 0) {
            if (ret < 0) {
                printf("[Persist] Protocol error at offset %d, stopping recovery\n", offset);
            } else {
                printf("[Persist] Incomplete command at end of file\n");
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
    
    printf("\n[Persist] AOF replay completed: %d commands, %d bytes processed\n", cmd_count, offset);
    
    kvs_free(buffer);
    g_is_loading = 0;
}

/* Called on every write operation (HSET, HDEL, HMOD) */
void kvs_rdb_notify_write(void) {
    g_persist_config.rdb.dirty++;
}

/* Check if RDB snapshot is needed based on time and changes */
/* Check if RDB snapshot is needed based on time only */
void kvs_rdb_check_and_save(void) {
    time_t now = time(NULL);
    
    if (now - g_persist_config.rdb.last_save_time >= g_persist_config.rdb.save_interval) {
        printf("[Persist] Auto RDB snapshot triggered (interval: %d seconds)\n",
               g_persist_config.rdb.save_interval);
        kvs_rdb_save();
        g_persist_config.rdb.last_save_time = now;
    }
}

/* AOF rewrite (existing) */
int kvs_aof_needs_rewrite(void) {
    struct stat st;
    if (!g_persist_config.aof.auto_rewrite) return 0;
    if (stat(AOF_FILE, &st) == 0) {
        long threshold = g_persist_config.aof.rewrite_size * 1024L * 1024L;
        return st.st_size > threshold;
    }
    return 0;
}

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
            int len = sprintf(buf, "*3\r\n$4\r\nHSET\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n",
                              (int)strlen(node->key), node->key,
                              (int)strlen(node->value), node->value);
            fwrite(buf, 1, len, fp);
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

void kvs_aof_check_and_rewrite(void) {
    if (kvs_aof_needs_rewrite()) {
        kvs_aof_rewrite();
    }
}
