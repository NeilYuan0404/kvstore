#include "../include/kvs_base.h"
#include "../include/kvs_mmpool.h"  // 添加内存池头文件

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Global memory pool */
static struct mp_pool_s *g_mp_pool = NULL;

/* Initialize global memory pool (call this at program start) */
int kvs_mp_init(size_t pool_size) {
    if (g_mp_pool == NULL) {
        g_mp_pool = mp_create_pool(pool_size);
        return (g_mp_pool != NULL) ? 0 : -1;
    }
    return 0;
}

/* Destroy global memory pool (call this at program exit) */
void kvs_mp_destory(void) {
    if (g_mp_pool) {
        mp_destory_pool(g_mp_pool);
        g_mp_pool = NULL;
    }
}

/* Memory allocation wrappers */
void *kvs_malloc(size_t size) {
    if (g_mp_pool) {
        void *ptr = mp_alloc(g_mp_pool, size);
#ifdef DEBUG        
        printf("[DEBUG] malloc(%zu) = %p\n", size, ptr);
#endif
        return ptr;
    } else {
        
#if ENABLE_JEMALLOC
        return je_malloc(size);
#else
        return malloc(size);
#endif
    }
}

void *kvs_calloc(size_t size) {
    void *ptr = kvs_malloc(size);
    if (ptr) {
        memset(ptr, 0, size);
    }
    return ptr;
}

void kvs_free(void *ptr) {
    if (g_mp_pool) {
#ifdef DEBUG
        printf("[DEBUG] free(%p)\n", ptr);
#endif
        mp_free(g_mp_pool, ptr);
        
    } else {
#if ENABLE_JEMALLOC
        je_free(ptr);
#else    
        free(ptr);
#endif
    }
}


void *kvs_realloc(void *ptr, size_t new_size) {
    if (!ptr) return kvs_malloc(new_size);
    if (new_size == 0) {
        kvs_free(ptr);
        return NULL;
    }
    
#if ENABLE_JEMALLOC
    return je_realloc(ptr, new_size);
#else
    return realloc(ptr, new_size);
#endif
}

/* Get current memory pool (for debugging) */
struct mp_pool_s *kvs_get_mp_pool(void) {
    return g_mp_pool;
}
