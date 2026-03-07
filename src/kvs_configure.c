#include "../include/kvs_configure.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>
#include <unistd.h>
#include <libgen.h>

kvs_config_t g_config;

void kvs_config_set_default(void) {
    g_config.port = 6379;
    g_config.log_level = LOG_LEVEL_INFO;

    g_config.persist_mode = PERSIST_MIXED;
    strcpy(g_config.rdb_file, "../data/kvstore.rdb");
    g_config.rdb_save_interval = 300;
    g_config.rdb_min_changes = 100;
    g_config.rdb_save_on_shutdown = true;
    strcpy(g_config.aof_file, "../data/kvstore.aof");
    g_config.aof_rewrite_size = 1;
    g_config.aof_auto_rewrite = true;

    g_config.repl_switch = REPL_OFF;
    g_config.repl_role = ROLE_MASTER;
    g_config.master_ip[0] = '\0';
    g_config.master_port = 0;
}

static char* trim(char *str) {
    char *end;
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

static bool parse_bool(const char *value) {
    if (strcasecmp(value, "true") == 0 ||
        strcasecmp(value, "yes") == 0 ||
        strcasecmp(value, "on") == 0 ||
        strcasecmp(value, "1") == 0) {
        return true;
    }
    return false;
}

static log_level_t parse_log_level(const char *value) {
    int level = atoi(value);
    if (level >= 1 && level <= 3) return (log_level_t)level;
    return LOG_LEVEL_INFO;
}

static persist_mode_t parse_persist_mode(const char *value) {
    int mode = atoi(value);
    switch (mode) {
        case 0: return PERSIST_OFF;
        case 1: return PERSIST_AOF_ONLY;
        case 2: return PERSIST_RDB_ONLY;
        case 3: return PERSIST_MIXED;
        default: return PERSIST_MIXED;
    }
}

static server_role_t parse_role(const char *value) {
    if (strcasecmp(value, "master") == 0 || strcmp(value, "0") == 0)
        return ROLE_MASTER;
    else if (strcasecmp(value, "slave") == 0 || strcmp(value, "1") == 0)
        return ROLE_SLAVE;
    return ROLE_MASTER;
}

static repl_switch_t parse_repl_switch(const char *value) {
    return parse_bool(value) ? REPL_ON : REPL_OFF;
}

int kvs_config_load(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        printf("[Config] Cannot open file: %s\n", filename);
        return -1;
    }

    printf("[Config] Loading from: %s\n", filename);

    char line[512];
    char current_section[64] = "";

    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == ';' || *p == '\n' || *p == '\0')
            continue;

        if (*p == '[') {
            char *end = strchr(p, ']');
            if (end) {
                *end = '\0';
                strncpy(current_section, p + 1, sizeof(current_section) - 1);
            }
            continue;
        }

        char *equals = strchr(p, '=');
        if (!equals) continue;

        *equals = '\0';
        char *key = trim(p);
        char *value = trim(equals + 1);

        if (strcmp(current_section, "server") == 0) {
            if (strcmp(key, "port") == 0) {
                g_config.port = atoi(value);
            } else if (strcmp(key, "log_level") == 0) {
                g_config.log_level = parse_log_level(value);
            }
        }
        else if (strcmp(current_section, "persist") == 0) {
            if (strcmp(key, "mode") == 0) {
                g_config.persist_mode = parse_persist_mode(value);
            } else if (strcmp(key, "rdb_file") == 0) {
                strncpy(g_config.rdb_file, value, sizeof(g_config.rdb_file)-1);
            } else if (strcmp(key, "rdb_save_interval") == 0) {
                g_config.rdb_save_interval = atoi(value);
            } else if (strcmp(key, "rdb_min_changes") == 0) {
                g_config.rdb_min_changes = atoi(value);
            } else if (strcmp(key, "rdb_save_on_shutdown") == 0) {
                g_config.rdb_save_on_shutdown = parse_bool(value);
            } else if (strcmp(key, "aof_file") == 0) {
                strncpy(g_config.aof_file, value, sizeof(g_config.aof_file)-1);
            } else if (strcmp(key, "aof_rewrite_size") == 0) {
                g_config.aof_rewrite_size = atoi(value);
            } else if (strcmp(key, "aof_auto_rewrite") == 0) {
                g_config.aof_auto_rewrite = parse_bool(value);
            }
        }
        else if (strcmp(current_section, "replication") == 0) {
            if (strcmp(key, "enabled") == 0) {
                g_config.repl_switch = parse_repl_switch(value);
            } else if (strcmp(key, "role") == 0) {
                g_config.repl_role = parse_role(value);
            } else if (strcmp(key, "master_ip") == 0) {
                strncpy(g_config.master_ip, value, sizeof(g_config.master_ip)-1);
            } else if (strcmp(key, "master_port") == 0) {
                g_config.master_port = atoi(value);
            }
        }
    }

    fclose(fp);
    return 0;
}

const char* kvs_config_find(void) {
    if (access("./kvstore.conf", F_OK) == 0) {
        return "./kvstore.conf";
    }

    char *home = getenv("HOME");
    if (home) {
        static char path[512];
        snprintf(path, sizeof(path), "%s/.kvstore.conf", home);
        if (access(path, F_OK) == 0) {
            return path;
        }
    }

    if (access("/etc/kvstore.conf", F_OK) == 0) {
        return "/etc/kvstore.conf";
    }

    return NULL;
}

void kvs_config_print(void) {
    printf("\n========== KVStore Configuration ==========\n");
    printf("Server:\n");
    printf("  port = %d\n", g_config.port);
    printf("  log_level = %d\n", g_config.log_level);

    printf("Persistence:\n");
    printf("  mode = %d\n", g_config.persist_mode);
    printf("  rdb_file = %s\n", g_config.rdb_file);
    printf("  rdb_save_interval = %d\n", g_config.rdb_save_interval);
    printf("  rdb_min_changes = %d\n", g_config.rdb_min_changes);
    printf("  rdb_save_on_shutdown = %s\n", g_config.rdb_save_on_shutdown ? "true" : "false");
    printf("  aof_file = %s\n", g_config.aof_file);
    printf("  aof_rewrite_size = %d MB\n", g_config.aof_rewrite_size);
    printf("  aof_auto_rewrite = %s\n", g_config.aof_auto_rewrite ? "true" : "false");

    printf("Replication:\n");
    printf("  enabled = %s\n", g_config.repl_switch == REPL_ON ? "true" : "false");
    printf("  role = %s\n", g_config.repl_role == ROLE_MASTER ? "master" : "slave");
    if (g_config.repl_role == ROLE_SLAVE) {
        printf("  master_ip = %s\n", g_config.master_ip);
        printf("  master_port = %d\n", g_config.master_port);
    }
    printf("============================================\n\n");
}

void kvs_log(log_level_t level, const char *format, ...) {
    if (level > g_config.log_level) return;

    va_list args;
    va_start(args, format);

    switch (level) {
        case LOG_LEVEL_INFO:
            printf("[INFO] ");
            break;
        case LOG_LEVEL_WARN:
            printf("[WARN] ");
            break;
        case LOG_LEVEL_DEBUG:
            printf("[DEBUG] ");
            break;
        default:
            break;
    }
    vprintf(format, args);
    va_end(args);
}