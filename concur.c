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

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <concur.h>

cc_heap_t cc_heap[CC_TOTAL_HEAPS];

cc_lock_t cc_count_lock;
cc_lock_t cc_arena_lock;
cc_lock_t cc_chunk_hdr_lock;
cc_lock_t cc_large_chunk_lock;
cc_lock_t cc_small_chunk_lock;

cc_localmem_t cc_chunk_hdr_mem;
cc_localmem_t cc_arena_hdr_mem;

cc_size_t cc_max_mem;
cc_size_t cc_total_mem;
cc_size_t cc_oversize_mem;

cc_ssize_t cc_lock_mem = -1;

cc_queue_t cc_arena_hdr_free_list;

int cc_num_heaps = CC_NUM_HEAPS;
int cc_initialized;
int cc_oversize_chunk_cnt;
int cc_large_arena_cnt;
int cc_small_arena_cnt;
int cc_sizeclass_cnt;
int cc_large_sizeclass_cnt;
int cc_small_sizeclass_cnt;
int cc_large_sizeclass_map[CC_LARGE_SIZECLASS_CNT];
int cc_small_sizeclass_map[CC_SMALL_SIZECLASS_CNT];

long long cc_arena_allocs;
long long cc_arena_frees;

int cc_alloc(cc_size_t, void **);
int cc_alloc_chunk_hdr_list(cc_heap_t *, cc_chunk_t **);
int cc_alloc_global_chunk(int, int, cc_chunk_t **);
int cc_alloc_spare_chunk(int, int, cc_chunk_t **);
int cc_alloc_spare_chunk_heap(int, int, cc_chunk_t **);
int cc_free_spare_chunk_heap(int, cc_chunk_t *);
int cc_heap_alloc(int, int, void **, int);
int cc_init(void);

void cc_prevbucket(int, int *);
void cc_free_chunk_hdr(int, cc_chunk_t *);
void cc_free_spare_chunk(int, cc_chunk_t *);
void cc_free_global_chunk(cc_chunk_t *);
void cc_default_err_func(char *, ...);

int (*cc_heapsel_func)(void) = cc_default_heapsel_func;
void (*cc_err_func)(char *, ...) = cc_default_err_func;


/* External. */
void *
cc_malloc(cc_size_t size)
{
	int ret;
	void *p;

	if(size == 0) {
		errno = EINVAL;
		return NULL;
	}

	ret = cc_alloc(size, &p);
	if(ret != 0) {
		errno = ret;
		return NULL;
	}

	return p;
}


/* External. */
void *
cc_calloc(cc_size_t nmemb, cc_size_t size)
{
	int ret;
	void *p;
	cc_size_t realsize;

	if((nmemb == 0) || (size == 0))
		return NULL;

	realsize = nmemb * size;

	ret = cc_alloc(realsize, &p);
	if(ret != 0) {
		errno = ret;
		return NULL;
	}

	memset(p, 0, realsize);

	return p;
}


/* External. */
void *
cc_realloc(void *ptr, cc_size_t size)
{
	int ret;
	void *p;
	cc_chunk_t *ch;
	cc_size_t offset;
	cc_size_t realsize;

	if(ptr == NULL)
		return cc_malloc(size);

	if(size == 0) {
		cc_free(ptr);
		return NULL;
	}

	ch = *(cc_chunk_t **)((char *)ptr - CC_HDRSZ);

	if(ch->ch_magic != CC_CHUNK_MAGIC) {
		cc_err_func("cc_realloc: bad magic number 0x%x != 0x%x\n",
		    ch->ch_magic, CC_CHUNK_MAGIC);
		errno = EINVAL;
		return NULL;
	}

	offset = (cc_size_t)((char *)ptr - ch->ch_ptr) % ch->ch_division_size;
	realsize = ch->ch_division_size - offset;

	/* If the existing buffer is large enough, we're done. */
	if(size <= realsize)
		return ptr;

	/*
	 * If this buffer was obtained with memalign or the like, we may be
	 * able to simply shift the data back to the beginning of the buffer
	 * to avoid allocating a new buffer. We don't need to keep the
	 * alignment - they apparently don't care about alignment since they
	 * called this function instead of getting a larger buffer with
	 * memalign.
	 */
	if((offset > CC_HDRSZ) && ((size + CC_HDRSZ) <= ch->ch_division_size)) {
		memmove(((char *)ptr - offset),
		    (const void *)((char *)ptr - CC_HDRSZ),
		    (realsize + CC_HDRSZ));

		return((char *)ptr - offset);
	}

	ret = cc_alloc(size, &p);
	if(ret != 0) {
		errno = ret;
		return NULL;
	}

	memcpy(p, ptr, realsize);
	cc_free(ptr);

	return p;
}


/* External. */
void *
cc_memalign(cc_size_t alignment, cc_size_t size)
{
	int ret;
	char *p;
	cc_size_t shift;

	if((size == 0) || (alignment == 0) || (alignment % CC_HDRSZ))
		return NULL;

	size += alignment - 1;

	ret = cc_alloc(size, (void **)&p);
	if(ret != 0) {
		errno = ret;
		return NULL;
	}

	if((cc_size_t)p % alignment) {
		shift = alignment - ((cc_size_t)p % alignment);
		memmove((void *)(p + shift - CC_HDRSZ), (void *)(p - CC_HDRSZ),
		    CC_HDRSZ);
		p += shift;
	}

	return((void *)p);
}


/* External. */
void *
cc_valloc(cc_size_t size)
{
	return cc_memalign(sysconf(_SC_PAGESIZE), size);
}


