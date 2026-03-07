#include "../include/kvs_base.h"

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

void *kvs_malloc(size_t size) {
    void *ptr = malloc(size);
#ifdef DEBUG        
    printf("[DEBUG] malloc(%zu) = %p\n", size, ptr);
#endif
    return ptr;
}

void *kvs_calloc(size_t size) {
    void *ptr = calloc(1, size);
    return ptr;
}

void kvs_free(void *ptr) {
#ifdef DEBUG
    printf("[DEBUG] free(%p)\n", ptr);
#endif
    free(ptr);
}

void *kvs_realloc(void *ptr, size_t new_size) {
    if (!ptr) return kvs_malloc(new_size);
    if (new_size == 0) {
        kvs_free(ptr);
        return NULL;
    }
    return realloc(ptr, new_size);
}