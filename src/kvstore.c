#include "../include/kvs_base.h"
#include "../include/kvs_hash.h"
#include "../include/kvs_persist.h"
#ifdef ENABLE_REPL
#include "../include/kvs_replication.h"
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stddef.h>
#include <ctype.h>

/* Global hash table instance */
extern kvs_hash_t global_hash;

/* Command mapping table */
static const char *command[] = {
    "HSET", "HGET", "HDEL", "HMOD", "HEXIST", "SAVE"
};
enum {
    CMD_HSET, CMD_HGET, CMD_HDEL, CMD_HMOD, CMD_HEXIST, CMD_SAVE, CMD_COUNT
};

/* typedef int (*msg_handler)(char *msg, int length, char *response, int *processed); */
extern int reactor_start(unsigned short port, 
                        int (*handler)(char *msg, int length, char *response, 
                                      int resp_size, int *processed, int *needed));
extern bool g_is_loading;

/* Helper: append bulk string to response buffer */
static int append_bulk_string(char *resp, const void *data, size_t len) {
    int n = sprintf(resp, "$%zu\r\n", len);
    memcpy(resp + n, data, len);
    n += len;
    resp[n++] = '\r';
    resp[n++] = '\n';
    return n;
}

/*--------------------------------------------------------------------------
 * Command executor with replication and AOF persistence
 * All responses follow RESP protocol.
 *--------------------------------------------------------------------------*/
int kvs_executor(char **tokens, int count, char *response) {
    if (!tokens || !tokens[0] || count < 1 || !response) return -1;

#ifdef DEBUG
    printf("[DEBUG] Executing command:");
    for (int i = 0; i < count; i++) {
        printf(" %s", tokens[i]);
    }
    printf("\n");
#endif

    int cmd;
    for (cmd = 0; cmd < CMD_COUNT; cmd++)
        if (strcmp(tokens[0], command[cmd]) == 0) break;
    if (cmd >= CMD_COUNT) {
#ifdef DEBUG
        printf("[DEBUG] Unknown command: %s\n", tokens[0]);
#endif
        return sprintf(response, "-ERR unknown command\r\n");
    }

    char *key = count > 1 ? tokens[1] : NULL;
    char *val = count > 2 ? tokens[2] : NULL;
    int ret, len = 0;
    size_t key_len = key ? strlen(key) : 0;
    size_t val_len = val ? strlen(val) : 0;

    switch (cmd) {
    case CMD_HSET:
        ret = kvs_hash_set(&global_hash, key, key_len, val, val_len);
        if (ret < 0)
            len = sprintf(response, "-ERR internal error\r\n");
        else if (ret == 0) {
            len = sprintf(response, "+OK\r\n");
#if ENABLE_PERSIST
            kvs_aof_append("HSET", key, val);
#endif
#if ENABLE_REPL
            if (g_repl.role == KVS_ROLE_MASTER)
                kvs_replication_feed_slaves("HSET", key, val);
#endif
        } else
            len = sprintf(response, "+EXIST\r\n");  // Key already exists
        break;

    case CMD_HGET: {
        size_t res_len;
        void *res = kvs_hash_get(&global_hash, key, key_len, &res_len);
        if (res) {
            len = append_bulk_string(response, res, res_len);
        } else {
            len = sprintf(response, "$-1\r\n");  // Null bulk string
        }
        break;
    }

    case CMD_HDEL:
        ret = kvs_hash_del(&global_hash, key, key_len);
        if (ret < 0)
            len = sprintf(response, "-ERR internal error\r\n");
        else if (ret == 0) {
            len = sprintf(response, "+OK\r\n");
#if ENABLE_PERSIST
            kvs_aof_append("HDEL", key, NULL);
#endif
#if ENABLE_REPL
            if (g_repl.role == KVS_ROLE_MASTER)
                kvs_replication_feed_slaves("HDEL", key, NULL);
#endif
        } else
            len = sprintf(response, "$-1\r\n");  // Key not found
        break;

    case CMD_HMOD:
        ret = kvs_hash_mod(&global_hash, key, key_len, val, val_len);
        if (ret < 0)
            len = sprintf(response, "-ERR internal error\r\n");
        else if (ret == 0) {
            len = sprintf(response, "+OK\r\n");
#if ENABLE_PERSIST
            kvs_aof_append("HMOD", key, val);
#endif
#if ENABLE_REPL
            if (g_repl.role == KVS_ROLE_MASTER)
                kvs_replication_feed_slaves("HMOD", key, val);
#endif
        } else
            len = sprintf(response, "$-1\r\n");  // Key not found
        break;

    case CMD_HEXIST:
        ret = kvs_hash_exist(&global_hash, key, key_len);
        len = (ret == 0) ? sprintf(response, ":1\r\n") : sprintf(response, ":0\r\n");
        break;

    case CMD_SAVE:
        kvs_rdb_save();
        len = sprintf(response, "+OK\r\n");
        break;
    }

#ifdef DEBUG
    printf("[DEBUG] Command result: %d bytes, response: %.*s", len, len, response);
#endif

    return len;
}