/* External. */
void
cc_free(void *ptr)
{
	int i;
	int lockcnt;
	int heapnum;
	char **base;
	cc_chunk_t *ch;
	cc_class_t *sc;
	cc_lock_t *locks[CC_TOTAL_HEAPS];

	ch = *(cc_chunk_t **)((char *)ptr - CC_HDRSZ);

	if(ch->ch_magic != CC_CHUNK_MAGIC) {
		cc_err_func("cc_free: bad magic number 0x%x != 0x%x\n",
		    ch->ch_magic, CC_CHUNK_MAGIC);
		return;
	}

	/* If it's an oversize chunk, just return it to the arena. */
	if(ch->ch_size_class == CC_SIZECLASS_OVERSIZE) {
		cc_lock(&cc_count_lock);
		cc_total_mem -= ch->ch_chunk_size;
		cc_oversize_mem -= ch->ch_chunk_size;
		cc_oversize_chunk_cnt--;
		cc_unlock(&cc_count_lock);

		cc_arena_free(ch->ch_chunk_size, ch->ch_ptr, NULL);
		cc_free_chunk_hdr(CC_GLOBAL_HEAP, ch);

		return;
	}

	/*
	 * Find the base address. It will be shifted at least CC_HDRSZ bytes,
	 * perhaps more if memaligned.
	 */
	base = (char **)((cc_size_t)ptr -
	    ((cc_size_t)((char *)ptr - ch->ch_ptr) % ch->ch_division_size));

	/*
	 * Figure out which heap the chunk is on. First we try looking on
	 * all of the heaps without locking. If we find it, we then lock the
	 * size class for that heap and check again (it might have moved in
	 * the tiny window). While this is technically a race, we will win it
	 * 99.999% of the time. If we fail to find it, we'll do a more
	 * deterministic search later. This optimization saves a lot of wasted
	 * cycles.
	 */
	for(i = 0; i < CC_TOTAL_HEAPS; i++) {
		if(ch->ch_owner_heap[i]) {
			sc = &cc_heap[i].hp_size_class[ch->ch_size_class];
			cc_lock(&sc->sc_lock);

			if(ch->ch_owner_heap[i]) {
				locks[0] = &sc->sc_lock;
				lockcnt = 1;
				break;
			}

			cc_unlock(&sc->sc_lock);
		}
	}

	/*
	 * If we didn't find the right heap in our previous attempt, we try
	 * finding it by locking all of the heaps in order until we do find
	 * it. This is guaranteed to work if the supplied address is valid.
	 */
	if(i >= CC_TOTAL_HEAPS) {
		for(i = 0; i < CC_TOTAL_HEAPS; i++) {
			sc = &cc_heap[i].hp_size_class[ch->ch_size_class];
			locks[i] = &sc->sc_lock;

			cc_lock(&sc->sc_lock);

			if(ch->ch_owner_heap[i])
				break;
		}

		lockcnt = i + 1;

		if(i >= CC_TOTAL_HEAPS) {
			for(i = lockcnt - 1; i >= 0; i--)
				cc_unlock(locks[i]);

			cc_err_func("cc_free: owner heap not found for "
			    "chunk 0x%lx 0x%lx\n", (unsigned long)ch,
			    (unsigned long)base);

			return;
		}
	}

	heapnum = i;

	/* Ensure that we haven't freed too many buffers to this chunk. */
	if(ch->ch_division_free_cnt >= ch->ch_division_total_cnt) {
		for(i = lockcnt - 1; i >= 0; i--)
			cc_unlock(locks[i]);

		cc_err_func(
		    "cc_free: attempt to free pointer to full chunk: 0x%lx\n",
		    (unsigned long)base);

		return;
	}

	/* Put the newly freed buffer on the division freelist. */
	*base = ch->ch_division_list;
	ch->ch_division_list = (char *)base;

	/* Account for the return of the buffer. */
	ch->ch_division_list_cnt++;
	ch->ch_division_free_cnt++;
	sc->sc_division_free_cnt++;

	/*
	 * Remove the chunk from the fullness group list and figure out
	 * where it should go now. The chunk will have at least one free
	 * division now, so it should never end up on the empty list.
	 */
	cc_dq(&ch->ch_fullness_q);

	switch(ch->ch_fullness_grp) {
	case CC_FULLNESS_GRP_EMPTY:
		sc->sc_empty_chunk_cnt--;
		break;

	case CC_FULLNESS_GRP_MAX:
		sc->sc_full_chunk_cnt--;
		break;
	}

	if(ch->ch_division_free_cnt >= ch->ch_division_total_cnt) {
		ch->ch_fullness_grp = CC_FULLNESS_GRP_MAX;
	}
	else {
		ch->ch_fullness_grp = CC_FULLNESS_GRP_CNT *
		    ch->ch_division_free_cnt / ch->ch_division_total_cnt;
	}

	/*
	 * If the chunk is totally full put it in the spare pool for
	 * repurposing (if not in the process of being moved to another heap).
	 * If the chunk is not on the global heap and it's
	 * nearly full, put it there if the local heap has too many full
	 * chunks, enough free memory, or if the sizeclass hasn't been emptied
	 * out too much.
	 * Otherwise just put it back on the local heap.
	 */
	if(ch->ch_division_free_cnt >= ch->ch_division_total_cnt) {
		/* Put completely full chunks back in spare pool. */
		sc->sc_division_total_cnt -= sc->sc_divisions_per_chunk;
		sc->sc_division_free_cnt -= ch->ch_division_free_cnt;
		sc->sc_chunk_cnt--;

		if((heapnum != CC_GLOBAL_HEAP) &&
		    (sc->sc_division_free_cnt <= 0))
			sc->sc_empty_cnt++;

		ch->ch_owner_heap[heapnum] = 0;
		cc_free_spare_chunk(heapnum, ch);
	}
	else if((heapnum != CC_GLOBAL_HEAP) &&
	    (ch->ch_fullness_grp == CC_FULLNESS_GRP_MAX) &&
	    (((sc->sc_full_chunk_cnt + 1) > CC_MAX_FULL_CHUNKS) ||
	    (sc->sc_division_free_cnt > (ch->ch_division_free_cnt +
	    sc->sc_division_free_threshold)) ||
	    (sc->sc_empty_cnt < CC_MAX_SERIAL_EMPTIES))) {
		/* Put back on global heap. */
		sc->sc_division_total_cnt -= sc->sc_divisions_per_chunk;
		sc->sc_division_free_cnt -= ch->ch_division_free_cnt;
		sc->sc_chunk_cnt--;

		if(sc->sc_division_free_cnt <= 0)
			sc->sc_empty_cnt++;

		ch->ch_owner_heap[heapnum] = 0;
		cc_free_global_chunk(ch);
	}
	else {
		/* Put back on local heap. */
		cc_nq_front(
		    &sc->sc_fullness_group_list[(int)ch->ch_fullness_grp],
		    &ch->ch_fullness_q);

		if(ch->ch_fullness_grp == CC_FULLNESS_GRP_MAX)
			sc->sc_full_chunk_cnt++;
	}

	for(i = lockcnt - 1; i >= 0; i--)
		cc_unlock(locks[i]);
}


