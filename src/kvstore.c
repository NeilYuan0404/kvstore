#include "../include/kvs_base.h"
#include "../include/kvs_hash.h"
#include "../include/kvs_persist.h"
#include "../include/kvs_configure.h"
#ifdef ENABLE_REPL
#include "../include/kvs_replication.h"
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>

extern kvs_hash_t global_hash;
extern int reactor_start(unsigned short port, msg_handler handler);
extern bool g_is_loading;

static const char *command[] = {
    "SET", "GET", "DEL", "MOD", "EXISTS", "SAVE"
};
enum {
    CMD_SET, CMD_GET, CMD_DEL, CMD_MOD, CMD_EXISTS, CMD_SAVE, CMD_COUNT
};

static int append_bulk_string(char *resp, const void *data, size_t len) {
    int n = sprintf(resp, "$%zu\r\n", len);
    memcpy(resp + n, data, len);
    n += len;
    resp[n++] = '\r';
    resp[n++] = '\n';
    return n;
}

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
    case CMD_SET:
        ret = kvs_hash_set(&global_hash, key, key_len, val, val_len);
        if (ret < 0)
            len = sprintf(response, "-ERR internal error\r\n");
        else if (ret == 0) {
            len = sprintf(response, "+OK\r\n");
#if ENABLE_PERSIST
            if (g_config.persist_mode == PERSIST_AOF_ONLY || 
                g_config.persist_mode == PERSIST_MIXED) {
                kvs_aof_append("SET", key, val);
            }
#endif
#if ENABLE_REPL
            if (g_repl.role == KVS_ROLE_MASTER)
                kvs_replication_feed_slaves("SET", key, val);
#endif
        } else
            len = sprintf(response, "+EXIST\r\n");
        break;

    case CMD_GET: {
        size_t res_len;
        void *res = kvs_hash_get(&global_hash, key, key_len, &res_len);
        if (res) {
            len = append_bulk_string(response, res, res_len);
        } else {
            len = sprintf(response, "$-1\r\n");
        }
        break;
    }

    case CMD_DEL:
        ret = kvs_hash_del(&global_hash, key, key_len);
        if (ret < 0)
            len = sprintf(response, "-ERR internal error\r\n");
        else if (ret == 0) {
            len = sprintf(response, "+OK\r\n");
#if ENABLE_PERSIST
            if (g_config.persist_mode == PERSIST_AOF_ONLY || 
                g_config.persist_mode == PERSIST_MIXED) {
                kvs_aof_append("DEL", key, NULL);
            }
#endif
#if ENABLE_REPL
            if (g_repl.role == KVS_ROLE_MASTER)
                kvs_replication_feed_slaves("DEL", key, NULL);
#endif
        } else
            len = sprintf(response, "$-1\r\n");
        break;

    case CMD_MOD:
        ret = kvs_hash_mod(&global_hash, key, key_len, val, val_len);
        if (ret < 0)
            len = sprintf(response, "-ERR internal error\r\n");
        else if (ret == 0) {
            len = sprintf(response, "+OK\r\n");
#if ENABLE_PERSIST
            if (g_config.persist_mode == PERSIST_AOF_ONLY || 
                g_config.persist_mode == PERSIST_MIXED) {
                kvs_aof_append("MOD", key, val);
            }
#endif
#if ENABLE_REPL
            if (g_repl.role == KVS_ROLE_MASTER)
                kvs_replication_feed_slaves("MOD", key, val);
#endif
        } else
            len = sprintf(response, "$-1\r\n");
        break;

    case CMD_EXISTS:
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

static int parse_resp(char *msg, int len, char *tokens[], int maxtok) {
    char *p = msg;
    char *end = msg + len;

    if (len < 4 || *p != '*') return -1;
    p++;

    int argc = 0;
    char *num_start = p;
    while (p < end && *p >= '0' && *p <= '9') {
        argc = argc * 10 + (*p - '0');
        p++;
    }
    if (p == num_start) return -1;
    if (p >= end) return 0;
    if (*p != '\r') return -1;
    p++;
    if (p >= end) return 0;
    if (*p != '\n') return -1;
    p++;

    if (argc > maxtok) return -1;

    for (int i = 0; i < argc; i++) {
        if (p >= end) return 0;
        if (*p != '$') return -1;
        p++;

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
        while (remain > 0 && isspace(*p)) {
            p++;
            remain--;
            (*processed)++;
        }
        if (remain <= 0) break;

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

            char *temp = kvs_malloc(64 * 1024);
            if (!temp) {
                for (int i = 0; i < tokcnt; i++) {
                    if (tokens[i]) kvs_free(tokens[i]);
                }
                return -1;
            }
            int resp_len = kvs_executor(tokens, tokcnt, temp);
            kvs_free(temp);

            if (resp_len > resp_remain) {
                *needed = resp_len;
                for (int i = 0; i < tokcnt; i++) {
                    if (tokens[i]) kvs_free(tokens[i]);
                }
                return -2;
            }

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

            if (g_is_loading) break;
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

#ifndef TEST_MODE
int main(int argc, char *argv[]) {
    kvs_config_set_default();

    char *config_file = NULL;
    int cmd_port = 0;
    int cmd_persist_mode = -1;
    char *slaveof_ip = NULL;
    int slaveof_port = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) {
            if (i+1 < argc) config_file = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) {
            if (i+1 < argc) cmd_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--persist-mode") == 0 && i+1 < argc) {
            cmd_persist_mode = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--slaveof") == 0 && i+2 < argc) {
            slaveof_ip = argv[++i];
            slaveof_port = atoi(argv[++i]);
        }
    }

    if (config_file) {
        kvs_config_load(config_file);
    } else {
        const char *default_cfg = kvs_config_find();
        if (default_cfg) {
            kvs_config_load(default_cfg);
        }
    }

    if (cmd_port > 0) g_config.port = cmd_port;
    if (cmd_persist_mode >= 0) g_config.persist_mode = cmd_persist_mode;
    if (slaveof_ip) {
        g_config.repl_switch = REPL_ON;
        g_config.repl_role = ROLE_SLAVE;
        strncpy(g_config.master_ip, slaveof_ip, sizeof(g_config.master_ip)-1);
        g_config.master_port = slaveof_port;
    }

    kvs_config_print();

    init_kvengine();

#if ENABLE_REPL
    kvs_replication_init();
#endif

#if ENABLE_PERSIST
    kvs_persist_init();

    if (g_config.persist_mode == PERSIST_RDB_ONLY ||
        g_config.persist_mode == PERSIST_MIXED) {
        kvs_hash_load_rdb(&global_hash, g_config.rdb_file);
    }
    if (g_config.persist_mode == PERSIST_AOF_ONLY ||
        g_config.persist_mode == PERSIST_MIXED) {
        if (strlen(g_config.aof_file) > 0) load_aof_file(g_config.aof_file);
    }
#endif

    reactor_start(g_config.port, kvs_protocol);

#if ENABLE_PERSIST
    if ((g_config.persist_mode == PERSIST_RDB_ONLY ||
         g_config.persist_mode == PERSIST_MIXED) &&
         g_config.rdb_save_on_shutdown) {
        kvs_rdb_save();
    }
#endif

    dest_kvengine();
    return 0;
}
#endif