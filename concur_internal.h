/*
 * Concur - an extremely scalable heap manager
 * Author: Steve Scherf (steve@gracenote.com)
 *
 * Copyright (c) Gracenote, Inc. 2007.
 * All rights reserved.
 *
 * This software is distributed under the terms of the Gracenote Open
 * Source License (GOSL) Version 1.0. You should have received a copy
 * of the GOSL along with this program; if not, you can obtain a copy
 * from: http:/www.gracenote.com/corporate/opensource/LICENSE.html
 */

#ifndef __CONCUR_INT_H__
#define __CONCUR_INT_H__

#include <stdarg.h>
#include <concur_queue.h>
#include <concur_plat.h>

#ifndef NULL
#define NULL	(void *)0
#endif

#define CC_LARGE_CHUNKSZ	1048576	/* Must be a multiple of pagesize. */
#define CC_LARGE_ALLOC_LIMIT	524288	/* Must divide into large chunk. */
#define CC_LARGE_ARENA_CHUNKCNT	10
#define CC_LARGE_ARENA_CHUNKSZ	(CC_LARGE_CHUNKSZ * CC_LARGE_ARENA_CHUNKCNT)
#define CC_LARGE_GRAINSZ	4096	/* Must divide into large alloc lim. */
#define CC_LARGE_SIZECLASS_CNT	\
	((CC_LARGE_ALLOC_LIMIT / CC_LARGE_GRAINSZ) - CC_LARGE_CLASS_OFFSET)
#define CC_SMALL_CHUNKSZ	262144	/* Must divide into large chunk. */
#define CC_SMALL_ALLOC_LIMIT	131072	/* Must divide into small chunk. */
#define CC_SMALL_ARENA_CHUNKCNT	50
#define CC_SMALL_ARENA_CHUNKSZ	(CC_SMALL_CHUNKSZ * CC_SMALL_ARENA_CHUNKCNT)
#define CC_SMALL_GRAINSZ	8	/* Must divide into small alloc lim. */
#define CC_SMALL_SIZECLASS_CNT	(CC_SMALL_ALLOC_LIMIT / CC_SMALL_GRAINSZ)
#define CC_SIZECLASS_CNT	\
	(CC_LARGE_SIZECLASS_CNT + CC_SMALL_SIZECLASS_CNT)
#define CC_OVERSIZE_CHUNKSZ	524288	/* Any size; LARGE_ALLOC_LIMIT best. */
#define CC_CHUNKT_CHUNKSZ	(100 * sizeof(cc_chunk_t))
#define CC_ARENA_HDR_CHUNKSZ	(100 * sizeof(cc_arena_t))
#define CC_SIZECLASS_OVERSIZE	-1
#define CC_FULLNESS_GRP_CNT	4
#define CC_FULLNESS_GRP_MAX	(CC_FULLNESS_GRP_CNT - 1)
#define CC_FULLNESS_GRP_EMPTY	-1
#define CC_MAX_SERIAL_EMPTIES	5	/* To avoid pingponging empties. */
#define CC_MAX_FULL_CHUNKS	1	/* Return full chunks if > than this. */
#define CC_CHUNK_FREE_THRESHOLD	50	/* % of a chunk avail before free. */
#define CC_MAX_CHUNKT_CNT	10	/* Max chunk headers on local list. */
#define CC_MAX_CHUNK_CNT	5	/* Max chunks on local list. */
#define CC_NUM_HEAPS		8
#define CC_TOTAL_HEAPS		(CC_NUM_HEAPS + 1)
#define CC_GLOBAL_HEAP		CC_NUM_HEAPS
#define CC_HDRSZ		8

#define CC_LARGE_CLASS_OFFSET \
	((CC_SMALL_ALLOC_LIMIT + CC_LARGE_GRAINSZ - 1) / CC_LARGE_GRAINSZ)
#define CC_SIZECLASS(size) \
	(((size) <= CC_SMALL_ALLOC_LIMIT) ? \
	cc_small_sizeclass_map[((size) - 1) / CC_SMALL_GRAINSZ] : \
	cc_large_sizeclass_map[((size) - 1) / CC_LARGE_GRAINSZ - \
	CC_LARGE_CLASS_OFFSET])

#if CC_LARGE_ARENA_CHUNKCNT > CC_SMALL_ARENA_CHUNKCNT
#define CC_MAX_ARENA_CHUNKCNT CC_LARGE_ARENA_CHUNKCNT
#else
#define CC_MAX_ARENA_CHUNKCNT CC_SMALL_ARENA_CHUNKCNT
#endif

#define CC_ARENA_RAWALLOC	0x00000001

#define CC_HEAP_GROWOK		0x00000001
#define CC_HEAP_OBSERVE_LIMIT	0x00000002


typedef struct concur_sizeclass {
	int sc_chunk_cnt;
	int sc_full_chunk_cnt;
	int sc_empty_chunk_cnt;
	int sc_divisions_per_chunk;
	int sc_division_total_cnt;
	int sc_division_free_cnt;
	int sc_division_free_threshold;
	int sc_empty_cnt;
	cc_size_t sc_division_size;
	cc_size_t sc_chunk_size;
	cc_queue_t sc_fullness_group_list[CC_FULLNESS_GRP_CNT];
	cc_queue_t sc_empty_list;
	cc_lock_t sc_lock;
} cc_class_t;