/* External. */
void
cc_maintain(int nice)
{
	int i;
	int j;
	int k;
	int ret;
	int *cnt;
	cc_lock_t *lp;
	cc_heap_t *hp;
	cc_chunk_t *ch;
	cc_class_t *sc;
	cc_queue_t *iq;
	cc_queue_t *head;

	if(!cc_initialized) {
		ret = cc_init();
		if(ret != 0)
			return;
	}

	/*
	 * Here we loop through all fullness groups of all sizeclasses of all
	 * heaps for chunks which have free space but have not been used for
	 * allocations in some time. When we find a chunk which seems
	 * underutilized, we move it to the global heap for later redistribution
	 * to the next heap that needs memory.
	 *
	 * We measure chunk activity with counters. Each time this function is
	 * called, each chunk "inactive" counter is incremented. When memory
	 * is allocated from a chunk, the counter is reset to zero. The next
	 * time this function is called, any chunks that still have an inactive
	 * counter greater than zero haven't been touched since we last looked.
	 * Those chunks are candidates for migration to the global heap.
	 * 
	 * We migrate chunks more readily in lower fullness groups. Chunks with
	 * more free memory are less desirable to keep around if they are not
	 * being actively used. Chunks in higher fullness groups will not be
	 * migrated until they have been idle for multiple calls to this
	 * function.
	 *
	 * This is an expensive operation and should not be called too
	 * regularly. Depending on how active your process is, this function
	 * should probably not be called more often than minutes or tens of
	 * minutes between calls.
	 */
	for(i = 0; i < CC_NUM_HEAPS; i++) {
		for(j = 0; j < cc_sizeclass_cnt; j++) {
			sc = &cc_heap[i].hp_size_class[j];
			cc_lock(&sc->sc_lock);

			for(k = 0; k < CC_FULLNESS_GRP_CNT; k++) {
				head = &sc->sc_fullness_group_list[k];
				iq = cc_nextq(head);

				while(iq != head) {
					ch = (cc_chunk_t *)cc_get_assocq(iq);
					iq = cc_nextq(iq);

					if(ch->ch_inactive > k) {
						cc_dq(&ch->ch_fullness_q);

						switch(ch->ch_fullness_grp) {
						case CC_FULLNESS_GRP_EMPTY:
							sc->sc_empty_chunk_cnt--;
							break;

						case CC_FULLNESS_GRP_MAX:
							sc->sc_full_chunk_cnt--;
							break;
						}

						/* Put back on global heap. */
						sc->sc_division_total_cnt -=
						    sc->sc_divisions_per_chunk;
						sc->sc_division_free_cnt -=
						    ch->ch_division_free_cnt;
						sc->sc_chunk_cnt--;

						if(sc->sc_division_free_cnt
						    <= 0) {
							sc->sc_empty_cnt++;
						}

						ch->ch_inactive = 0;
						ch->ch_owner_heap[i] = 0;
						cc_free_global_chunk(ch);
					}
					else {
						ch->ch_inactive++;
					}
				}
			}

			cc_unlock(&sc->sc_lock);

			if(nice)
				cc_sched_yield();
		}
	}

	/*
	 * Check heaps for spare chunks that have been sitting around
	 * for too long. Move them to the global heap. This will allow
	 * them to be reused more readily, and will also allow unused
	 * arenas to get freed that otherwise would have been locked up.
	 */
	for(i = 0; i < CC_NUM_HEAPS; i++) {
		hp = &cc_heap[i];

		for(j = 0; j < 2; j++) {
			if(j == 0) {
				head = &hp->hp_small_chunk_list;
				cnt = &hp->hp_small_chunk_cnt;
				lp = &hp->hp_small_chunk_lock;
			}
			else {
				head = &hp->hp_large_chunk_list;
				cnt = &hp->hp_large_chunk_cnt;
				lp = &hp->hp_large_chunk_lock;
			}

			cc_lock(lp);

			iq = cc_nextq(head);

			while(iq != head) {
				ch = (cc_chunk_t *)cc_get_assocq(iq);
				iq = cc_nextq(iq);

				if(ch->ch_inactive > 0) {
					(*cnt)--;
					cc_dq(&ch->ch_fullness_q);
					ch->ch_inactive = 0;
					ch->ch_owner_heap[i] = 0;

					(void)cc_free_spare_chunk_heap(
					    CC_GLOBAL_HEAP, ch);
				}
				else {
					ch->ch_inactive++;
				}
			}

			cc_unlock(lp);

			if(nice)
				cc_sched_yield();
		}
	}
}


/* External. */
int
cc_malloc_size(void *ptr, cc_size_t *realsize)
{
	cc_chunk_t *ch;
	cc_size_t offset;

	ch = *(cc_chunk_t **)((char *)ptr - CC_HDRSZ);

	if(ch->ch_magic != CC_CHUNK_MAGIC) {
		cc_err_func("cc_malloc_size: bad magic number 0x%x != 0x%x\n",
		    ch->ch_magic, CC_CHUNK_MAGIC);

		errno = EINVAL;
		return EINVAL;
	}

	offset = (cc_size_t)((char *)ptr - ch->ch_ptr) % ch->ch_division_size;
	*realsize = ch->ch_division_size - offset;

	return 0;
}


/* External. */
int
cc_malloc_good_size(cc_size_t size, cc_size_t *goodsize)
{
	int ret;
	int sizeclass;
	size_t fullsize;
	cc_class_t *sc;

	if(!cc_initialized) {
		ret = cc_init();
		if(ret != 0)
			return ret;
	}

	fullsize = size + CC_HDRSZ;

	if(fullsize > CC_LARGE_ALLOC_LIMIT) {
		/* Round up to a page. */
		*goodsize = size + cc_sys_pagesize -
		    (fullsize % cc_sys_pagesize);
	}
	else {
		sizeclass = CC_SIZECLASS(fullsize);
		sc = &cc_heap[0].hp_size_class[sizeclass];
		*goodsize = sc->sc_division_size - CC_HDRSZ;
	}

	return 0;
}



/* External. */
/* Not threadsafe. Call only when singlethreaded (i.e. at startup). */
int
cc_set_heapsel_func(int (*func)(void))
{
	cc_heapsel_func = func;
	return 0;
}


/* External. */
/* Not threadsafe. Call only when singlethreaded (i.e. at startup). */
int
cc_set_num_heaps(int num_heaps)
{
	if((num_heaps > CC_NUM_HEAPS) || (num_heaps < 1))
		cc_num_heaps = CC_NUM_HEAPS;
	else
		cc_num_heaps = num_heaps;

	return 0;
}


/* External. */
/* Not threadsafe. Call only when singlethreaded (i.e. at startup). */
int
cc_set_err_func(void (*func)(char *, ...))
{
	cc_err_func = func;
	return 0;
}


void
cc_default_err_func(char *fmt, ...)
{
	/* Empty stub. */
}


/* External. */
int
cc_set_max_mem(cc_size_t size)
{
	cc_lock(&cc_count_lock);
	cc_max_mem = size;
	cc_unlock(&cc_count_lock);

	return 0;
}


/* External. */
void
cc_get_max_mem(cc_size_t *max_mem)
{
	cc_lock(&cc_count_lock);
	*max_mem = cc_max_mem;
	cc_unlock(&cc_count_lock);
}


/* External. */
void
cc_get_total_mem(cc_size_t *total_mem)
{
	cc_lock(&cc_count_lock);
	*total_mem = cc_total_mem;
	cc_unlock(&cc_count_lock);
}


/* External. */
void
cc_get_oversize_mem(cc_size_t *oversize_mem)
{
	cc_lock(&cc_count_lock);
	*oversize_mem = cc_oversize_mem;
	cc_unlock(&cc_count_lock);
}


/* External. */
/* Not threadsafe. Call only when singlethreaded (i.e. at startup). */
int
cc_set_lock_mem(cc_ssize_t lock_mem)
{
	cc_lock_mem = lock_mem;
	return 0;
}


