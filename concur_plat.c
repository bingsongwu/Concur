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
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include "concur.h"


pthread_key_t cc_threadkey;
pthread_mutex_t cc_init_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t cc_heapcnt_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t cc_large_arena_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t cc_small_arena_lock = PTHREAD_MUTEX_INITIALIZER;

cc_localmem_t cc_large_arena_mem;
cc_localmem_t cc_small_arena_mem;

cc_arena_t *cc_large_arena_hdr;
cc_arena_t *cc_small_arena_hdr;

int cc_heapcnt[CC_NUM_HEAPS];
int cc_gotkey;

cc_size_t cc_sys_pagesize;

void cc_key_destruct(void *);
int cc_mmap(cc_size_t, void **);


void
cc_plat_init(void)
{
	cc_lm_init(&cc_large_arena_mem, 0, 0);
	cc_lm_init(&cc_small_arena_mem, 0, 0);

	cc_sys_pagesize = getpagesize();

	if(pthread_key_create(&cc_threadkey, cc_key_destruct) == 0) {
		cc_gotkey++;
	}
	else {
		cc_err_func(
		    "cc_plat_init: failed to create thread key (%d): %s",
		    errno, strerror(errno));
	}
}


void
cc_key_destruct(void *arg)
{
	(void)cc_lock(&cc_heapcnt_lock);

	if(arg == NULL)
		cc_heapcnt[0]--;
	else
		cc_heapcnt[(int)(long)arg - 1]--;

	(void)cc_unlock(&cc_heapcnt_lock);
}


int
cc_default_heapsel_func(void)
{
	int i;
	int heapmin;
	int heapnum;
	void *specval;
		
	/*
	 * Always use heap 0 if we had problems with thread keys.
	 * We still get good concurrency from only one heap.
	 */
	if(!cc_gotkey)
		return 0;

	specval = pthread_getspecific(cc_threadkey);
	if(specval != NULL)
		return((int)(long)specval - 1);

	/* We haven't yet set a heap for this thread. Pick the least used. */
	heapnum = 0;

	(void)cc_lock(&cc_heapcnt_lock);
	heapmin = cc_heapcnt[0];

	/* Find the first empty, or the least used otherwise. */
	for(i = 1; (i < cc_num_heaps) && (heapmin > 0); i++) {
		if(cc_heapcnt[i] < heapmin) {
			heapnum = i;
			heapmin = cc_heapcnt[i];
		}
	}

	cc_heapcnt[heapnum]++;

	(void)cc_unlock(&cc_heapcnt_lock);

	/*
	 * Set the heap for this thread.
	 * Arbitrarily pick heap 0 on error. Shouldn't ever happen...
	 */
	if(pthread_setspecific(cc_threadkey,
	    (const void *)(long)(heapnum + 1)) != 0) {
		(void)cc_lock(&cc_heapcnt_lock);
		cc_heapcnt[heapnum]--;
		cc_heapcnt[0]++;
		(void)cc_unlock(&cc_heapcnt_lock);

		return 0;
	}

	return heapnum;
}


void
cc_lock_init(cc_lock_t *lock)
{
	pthread_mutexattr_t mattr;

	(void)pthread_mutexattr_init(&mattr);
	(void)pthread_mutex_init(lock, &mattr);
	(void)pthread_mutexattr_destroy(&mattr);
}



void
cc_cond_init(cc_cond_t *cond)
{
        pthread_condattr_t cattr;

        (void)pthread_condattr_init(&cattr);
        (void)pthread_cond_init(cond, &cattr);
        (void)pthread_condattr_destroy(&cattr);
}


