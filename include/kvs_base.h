#ifndef KVS_BASE_H
#define KVS_BASE_H

#include <stddef.h>

/* Memory allocation wrappers */
void *kvs_malloc(size_t size);
void kvs_free(void *ptr);
void *kvs_calloc(size_t size);  
void *kvs_realloc(void *ptr, size_t new_size);
/* Memory pool interface */
int kvs_mp_init(size_t pool_size);
void kvs_mp_destory(void);
void kvs_mp_stats(void);  

#define KVS_MAX_TOKENS 128
#define KVS_MAX_MSG_LEN 1024
#define BUFFER_LENGTH (2 * 1024 * 1024)  

#define ENABLE_JEMALLOC 0
#if ENABLE_JEMALLOC
#define JEMALLOC_NO_RENAME 1
#include <jemalloc/jemalloc.h>
#endif

/* Default macro definitions (can be overridden by compiler) */
#ifndef ENABLE_PERSIST
#define ENABLE_PERSIST 1
#endif
#ifndef ENABLE_REPL
#define ENABLE_REPL 1
#endif



#endif