int
cc_init(void)
{
	int i;
	int j;
	int k;
	int ret;
	int classcnt;
	int divcnt;
	int next_divcnt;
	cc_heap_t *hp;
	cc_class_t *sc;
	cc_class_t *tsc;
	cc_size_t divsz;
	cc_size_t next_divsz;

	CC_INIT_LOCK;

	if(cc_initialized) {
		CC_INIT_UNLOCK;
		return 0;
	}

	cc_plat_init();

	cc_lm_init(&cc_chunk_hdr_mem, 0, 0);
	cc_lm_init(&cc_arena_hdr_mem, 0, 0);
	cc_lock_init(&cc_arena_lock);
	cc_lock_init(&cc_count_lock);
	cc_initq(&cc_arena_hdr_free_list, &cc_arena_hdr_free_list);

	classcnt = 0;
	next_divcnt = 0;
	divsz = CC_SMALL_GRAINSZ;
	divcnt = CC_SMALL_CHUNKSZ / divsz;

	for(i = 0; i < CC_SMALL_SIZECLASS_CNT; i++) {
		next_divsz = CC_SMALL_GRAINSZ * (i + 2);
		next_divcnt = CC_SMALL_CHUNKSZ / next_divsz;

		if(next_divcnt != divcnt)
			classcnt++;

		divsz = next_divsz;
		divcnt = next_divcnt;
	}

	next_divcnt = 0;
	divsz = CC_LARGE_GRAINSZ * (1 + CC_LARGE_CLASS_OFFSET);
	divcnt = CC_LARGE_CHUNKSZ / divsz;

	for(i = 0; i < CC_LARGE_SIZECLASS_CNT; i++) {
		next_divsz = CC_LARGE_GRAINSZ * (i + 2 + CC_LARGE_CLASS_OFFSET);
		next_divcnt = CC_LARGE_CHUNKSZ / next_divsz;

		if(next_divcnt != divcnt)
			classcnt++;

		divsz = next_divsz;
		divcnt = next_divcnt;
	}

	ret = cc_arena_alloc((classcnt * sizeof(cc_class_t) * CC_TOTAL_HEAPS),
	    (void **)&tsc, 0, 0, CC_ARENA_RAWALLOC);

	if(ret != 0) {
		CC_INIT_UNLOCK;
		return ret;
	}

	for(i = 0; i < CC_TOTAL_HEAPS; i++) {
		cc_heap[i].hp_size_class = tsc;
		tsc += classcnt;
	}

	next_divcnt = 0;
	divsz = CC_SMALL_GRAINSZ;
	divcnt = CC_SMALL_CHUNKSZ / divsz;

	for(i = 0; i < CC_SMALL_SIZECLASS_CNT; i++) {
		next_divsz = CC_SMALL_GRAINSZ * (i + 2);
		next_divcnt = CC_SMALL_CHUNKSZ / next_divsz;

		cc_small_sizeclass_map[i] = cc_sizeclass_cnt;

		if(next_divcnt != divcnt) {
			for(j = 0; j < CC_TOTAL_HEAPS; j++) {
				sc =
				    &cc_heap[j].hp_size_class[cc_sizeclass_cnt];
				sc->sc_chunk_size = CC_SMALL_CHUNKSZ;
				sc->sc_division_size = divsz;
				sc->sc_divisions_per_chunk = divcnt;

				sc->sc_division_free_threshold =
				    (sc->sc_divisions_per_chunk *
				    CC_CHUNK_FREE_THRESHOLD) / 100;

				if(sc->sc_division_free_threshold <= 0)
					sc->sc_division_free_threshold = 1;
			}

			cc_sizeclass_cnt++;
			cc_small_sizeclass_cnt++;
		}

		divsz = next_divsz;
		divcnt = next_divcnt;
	}

	next_divcnt = 0;
	divsz = CC_LARGE_GRAINSZ * (1 + CC_LARGE_CLASS_OFFSET);
	divcnt = CC_LARGE_CHUNKSZ / divsz;

	for(i = 0; i < CC_LARGE_SIZECLASS_CNT; i++) {
		next_divsz = CC_LARGE_GRAINSZ * (i + 2 + CC_LARGE_CLASS_OFFSET);
		next_divcnt = CC_LARGE_CHUNKSZ / next_divsz;

		cc_large_sizeclass_map[i] = cc_sizeclass_cnt;

		if(next_divcnt != divcnt) {
			for(j = 0; j < CC_TOTAL_HEAPS; j++) {
				sc =
				    &cc_heap[j].hp_size_class[cc_sizeclass_cnt];
				sc->sc_chunk_size = CC_LARGE_CHUNKSZ;
				sc->sc_division_size = divsz;
				sc->sc_divisions_per_chunk = divcnt;

				sc->sc_division_free_threshold =
				    (sc->sc_divisions_per_chunk *
				    CC_CHUNK_FREE_THRESHOLD) / 100;

				if(sc->sc_division_free_threshold <= 0)
					sc->sc_division_free_threshold = 1;
			}

			cc_sizeclass_cnt++;
			cc_large_sizeclass_cnt++;
		}

		divsz = next_divsz;
		divcnt = next_divcnt;
	}

	for(i = 0; i < CC_TOTAL_HEAPS; i++) {
		hp = &cc_heap[i];

		cc_lock_init(&hp->hp_chunk_hdr_lock);
		cc_lock_init(&hp->hp_large_chunk_lock);
		cc_lock_init(&hp->hp_small_chunk_lock);

		cc_initq(&hp->hp_chunk_hdr_list, hp);
		cc_initq(&hp->hp_large_chunk_list, hp);
		cc_initq(&hp->hp_small_chunk_list, hp);

		for(j = 0; j < cc_sizeclass_cnt; j++) {
			sc = &hp->hp_size_class[j];

			cc_lock_init(&sc->sc_lock);
			cc_initq(&sc->sc_empty_list, sc);

			for(k = 0; k < CC_FULLNESS_GRP_CNT; k++)
				cc_initq(&sc->sc_fullness_group_list[k], sc);
		}
	}

	cc_initialized++;
	CC_INIT_UNLOCK;

	return 0;
}


int
cc_alloc_arena_hdr(cc_arena_t **uca)
{
	int ret;
	char *p;
	cc_arena_t *ca;
	cc_queue_t *iq;
	cc_size_t realsize;

	cc_lock(&cc_arena_lock);

	if(!cc_emptyq(&cc_arena_hdr_free_list)) {
		iq = cc_nextq(&cc_arena_hdr_free_list);
		ca = (cc_arena_t *)cc_get_assocq(iq);
		cc_dq(iq);

		ret = 0;
	}
	else {
		ret = cc_lm_alloc(&cc_arena_hdr_mem, sizeof(cc_arena_t),
		    (void **)&ca);

		if(ret != 0) {
			ret = cc_arena_alloc(CC_ARENA_HDR_CHUNKSZ, (void **)&p,
			    &realsize, 0, CC_ARENA_RAWALLOC);

			if(ret == 0) {
				cc_lock(&cc_count_lock);
				cc_total_mem += realsize;
				cc_unlock(&cc_count_lock);

				cc_lm_init(&cc_arena_hdr_mem, realsize, p);
				(void)cc_lm_alloc(&cc_arena_hdr_mem,
				    sizeof(cc_arena_t), (void **)&ca);
			}
		}
	}

	if(ret == 0) {
		*uca = ca;
		cc_set_assocq(&ca->ca_free_list, ca);

		cc_lock(&cc_count_lock);
		cc_arena_allocs++;
		cc_unlock(&cc_count_lock);
	}

	cc_unlock(&cc_arena_lock);

	return ret;
}