/*--------------------------------------------------------------------------
 * RESP protocol parser (only RESP format is supported)
 *--------------------------------------------------------------------------*/

/* Parse a RESP array and extract tokens. Returns number of bytes consumed,
 * 0 if incomplete, -1 on error.
 */
static int parse_resp(char *msg, int len, char *tokens[], int maxtok) {
    char *p = msg;
    char *end = msg + len;

    if (len < 4 || *p != '*') return -1;
    p++;  // skip '*'

    // Parse argc
    int argc = 0;
    char *num_start = p;
    while (p < end && *p >= '0' && *p <= '9') {
        argc = argc * 10 + (*p - '0');
        p++;
    }
    if (p == num_start) return -1;  // 确实没有数字，格式错误
    
    // 关键修改：如果已经到结尾，返回 0 表示需要更多数据
    if (p >= end) return 0;
    if (*p != '\r') return -1;
    p++;
    if (p >= end) return 0;
    if (*p != '\n') return -1;
    p++;

    if (argc > maxtok) return -1;

    for (int i = 0; i < argc; i++) {
        // 检查 $ 前缀
        if (p >= end) return 0;
        if (*p != '$') return -1;
        p++;

        // Parse bulk string length
        int blen = 0;
        char *blen_start = p;
        while (p < end && *p >= '0' && *p <= '9') {
            blen = blen * 10 + (*p - '0');
            p++;
        }
        if (p == blen_start) return -1;
        if (p >= end) return 0;
        if (*p != '\r') return -1;
        p++;
        if (p >= end) return 0;
        if (*p != '\n') return -1;
        p++;

        // 检查数据是否完整
        if (p + blen + 2 > end) return 0;

        tokens[i] = kvs_malloc(blen + 1);
        if (!tokens[i]) {
            for (int j = 0; j < i; j++) {
                kvs_free(tokens[j]);
                tokens[j] = NULL;
            }
            return -1;
        }
        memcpy(tokens[i], p, blen);
        tokens[i][blen] = '\0';
        p += blen + 2;
    }
    return p - msg;
}

/* Main protocol entry: only RESP format is accepted.
 * Processes as many complete commands as possible from the input buffer.
 * Returns:
 *   >0: actual response length written (if response buffer was large enough)
 *    0: need more data
 *   -1: protocol error
 * If response buffer is too small, returns -2 and sets *needed to required size.
 */