int
cc_arena_alloc(cc_size_t size, void **ptr, cc_size_t *realsize,
    cc_arena_t **arena, int flags)
{
	int ret;
	int large;
	void *p;
	cc_lock_t *lp;
	cc_arena_t **cap;
	cc_arena_t *ca;
	cc_localmem_t *lm;
	cc_size_t chunksz;

	switch(size) {
	case CC_LARGE_CHUNKSZ:
		lm = &cc_large_arena_mem;
		lp = &cc_large_arena_lock;
		cap = &cc_large_arena_hdr;
		large = 1;
		break;

#if CC_SMALL_CHUNKSZ != CC_LARGE_CHUNKSZ
	case CC_SMALL_CHUNKSZ:
		lm = &cc_small_arena_mem;
		lp = &cc_small_arena_lock;
		cap = &cc_small_arena_hdr;
		large = 0;
		break;
#endif

	default:
		lm = 0;
		lp = 0;
		cap = 0;
		large = 0;
		flags |= CC_ARENA_RAWALLOC;
		break;
	}

	if(flags & CC_ARENA_RAWALLOC) {
		size = ((size + cc_sys_pagesize - 1) / cc_sys_pagesize)
		    * cc_sys_pagesize;

		ret = cc_mmap(size, ptr);
		if(ret != 0)
			return ret;

		if(realsize)
			*realsize = size;

		if(arena)
			*arena = 0;

		return 0;
	}

	cc_lock(lp);

	if(cc_lm_alloc(lm, size, ptr) != 0) {
		ret = cc_alloc_arena_hdr(cap);
		if(ret != 0) {
			cc_unlock(lp);
			return ret;
		}

		ca = *cap;
		ca->ca_current_chunk_cnt = 0;
		ca->ca_free_chunk_cnt = 0;
		ca->ca_is_large = large;

		if(large) {
			ca->ca_max_chunk_cnt = CC_LARGE_ARENA_CHUNKCNT;
			chunksz = CC_LARGE_ARENA_CHUNKSZ;
		}
		else {
			ca->ca_max_chunk_cnt = CC_SMALL_ARENA_CHUNKCNT;
			chunksz = CC_SMALL_ARENA_CHUNKSZ;
		}

		ret = cc_mmap(chunksz, &p);
		if(ret != 0) {
			cc_free_arena_hdr(ca);
			cc_unlock(lp);
			return ret;
		}

		ca->ca_ptr = p;
		cc_lm_init(lm, chunksz, p);
		(void)cc_lm_alloc(lm, size, ptr);
	}
	else {
		ca = *cap;
	}

	cc_unlock(lp);

	if(arena != NULL)
		*arena = ca;

	if(realsize != NULL)
		*realsize = size;

	return 0;
}


void
cc_arena_free(cc_size_t size, void *ptr, cc_arena_t *ca)
{
	int ret;
	int err;

	if(ca != NULL)
		cc_free_arena_hdr(ca);

	ret = munmap(ptr, size);
	if(ret != 0) {
		if(errno == 0)
			err = ENOMEM;
		else
			err = errno;

		cc_err_func("cc_arena_free: free of %lx bytes failed (%d): %s",
		    (unsigned long)size, err, strerror(err));
	}
}


int
cc_mmap(cc_size_t size, void **ptr)
{
	int ret;
	int err;
	void *val;

	val = mmap(0, size, (PROT_READ | PROT_WRITE | PROT_EXEC),
	    (MAP_ANON | MAP_PRIVATE), -1, 0);

	if(val == MAP_FAILED) {
		if(errno == 0)
			err = ENOMEM;
		else
			err = errno;

		cc_err_func("cc_mmap: allocation of %lx bytes failed (%d): %s",
		    (unsigned long)size, err, strerror(err));

		/* Reset errno in case strerror changed it. */
		errno = err;

		return err;
	}

	/*
	 * Accounting for memory locking assumes that mlock() will never
	 * fail when called. If it does, then you may have less locked
	 * memory than total memory when you don't expect it to be that way.
	 * Such is life. More complicated accounting is probably not called
	 * for.
	 */
	if((cc_lock_mem == 0) ||
	    ((cc_lock_mem > 0) && ((cc_total_mem + size) < cc_lock_mem))) {
		ret = mlock(val, size);
		if(ret != 0) {
			/* Not fatal, just a warning of sorts. */
			cc_err_func(
			    "cc_mmap: mlock of %lx bytes failed (%d): %s",
			    (unsigned long)size, errno, strerror(errno));
		}
	}

	*ptr = val;

	return 0;
}


void
cc_sched_yield(void)
{
	sched_yield();
}