void
cc_free_arena_hdr(cc_arena_t *ca)
{
	cc_lock(&cc_arena_lock);
	cc_nq(&cc_arena_hdr_free_list, &ca->ca_free_list);

	cc_lock(&cc_count_lock);
	cc_arena_frees++;
	cc_unlock(&cc_count_lock);

	cc_unlock(&cc_arena_lock);
}


int
cc_alloc_chunk_hdr(int heapnum, cc_chunk_t **uch)
{
	int ret;
	char *p;
	cc_heap_t *hp;
	cc_chunk_t *ch;
	cc_size_t realsize;

	hp = &cc_heap[heapnum];

	ret = cc_alloc_chunk_hdr_list(hp, uch);
	if(ret == 0)
		return ret;

	if(heapnum != CC_GLOBAL_HEAP) {
		hp = &cc_heap[CC_GLOBAL_HEAP];

		ret = cc_alloc_chunk_hdr_list(hp, uch);
		if(ret == 0)
			return ret;
	}

	cc_lock(&cc_chunk_hdr_lock);

	ret = cc_lm_alloc(&cc_chunk_hdr_mem, sizeof(cc_chunk_t), (void **)&ch);
	if(ret != 0) {
		ret = cc_arena_alloc(CC_CHUNKT_CHUNKSZ, (void **)&p,
		    &realsize, 0, CC_ARENA_RAWALLOC);

		if(ret == 0) {
			cc_lock(&cc_count_lock);
			cc_total_mem += realsize;
			cc_unlock(&cc_count_lock);

			cc_lm_init(&cc_chunk_hdr_mem, realsize, p);
			(void)cc_lm_alloc(&cc_chunk_hdr_mem,
			    sizeof(cc_chunk_t), (void **)&ch);
		}
	}

	cc_unlock(&cc_chunk_hdr_lock);

	if(ret == 0) {
		cc_set_assocq(&ch->ch_fullness_q, ch);
		memset(ch->ch_owner_heap, 0, sizeof(ch->ch_owner_heap));
		ch->ch_magic = CC_CHUNK_MAGIC;
		*uch = ch;
	}

	return ret;
}


void
cc_free_chunk_hdr(int heapnum, cc_chunk_t *ch)
{
	cc_heap_t *hp;
	cc_heap_t *ghp;

	ch->ch_magic = 0;
	hp = &cc_heap[heapnum];
	cc_lock(&hp->hp_chunk_hdr_lock);

	/* If the local list is full, put the struct on the global freelist. */
	if((heapnum != CC_GLOBAL_HEAP) &&
	    (hp->hp_chunk_hdr_cnt >= CC_MAX_CHUNKT_CNT)) {
		ghp = &cc_heap[CC_GLOBAL_HEAP];

		cc_lock(&ghp->hp_chunk_hdr_lock);
		cc_nq_front(&ghp->hp_chunk_hdr_list, &ch->ch_fullness_q);
		ghp->hp_chunk_hdr_cnt++;
		cc_unlock(&ghp->hp_chunk_hdr_lock);
	}
	else {
		cc_nq_front(&hp->hp_chunk_hdr_list, &ch->ch_fullness_q);
		hp->hp_chunk_hdr_cnt++;
	}

	cc_unlock(&hp->hp_chunk_hdr_lock);
}


int
cc_alloc_chunk_hdr_list(cc_heap_t *hp, cc_chunk_t **uch)
{
	cc_queue_t *iq;

	cc_lock(&hp->hp_chunk_hdr_lock);

	if(hp->hp_chunk_hdr_cnt <= 0) {
		cc_unlock(&hp->hp_chunk_hdr_lock);
		return ENOMEM;
	}

	iq = cc_nextq(&hp->hp_chunk_hdr_list);
	if(iq == &hp->hp_chunk_hdr_list) {
		cc_unlock(&hp->hp_chunk_hdr_lock);
		return ENOMEM;
	}

	cc_dq(iq);
	hp->hp_chunk_hdr_cnt--;
	cc_unlock(&hp->hp_chunk_hdr_lock);

	*uch = (cc_chunk_t *)cc_get_assocq(iq);
	(*uch)->ch_magic = CC_CHUNK_MAGIC;

	return 0;
}


int
cc_alloc(cc_size_t size, void **ptr)
{
	int i;
	int j;
	int ret;
	int heapnum;
	int sizeclass;
	int max_sizeclass;
	cc_chunk_t *ch;

	if(!cc_initialized) {
		ret = cc_init();
		if(ret != 0)
			return ret;
	}

	heapnum = cc_heapsel_func();

	size += CC_HDRSZ;

	/* Use raw allocation from the arena for large buffers. */
	if(size > CC_LARGE_ALLOC_LIMIT) {
		ret = cc_alloc_chunk_hdr(heapnum, &ch);
		if(ret != 0)
			return ret;

		ret = cc_arena_alloc(size, (void **)&ch->ch_ptr,
		    &ch->ch_chunk_size, 0, CC_ARENA_RAWALLOC);

		if(ret != 0) {
			cc_free_chunk_hdr(heapnum, ch);
			return ret;
		}

		cc_lock(&cc_count_lock);
		cc_total_mem += ch->ch_chunk_size;
		cc_oversize_mem += ch->ch_chunk_size;
		cc_oversize_chunk_cnt++;
		cc_unlock(&cc_count_lock);

		ch->ch_division_size = ch->ch_chunk_size;
		ch->ch_size_class = CC_SIZECLASS_OVERSIZE;

		*(cc_chunk_t **)ch->ch_ptr = ch;
		*ptr = ch->ch_ptr + CC_HDRSZ;

		return 0;
	}

	sizeclass = CC_SIZECLASS(size);

	/* Get memory from our local heap, growing it if necessary. */
	ret = cc_heap_alloc(heapnum, sizeclass, ptr,
	    (CC_HEAP_GROWOK | CC_HEAP_OBSERVE_LIMIT));

	if(ret == 0)
		return ret;

	/*
	 * We are out of new space. We attempt to raid the other
	 * heaps for memory of this size class.
	 */
	for(i = 0; i < CC_NUM_HEAPS; i++) {
		if(i != heapnum) {
			ret = cc_heap_alloc(i, sizeclass, ptr, 0);
			if(ret == 0)
				return ret;
		}
	}

	/*
	 * We still haven't found any available memory.
	 * Start looking for memory in larger size classes in all heaps.
	 */
	if(sizeclass < cc_small_sizeclass_cnt)
		max_sizeclass = cc_small_sizeclass_cnt;
	else
		max_sizeclass = cc_sizeclass_cnt;

	for(i = sizeclass + 1; i < max_sizeclass; i++) {
		for(j = 0; j < CC_NUM_HEAPS; j++) {
			if(j != heapnum) {
				ret = cc_heap_alloc(j, i, ptr, 0);
				if(ret == 0)
					return ret;
			}
		}
	}

	/* One last ditch to get memory, ignoring limits. */
	cc_lock(&cc_count_lock);

	if(cc_max_mem > 0) {
		cc_unlock(&cc_count_lock);

		ret = cc_heap_alloc(heapnum, sizeclass, ptr, CC_HEAP_GROWOK);
		if(ret == 0)
			return ret;
	}
	else {
		cc_unlock(&cc_count_lock);
	}

	/* Bummer, no memory available. */

	return ENOMEM;
}