int kvs_protocol(char *msg, int length, char *response, int resp_size, int *processed, int *needed) {
    if (!msg || !response || !processed || !needed) return -1;
    if (length <= 0) return 0;
    
    char *p = msg;
    int remain = length;
    int total = 0;
    char *resp = response;
    int resp_remain = resp_size;
    *processed = 0;
    *needed = 0;

#ifdef DEBUG
    int cmd_count = 0;
#endif

    while (remain > 0) {
        // 跳过空白字符
        while (remain > 0 && isspace(*p)) {
            p++;
            remain--;
            (*processed)++;
        }
        if (remain <= 0) break;

        // 重新同步逻辑
        if (*p != '*') {
            char *next_star = memchr(p, '*', remain);
            if (next_star) {
                int junk = next_star - p;
#ifdef DEBUG
                printf("[DEBUG] Protocol resync: skipping %d bytes\n", junk);
#endif
                p = next_star;
                remain -= junk;
                *processed += junk;
                continue;
            } else {
                break;
            }
        }

        char *tokens[KVS_MAX_TOKENS] = {0};
        int consumed = parse_resp(p, remain, tokens, KVS_MAX_TOKENS);
        if (consumed > 0) {
            int tokcnt = 0;
            while (tokcnt < KVS_MAX_TOKENS && tokens[tokcnt]) tokcnt++;
            
            // **关键修改：先计算响应长度，但不写入**
            // 使用一个临时缓冲区计算长度
            char *temp = kvs_malloc(64 * 1024);
            if (!temp) {
                // 处理内存分配失败
                for (int i = 0; i < tokcnt; i++) {
                    if (tokens[i]) kvs_free(tokens[i]);
                }
                return -1;
            }
            int resp_len = kvs_executor(tokens, tokcnt, temp);
            kvs_free(temp);
            
            if (resp_len > resp_remain) {
                // 缓冲区太小，记录需要的空间
                *needed = resp_len;
                // 释放 tokens
                for (int i = 0; i < tokcnt; i++) {
                    if (tokens[i]) kvs_free(tokens[i]);
                }
                return -2;  // 需要更大的缓冲区
            }
            
            // 缓冲区足够，真正执行并写入
#ifdef DEBUG
            cmd_count++;
#endif
            resp_len = kvs_executor(tokens, tokcnt, resp);
            for (int i = 0; i < tokcnt; i++) {
                if (tokens[i]) kvs_free(tokens[i]);
            }
            
            if (resp_len > 0) {
                resp += resp_len;
                resp_remain -= resp_len;
                total += resp_len;
            }
            
            p += consumed;
            remain -= consumed;
            *processed += consumed;

            if (g_is_loading) {
                break;
            }
        } else if (consumed == 0) {
            break;
        } else {
#ifdef DEBUG
            printf("[DEBUG] Parse error, skipping line\n");
#endif
            char *next_line = memchr(p, '\n', remain);
            if (next_line) {
                int skip = next_line - p + 1;
                p += skip;
                remain -= skip;
                *processed += skip;
                continue;
            } else {
                break;
            }
        }
    }

#ifdef DEBUG
    printf("[DEBUG] Protocol processed %d commands, total response %d bytes\n", cmd_count, total);
#endif

    return total;
}
/*--------------------------------------------------------------------------
 * Storage engine lifecycle
 *--------------------------------------------------------------------------*/
int init_kvengine(void) {
#ifdef DEBUG
    printf("[DEBUG] Initializing KV engine (hash table)\n");
#endif
    memset(&global_hash, 0, sizeof(global_hash));
    return kvs_hash_create(&global_hash);
}

void dest_kvengine(void) {
#ifdef DEBUG
    printf("[DEBUG] Destroying KV engine\n");
#endif
    kvs_hash_destroy(&global_hash);
}

/*--------------------------------------------------------------------------
 * Main function
 *--------------------------------------------------------------------------*/
#ifndef TEST_MODE
int main(int argc, char *argv[]) {
    if (argc < 2) return -1;
    int port = atoi(argv[1]);

#ifdef DEBUG
    printf("[DEBUG] Starting server on port %d\n", port);
#endif

    init_kvengine();

#if ENABLE_REPL
#ifdef DEBUG
    printf("[DEBUG] Replication enabled\n");
#endif
    kvs_replication_init();
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--slaveof") == 0 && i + 2 < argc) {
            kvs_slaveof(argv[i+1], atoi(argv[i+2]));
            i += 2;
        }
    }
#endif

#if ENABLE_PERSIST
#ifdef DEBUG
    printf("[DEBUG] Persistence enabled\n");
#endif
    kvs_persist_init("../data/kvstore.aof", "../data/kvstore.rdb");
    // Load RDB data into hash table
    kvs_hash_load_rdb(&global_hash, RDB_FILE);
    // Replay AOF
    if (strlen(AOF_FILE) > 0) load_aof_file(AOF_FILE);
#endif

#ifdef DEBUG
    printf("[DEBUG] Entering reactor loop\n");
#endif
    reactor_start(port, kvs_protocol);

    dest_kvengine();

    return 0;
}
#endif
