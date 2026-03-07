#ifndef __KV_STORE_H__
#define __KV_STORE_H__

#include <stddef.h>

#define ENABLE_JEMALLOC 1
#if ENABLE_JEMALLOC
#define JEMALLOC_NO_RENAME 1
#include <jemalloc/jemalloc.h>
#endif

#define PROTOCOL_UNKNOWN -1
#define PROTOCOL_RESP    0
#define PROTOCOL_TEXT    1

#endif