int
cc_heap_alloc(int heapnum, int sizeclass, void **ptr, int flags)
{
	int i;
	int ret;
	char **base;
	cc_arena_t *ca;
	cc_class_t *sc;
	cc_chunk_t *ch;
	cc_queue_t *iq;
	cc_queue_t *head;

	sc = &cc_heap[heapnum].hp_size_class[sizeclass];
	cc_lock(&sc->sc_lock);

	if(sc->sc_division_free_cnt > 0) {
		/*
		 * We didn't have to get a new chunk for this allocation.
		 * Note that by zeroing the empty count.
		 */
		sc->sc_empty_cnt = 0;

		/*
		 * Start looking for memory in the emptiest chunks.
		 * We do this so that more full chunks will tend to fill up and
		 * be returned to the global heap.
		 */
		for(i = 0; i < CC_FULLNESS_GRP_CNT; i++) {
			head = &sc->sc_fullness_group_list[i];
			iq = cc_nextq(head);
			if(iq != head) {
				ch = (cc_chunk_t *)cc_get_assocq(iq);
				break;
			}
		}

		/*
		 * Remove the selected chunk from the fullness list.
		 * It will later be placed at the front of whatever list
		 * it belongs in after giving up a memory division.
		 */
		cc_dq(iq);

		if(ch->ch_fullness_grp == CC_FULLNESS_GRP_MAX)
			sc->sc_full_chunk_cnt--;
	}
	else {
		if(!(flags & CC_HEAP_GROWOK)) {
			cc_unlock(&sc->sc_lock);
			return ENOMEM;
		}

		ret = cc_alloc_global_chunk(heapnum, sizeclass, &ch);
		if(ret != 0) {
			ret = cc_alloc_spare_chunk(heapnum, sizeclass, &ch);
			if(ret != 0) {
				cc_lock(&cc_count_lock);

				if((flags & CC_HEAP_OBSERVE_LIMIT) &&
				    (cc_max_mem > 0) &&
				    (cc_total_mem >= cc_max_mem)) {
					cc_unlock(&cc_count_lock);
					cc_unlock(&sc->sc_lock);
					return ENOMEM;
				}

				cc_unlock(&cc_count_lock);

				/* No available chunks, get a new chunk. */
				ret = cc_alloc_chunk_hdr(heapnum, &ch);
				if(ret != 0) {
					cc_unlock(&sc->sc_lock);
					return ret;
				}

				ret = cc_arena_alloc(sc->sc_chunk_size,
				    (void **)&ch->ch_ptr, &ch->ch_chunk_size,
				    &ca, 0);

				if(ret != 0) {
					cc_free_chunk_hdr(heapnum, ch);
					cc_unlock(&sc->sc_lock);
					return ret;
				}

				cc_lock(&cc_arena_lock);
				ca->ca_chunk[ca->ca_current_chunk_cnt] = ch;
				ca->ca_current_chunk_cnt++;

				cc_lock(&cc_count_lock);
				cc_total_mem += ch->ch_chunk_size;

				/* If this is a new arena, count it. */
				if(ca->ca_current_chunk_cnt == 1) {
					if(ca->ca_is_large)
						cc_large_arena_cnt++;
					else
						cc_small_arena_cnt++;
				}

				cc_unlock(&cc_count_lock);
				cc_unlock(&cc_arena_lock);

				ch->ch_size_class = sizeclass;
				ch->ch_arena = ca;
			}

			/* Initialize the new chunk. */
			ch->ch_curptr = ch->ch_ptr;
			ch->ch_division_free_cnt = sc->sc_divisions_per_chunk;
			ch->ch_division_total_cnt = sc->sc_divisions_per_chunk;
			ch->ch_division_list_cnt = 0;
			ch->ch_division_size = sc->sc_division_size;
			ch->ch_size_class = sizeclass;
			ch->ch_fullness_grp = CC_FULLNESS_GRP_MAX;
			ch->ch_inactive = 0;
		}

		ch->ch_owner_heap[heapnum] = 1;

		sc->sc_chunk_cnt++;
		sc->sc_division_total_cnt += sc->sc_divisions_per_chunk;
		sc->sc_division_free_cnt += ch->ch_division_free_cnt;
	}

	/*
	 * Get a free buffer from the free list if possible. We want to
	 * reuse the most recently freed buffers. Otherwise, grab a buffer
	 * from the previously unused chunk memory.
	 */
	if(ch->ch_division_list_cnt > 0) {
		base = (char **)ch->ch_division_list;
		ch->ch_division_list = *(char **)ch->ch_division_list;
		ch->ch_division_list_cnt--;
	}
	else {
		base = (char **)ch->ch_curptr;
		ch->ch_curptr += sc->sc_division_size;
	}

	ch->ch_division_free_cnt--;
	sc->sc_division_free_cnt--;

	/*
	 * If the chunk is now empty, put it on the empty list.
	 * Otherwise, figure out which fullness group it belongs to and put
	 * it on the front of the list.
	 */
	if(ch->ch_division_free_cnt <= 0) {
		ch->ch_fullness_grp = CC_FULLNESS_GRP_EMPTY;
		sc->sc_empty_chunk_cnt++;
		cc_nq_front(&sc->sc_empty_list, &ch->ch_fullness_q);
	}
	else {
		ch->ch_fullness_grp = CC_FULLNESS_GRP_CNT *
		    ch->ch_division_free_cnt / ch->ch_division_total_cnt;

		if(ch->ch_fullness_grp == CC_FULLNESS_GRP_MAX)
			sc->sc_full_chunk_cnt++;

		cc_nq_front(
		    &sc->sc_fullness_group_list[(int)ch->ch_fullness_grp],
		    &ch->ch_fullness_q);
	}

	/* Mark this chunk as active (zero) so it doesn't get housecleaned. */
	ch->ch_inactive = 0;

	cc_unlock(&sc->sc_lock);

	*base = (char *)ch;
	*ptr = (char *)base + CC_HDRSZ;

	return 0;
}