typedef struct concur_arena {
	unsigned char ca_max_chunk_cnt;
	unsigned char ca_current_chunk_cnt;
	unsigned char ca_free_chunk_cnt;
	unsigned char ca_is_large;
	cc_queue_t ca_free_list;
	char *ca_ptr;
	struct concur_chunk *ca_chunk[CC_MAX_ARENA_CHUNKCNT];
} cc_arena_t;


#define CC_CHUNK_MAGIC	0x2984AFC3

typedef struct concur_chunk {
	int ch_magic;
	short ch_size_class;
	char ch_fullness_grp;
	unsigned char ch_inactive;
	char ch_owner_heap[CC_TOTAL_HEAPS];
	int ch_division_total_cnt;
	int ch_division_free_cnt;
	int ch_division_list_cnt;
	cc_size_t ch_division_size;
	cc_size_t ch_chunk_size;
	char *ch_ptr;
	char *ch_curptr;
	char *ch_division_list;
	cc_arena_t *ch_arena;
	cc_queue_t ch_fullness_q;
} cc_chunk_t;


typedef struct concur_heap {
	int hp_chunk_hdr_cnt;
	int hp_large_chunk_cnt;
	int hp_small_chunk_cnt;
	cc_queue_t hp_chunk_hdr_list;
	cc_queue_t hp_large_chunk_list;
	cc_queue_t hp_small_chunk_list;
	cc_lock_t hp_chunk_hdr_lock;
	cc_lock_t hp_large_chunk_lock;
	cc_lock_t hp_small_chunk_lock;
	cc_class_t *hp_size_class;
} cc_heap_t;


typedef struct concur_localmem {
	char *lm_ptr;
	cc_size_t lm_size;
} cc_localmem_t;


typedef struct concur_classstat {
	int cl_chunk_cnt;
	int cl_full_chunk_cnt;
	int cl_empty_chunk_cnt;
	int cl_empty_cnt;
	cc_size_t cl_chunk_size;
	cc_size_t cl_total_heap_mem;
	cc_size_t cl_total_heap_free_mem;
	cc_size_t cl_division_size;
	int cl_division_total_cnt;
	int cl_division_free_cnt;
} cc_classstat_t;


typedef struct concur_heapstat {
	int hs_chunk_cnt;
	int hs_chunk_hdr_free_cnt;
	int hs_large_chunk_free_cnt;
	int hs_small_chunk_free_cnt;
	int hs_division_total_cnt;
	int hs_division_free_cnt;
	cc_size_t hs_total_heap_mem;
	cc_size_t hs_total_heap_free_mem;
	cc_classstat_t hs_size_class[CC_SIZECLASS_CNT];
} cc_heapstat_t;


typedef struct concur_stat {
	int cs_chunk_cnt;
	int cs_oversize_chunk_cnt;
	int cs_global_chunk_free_cnt;
	int cs_global_large_chunk_free_cnt;
	int cs_global_small_chunk_free_cnt;
	int cs_global_division_total_cnt;
	int cs_global_division_free_cnt;
	int cs_global_chunk_hdr_free_cnt;
	int cs_total_chunk_free_cnt;
	int cs_total_large_chunk_free_cnt;
	int cs_total_small_chunk_free_cnt;
	int cs_total_division_total_cnt;
	int cs_total_division_free_cnt;
	int cs_total_chunk_hdr_free_cnt;
	cc_size_t cs_max_mem;
	cc_size_t cs_total_mem;
	cc_size_t cs_total_free_mem;
	cc_size_t cs_oversize_mem;
	cc_size_t cs_total_heap_mem;
	cc_size_t cs_total_heap_free_mem;
	cc_size_t cs_small_heap_mem;
	cc_size_t cs_small_heap_free_mem;
	cc_size_t cs_large_heap_mem;
	cc_size_t cs_large_heap_free_mem;
	int cs_heap_cnt;
	int cs_size_class_cnt;
	int cs_arena_cnt;
	int cs_large_arena_cnt;
	int cs_small_arena_cnt;
	long long cs_arena_allocs;
	long long cs_arena_frees;
	cc_heapstat_t cs_heap[CC_TOTAL_HEAPS];
} cc_stat_t;


int cc_alloc_arena_hdr(cc_arena_t **);
int cc_arena_alloc(cc_size_t, void **, cc_size_t *, cc_arena_t **, int);
int cc_default_heapsel_func(void);
int cc_lm_alloc(cc_localmem_t *, cc_size_t, void **);

void cc_arena_free(cc_size_t, void *, cc_arena_t *);
void cc_free_arena_hdr(cc_arena_t *);
void cc_lm_init(cc_localmem_t *, cc_size_t, void *);
void cc_sched_yield(void);

extern int cc_num_heaps;
extern int (*cc_heapsel_func)(void);
extern void (*cc_err_func)(char *, ...);
extern cc_ssize_t cc_lock_mem;
extern cc_size_t cc_total_mem;

#endif /* __CONCUR_INT_H__ */
