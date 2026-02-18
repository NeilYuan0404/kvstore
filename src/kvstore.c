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

typedef int (*msg_handler)(char *msg, int length, char *response, int *processed);
extern int reactor_start(unsigned short port, msg_handler handler);
extern bool g_is_loading;

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

    switch (cmd) {
    case CMD_HSET:
        ret = kvs_hash_set(&global_hash, key, val);
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
        char *res = kvs_hash_get(&global_hash, key);
        if (res) {
            len = sprintf(response, "$%d\r\n%s\r\n", (int)strlen(res), res);
        } else {
            len = sprintf(response, "$-1\r\n");  // Null bulk string
        }
        break;
    }

    case CMD_HDEL:
        ret = kvs_hash_del(&global_hash, key);
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
        ret = kvs_hash_mod(&global_hash, key, val);
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
        ret = kvs_hash_exist(&global_hash, key);
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
    if (p == num_start) return -1;            // no digits after '*'
    if (p >= end || *p != '\r') return -1;
    p++;
    if (p >= end || *p != '\n') return -1;
    p++;

    if (argc > maxtok) return -1;

    for (int i = 0; i < argc; i++) {
        if (p >= end || *p != '$') return -1;
        p++;  // skip '$'

        // Parse bulk string length
        int blen = 0;
        char *blen_start = p;
        while (p < end && *p >= '0' && *p <= '9') {
            blen = blen * 10 + (*p - '0');
            p++;
        }
        if (p == blen_start) return -1;       // no digits after '$'
        if (p >= end || *p != '\r') return -1;
        p++;
        if (p >= end || *p != '\n') return -1;
        p++;

        // Check if bulk string data is complete
        if (p + blen + 2 > end) return 0;     // incomplete

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
        p += blen + 2;                         // skip data and trailing \r\n
    }
    return p - msg;
}

/* Main protocol entry: only RESP format is accepted.
 * Processes as many complete commands as possible from the input buffer.
 * Returns total response length (always >= 0), and sets *processed to number of bytes consumed.
 * If a protocol error occurs, returns -1 (caller should close connection).
 */
// 在文件开头添加外部变量声明


int kvs_protocol(char *msg, int length, char *response, int *processed) {
    if (!msg || !response || !processed) return -1;
    if (length <= 0) return 0;
    char *p = msg;
    int remain = length;
    int total = 0;
    char *resp = response;
    *processed = 0;

#ifdef DEBUG
    int cmd_count = 0;
#endif

    while (remain > 0) {
        // 跳过空白字符（如 \r, \n, 空格等），避免空行导致误判
        while (remain > 0 && isspace(*p)) {
            p++;
            remain--;
            (*processed)++;
        }
        if (remain <= 0) break;

        // 如果不是 '*'，说明协议可能出错，尝试找到下一个 '*' 以恢复同步
        if (*p != '*') {
            char *next_star = memchr(p, '*', remain);
            if (next_star) {
                int junk = next_star - p;
#ifdef DEBUG
                printf("[DEBUG] Protocol resync: skipping %d bytes of non-RESP data\n", junk);
#endif
                p = next_star;
                remain -= junk;
                *processed += junk;
                continue;   // 重新开始循环
            } else {
                // 没有找到 '*'，可能是数据错误或数据不足，等待更多数据
                break;
            }
        }

        char *tokens[KVS_MAX_TOKENS] = {0};
        int consumed = parse_resp(p, remain, tokens, KVS_MAX_TOKENS);
        if (consumed > 0) {
            int tokcnt = 0;
            while (tokcnt < KVS_MAX_TOKENS && tokens[tokcnt]) tokcnt++;
#ifdef DEBUG
            cmd_count++;
#endif
            int resp_len = kvs_executor(tokens, tokcnt, resp);
            for (int i = 0; i < tokcnt; i++) {
                if (tokens[i]) kvs_free(tokens[i]);
            }
            if (resp_len > 0) {
                resp += resp_len;
                total += resp_len;
            }
            p += consumed;
            remain -= consumed;
            *processed += consumed;

            // 在AOF重放模式下，只处理一条命令就返回
            if (g_is_loading) {
                break;
            }
        } else if (consumed == 0) {
            // 数据不足，等待更多
            break;
        } else {
            // 解析错误，尝试跳过当前行（直到遇到 '\n'）然后继续
#ifdef DEBUG
            printf("[DEBUG] Parse error, skipping line\n");
#endif
            char *next_line = memchr(p, '\n', remain);
            if (next_line) {
                int skip = next_line - p + 1;
                p += skip;
                remain -= skip;
                *processed += skip;
                // 继续下一轮循环
            } else {
                // 没有找到换行，等待更多数据
                break;
            }
        }
    }

#ifdef DEBUG
    printf("[DEBUG] Protocol processed %d commands, total response %d bytes\n", cmd_count, total);
#endif

    if (total > 0) *resp = '\0';
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
    kvs_hash_destory(&global_hash);
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

    kvs_mp_destory();  
    return 0;
}
#endif
