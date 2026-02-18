#include<stddef.h>

#define MP_ALIGNMENT       		32
#define MP_PAGE_SIZE			4096
#define MP_MAX_ALLOC_FROM_POOL	(MP_PAGE_SIZE-1)

struct mp_large_s {
	struct mp_large_s *next;
	void *alloc;
};

struct mp_node_s {

	unsigned char *last;
	unsigned char *end;
	
	struct mp_node_s *next;
	size_t failed;
};

struct mp_pool_s {

	size_t max;

	struct mp_node_s *current;
	struct mp_large_s *large;

	struct mp_node_s head[0];

};

struct mp_pool_s *mp_create_pool(size_t size);
void mp_destory_pool(struct mp_pool_s *pool);
void *mp_alloc(struct mp_pool_s *pool, size_t size);
void *mp_nalloc(struct mp_pool_s *pool, size_t size);
void *mp_calloc(struct mp_pool_s *pool, size_t size);
void mp_free(struct mp_pool_s *pool, void *p);