int
cc_alloc_global_chunk(int heapnum, int sizeclass, cc_chunk_t **uch)
{
	int i;
	cc_class_t *sc;
	cc_chunk_t *ch;
	cc_queue_t *iq;
	cc_queue_t *head;

	sc = &cc_heap[CC_GLOBAL_HEAP].hp_size_class[sizeclass];
	cc_lock(&sc->sc_lock);

	for(i = 0; i < CC_FULLNESS_GRP_CNT; i++) {
		head = &sc->sc_fullness_group_list[i];
		iq = cc_nextq(head);

		if(iq == head)
			continue;

		cc_dq(iq);
		ch = (cc_chunk_t *)cc_get_assocq(iq);
		ch->ch_owner_heap[CC_GLOBAL_HEAP] = 0;

		if(ch->ch_fullness_grp == CC_FULLNESS_GRP_MAX)
			sc->sc_full_chunk_cnt--;

		sc->sc_chunk_cnt--;
		sc->sc_division_total_cnt -= sc->sc_divisions_per_chunk;
		sc->sc_division_free_cnt -= ch->ch_division_free_cnt;

		break;
	}

	cc_unlock(&sc->sc_lock);

	if(i >= CC_FULLNESS_GRP_CNT)
		return ENOMEM;

	*uch = ch;

	return 0;
}


void
cc_free_global_chunk(cc_chunk_t *ch)
{
	cc_class_t *sc;

	sc = &cc_heap[CC_GLOBAL_HEAP].hp_size_class[ch->ch_size_class];
	cc_lock(&sc->sc_lock);

	if(ch->ch_fullness_grp == CC_FULLNESS_GRP_MAX)
		sc->sc_full_chunk_cnt++;

	sc->sc_chunk_cnt++;
	sc->sc_division_total_cnt += sc->sc_divisions_per_chunk;
	sc->sc_division_free_cnt += ch->ch_division_free_cnt;
	ch->ch_owner_heap[CC_GLOBAL_HEAP] = 1;

	cc_nq_front(&sc->sc_fullness_group_list[ch->ch_fullness_grp],
	    &ch->ch_fullness_q);

	cc_unlock(&sc->sc_lock);
}


int
cc_alloc_spare_chunk(int heapnum, int sizeclass, cc_chunk_t **uch)
{
	int ret;

	ret = cc_alloc_spare_chunk_heap(heapnum, sizeclass, uch);
	if(ret == 0)
		return 0;

	ret = cc_alloc_spare_chunk_heap(CC_GLOBAL_HEAP, sizeclass, uch);

	return ret;
}


int
cc_alloc_spare_chunk_heap(int heapnum, int sizeclass, cc_chunk_t **uch)
{
	cc_lock_t *lp;
	cc_heap_t *hp;
	cc_queue_t *iq;
	cc_queue_t *head;
	cc_chunk_t *ch;

	hp = &cc_heap[heapnum];

	if((sizeclass < cc_small_sizeclass_cnt) &&
	    (CC_LARGE_CHUNKSZ != CC_SMALL_CHUNKSZ)) {
		lp = &hp->hp_small_chunk_lock;
		cc_lock(lp);

		if(hp->hp_small_chunk_cnt <= 0) {
			cc_unlock(lp);
			return ENOMEM;
		}

		head = &hp->hp_small_chunk_list;
		hp->hp_small_chunk_cnt--;
	} else {
		lp = &hp->hp_large_chunk_lock;
		cc_lock(lp);

		if(hp->hp_large_chunk_cnt <= 0) {
			cc_unlock(lp);
			return ENOMEM;
		}

		head = &hp->hp_large_chunk_list;
		hp->hp_large_chunk_cnt--;
	}

	iq = cc_nextq(head);
	cc_dq(iq);
	ch = (cc_chunk_t *)cc_get_assocq(iq);
	ch->ch_inactive = 0;

	if(heapnum == CC_GLOBAL_HEAP) {
		cc_lock(&cc_arena_lock);
		ch->ch_arena->ca_free_chunk_cnt--;
		cc_unlock(&cc_arena_lock);
	}

	*uch = (cc_chunk_t *)cc_get_assocq(iq);

	cc_unlock(lp);

	return 0;
}


void
cc_free_spare_chunk(int heapnum, cc_chunk_t *ch)
{
	int ret;

	/* Try freeing to the local heap. This fails if it's too full. */
	ret = cc_free_spare_chunk_heap(heapnum, ch);
	if(ret == 0)
		return;

	/* Free to the global heap. This always works. */
	(void)cc_free_spare_chunk_heap(CC_GLOBAL_HEAP, ch);
}


int
cc_free_spare_chunk_heap(int heapnum, cc_chunk_t *ch)
{
	int i;
	int *cnt;
	char *p;
	cc_lock_t *lp;
	cc_heap_t *hp;
	cc_queue_t *head;
	cc_arena_t *ca;
	size_t size;

	hp = &cc_heap[heapnum];

	if((ch->ch_size_class < cc_small_sizeclass_cnt) &&
	    (CC_LARGE_CHUNKSZ != CC_SMALL_CHUNKSZ)) {
		cnt = &hp->hp_small_chunk_cnt;
		head = &hp->hp_small_chunk_list;
		lp = &hp->hp_small_chunk_lock;
		size = CC_SMALL_ARENA_CHUNKSZ;
		cc_lock(lp);
	} else {
		cnt = &hp->hp_large_chunk_cnt;
		head = &hp->hp_large_chunk_list;
		lp = &hp->hp_large_chunk_lock;
		size = CC_LARGE_ARENA_CHUNKSZ;
		cc_lock(lp);
	}

	if((heapnum != CC_GLOBAL_HEAP) && (*cnt >= CC_MAX_CHUNK_CNT)) {
		cc_unlock(lp);
		return EFBIG;
	}

	(*cnt)++;
	cc_nq_front(head, &ch->ch_fullness_q);

	/* Check to see if this arena can be freed. */
	if(heapnum == CC_GLOBAL_HEAP) {
		ca = ch->ch_arena;

		cc_lock(&cc_arena_lock);
		ca->ca_free_chunk_cnt++;

		/*
		 * If all chunks in this arena are free, take them
		 * all off the free list and free the arena.
		 */
		if(ca->ca_free_chunk_cnt >= ca->ca_max_chunk_cnt) {
			for(i = 0; i < ca->ca_max_chunk_cnt; i++)
				cc_dq(&ca->ca_chunk[i]->ch_fullness_q);

			(*cnt) -= ca->ca_max_chunk_cnt;

			cc_lock(&cc_count_lock);
			cc_total_mem -= size;

			if(ca->ca_is_large)
				cc_large_arena_cnt--;
			else
				cc_small_arena_cnt--;

			cc_unlock(&cc_count_lock);
			cc_unlock(&cc_arena_lock);
			cc_unlock(lp);

			for(i = 0; i < ca->ca_max_chunk_cnt; i++)
				cc_free_chunk_hdr(heapnum, ca->ca_chunk[i]);

			cc_arena_free(size, ca->ca_ptr, ca);

			return 0;
		}

		cc_unlock(&cc_arena_lock);
	}

	cc_unlock(lp);

	return 0;
}


int
cc_lm_alloc(cc_localmem_t *lm, cc_size_t size, void **ptr)
{
	if(size > lm->lm_size)
		return ENOMEM;

	*ptr = lm->lm_ptr;
	lm->lm_ptr += size;
	lm->lm_size -= size;

	return 0;
}


void
cc_lm_init(cc_localmem_t *lm, cc_size_t size, void *ptr)
{
	lm->lm_ptr = ptr;
	lm->lm_size = size;
}


/* Metrics. */

/* External. */
void
cc_get_stats(cc_stat_t *cstat)
{
	int i;
	int j;
	int n;
	cc_size_t s;
	cc_heap_t *hp;

	memset(cstat, 0, sizeof(cc_stat_t));

	/* Lock everything. */
	for(i = 0; i < cc_sizeclass_cnt; i++)
		for(j = 0; j < CC_TOTAL_HEAPS; j++)
			cc_lock(&cc_heap[j].hp_size_class[i].sc_lock);

	for(i = 0; i < CC_TOTAL_HEAPS; i++) {
		hp = &cc_heap[i];
		cc_lock(&hp->hp_small_chunk_lock);
		cc_lock(&hp->hp_large_chunk_lock);
	}

	cstat->cs_heap_cnt = CC_TOTAL_HEAPS;
	cstat->cs_size_class_cnt = cc_sizeclass_cnt;

	cstat->cs_global_small_chunk_free_cnt =
	    cc_heap[CC_GLOBAL_HEAP].hp_small_chunk_cnt;
	cstat->cs_global_large_chunk_free_cnt =
	    cc_heap[CC_GLOBAL_HEAP].hp_large_chunk_cnt;
	cstat->cs_global_chunk_free_cnt =
	    cc_heap[CC_GLOBAL_HEAP].hp_small_chunk_cnt +
	    cc_heap[CC_GLOBAL_HEAP].hp_large_chunk_cnt;
	cstat->cs_global_chunk_hdr_free_cnt =
	    cc_heap[CC_GLOBAL_HEAP].hp_chunk_hdr_cnt;

	cc_lock(&cc_count_lock);
	cstat->cs_max_mem = cc_max_mem;
	cstat->cs_total_mem = cc_total_mem;
	cstat->cs_oversize_mem = cc_oversize_mem;
	cstat->cs_oversize_chunk_cnt = cc_oversize_chunk_cnt;
	cstat->cs_large_arena_cnt = cc_large_arena_cnt;
	cstat->cs_small_arena_cnt = cc_small_arena_cnt;
	cstat->cs_arena_cnt = cc_large_arena_cnt + cc_small_arena_cnt;
	cstat->cs_arena_allocs = cc_arena_allocs;
	cstat->cs_arena_frees = cc_arena_frees;
	cc_unlock(&cc_count_lock);

	/* Tally up the stats. */
	for(i = 0; i < CC_TOTAL_HEAPS; i++) {
		hp = &cc_heap[i];

		cstat->cs_total_small_chunk_free_cnt +=
		    cc_heap[i].hp_small_chunk_cnt;
		cstat->cs_total_large_chunk_free_cnt +=
		    cc_heap[i].hp_large_chunk_cnt;
		cstat->cs_total_chunk_free_cnt +=
		    cc_heap[i].hp_small_chunk_cnt +
		    cc_heap[i].hp_large_chunk_cnt;
		cstat->cs_total_chunk_hdr_free_cnt +=
		    cc_heap[i].hp_chunk_hdr_cnt;

		cstat->cs_heap[i].hs_chunk_hdr_free_cnt =
		    cc_heap[i].hp_chunk_hdr_cnt;
		cstat->cs_heap[i].hs_small_chunk_free_cnt =
		    cc_heap[i].hp_small_chunk_cnt;
		cstat->cs_heap[i].hs_large_chunk_free_cnt =
		    cc_heap[i].hp_large_chunk_cnt;

		for(j = 0; j < cc_sizeclass_cnt; j++) {
			cstat->cs_heap[i].hs_size_class[j].cl_full_chunk_cnt =
			    hp->hp_size_class[j].sc_full_chunk_cnt;

			cstat->cs_heap[i].hs_size_class[j].cl_empty_chunk_cnt =
			    hp->hp_size_class[j].sc_empty_chunk_cnt;

			cstat->cs_heap[i].hs_size_class[j].cl_empty_cnt =
			    hp->hp_size_class[j].sc_empty_cnt;

			s = hp->hp_size_class[j].sc_chunk_size;
			cstat->cs_heap[i].hs_size_class[j].cl_chunk_size = s;

			n = hp->hp_size_class[j].sc_chunk_cnt;
			cstat->cs_chunk_cnt += n;
			cstat->cs_heap[i].hs_chunk_cnt += n;
			cstat->cs_heap[i].hs_size_class[j].cl_chunk_cnt = n;

			cstat->cs_total_heap_mem += s * n;
			cstat->cs_heap[i].hs_total_heap_mem += s * n;
			cstat->cs_heap[i].hs_size_class[j].cl_total_heap_mem
			    = s * n;

			if(j < cc_small_sizeclass_cnt)
				cstat->cs_small_heap_mem += s * n;
			else
				cstat->cs_large_heap_mem += s * n;

			s = hp->hp_size_class[j].sc_division_size;
			cstat->cs_heap[i].hs_size_class[j].cl_division_size = s;

			n = hp->hp_size_class[j].sc_division_total_cnt;
			cstat->cs_total_division_total_cnt += n;
			cstat->cs_heap[i].hs_division_total_cnt += n;
			cstat->cs_heap[i].hs_size_class[j].cl_division_total_cnt
			    = n;

			n = hp->hp_size_class[j].sc_division_free_cnt;
			cstat->cs_total_division_free_cnt += n;
			cstat->cs_heap[i].hs_division_free_cnt += n;
			cstat->cs_heap[i].hs_size_class[j].cl_division_free_cnt
			    = n;

			cstat->cs_total_heap_free_mem += s * n;
			cstat->cs_heap[i].hs_total_heap_free_mem += s * n;
			cstat->cs_heap[i].hs_size_class[j].cl_total_heap_free_mem
			    = s * n;

			if(j < cc_small_sizeclass_cnt)
				cstat->cs_small_heap_free_mem += s * n;
			else
				cstat->cs_large_heap_free_mem += s * n;
		}
	}

	cstat->cs_total_free_mem = cstat->cs_total_heap_free_mem +
	    ((cc_size_t)cstat->cs_total_small_chunk_free_cnt *
	    CC_SMALL_CHUNKSZ) +
	    ((cc_size_t)cstat->cs_total_large_chunk_free_cnt *
	    CC_LARGE_CHUNKSZ);

	cstat->cs_global_division_total_cnt = 
	    cstat->cs_heap[CC_GLOBAL_HEAP].hs_division_total_cnt;

	cstat->cs_global_division_free_cnt = 
	    cstat->cs_heap[CC_GLOBAL_HEAP].hs_division_free_cnt;

	/* Free up all the locks. */
	for(i = (CC_TOTAL_HEAPS - 1); i >= 0; i--) {
		hp = &cc_heap[i];
		cc_unlock(&hp->hp_large_chunk_lock);
		cc_unlock(&hp->hp_small_chunk_lock);
	}

	for(i = (cc_sizeclass_cnt - 1); i >= 0; i--)
		for(j = (CC_TOTAL_HEAPS - 1); j >= 0; j--)
			cc_unlock(&cc_heap[j].hp_size_class[i].sc_lock);
}
