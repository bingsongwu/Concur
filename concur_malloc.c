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

#include <stdlib.h>
#include <errno.h>
#include <concur.h>
#include <unistd.h>

#if HAVE_MALLOC_H
#include <malloc.h>
#endif


void *
malloc(size_t __size)
{
#ifndef NO_ZERO_MALLOCS
	/* Support lame code that mallocs zero byte buffers. */
	if(__size == 0)
		__size = 1;
#endif

	return cc_malloc((cc_size_t)__size);
}

void
free(void *__ptr)
{
#ifndef NO_NULL_FREES
	/* Support lame code that frees null pointers. */
	if(__ptr == NULL)
		return;
#endif
	cc_free(__ptr);
}

void *
calloc(size_t __nmemb, size_t __size)
{
	return cc_calloc((cc_size_t)__nmemb, (cc_size_t)__size);
}

void *
realloc(void *__ptr, size_t __size)
{
#ifndef NO_ZERO_MALLOCS
	/* Support lame code that mallocs zero byte buffers. */
	if(__size == 0)
		__size = 1;
#endif

	return cc_realloc(__ptr, (cc_size_t)__size);
}

#if HAVE_REALLOCF
void *
reallocf(void *__ptr, size_t __size)
{
	void *rslt;

	rslt = cc_realloc(__ptr, (cc_size_t)__size);

	/* If allocation failed, free the pointer anyway. */
	if((rslt == NULL) && (__ptr != NULL))
		cc_free(__ptr);

	return rslt;
}
#endif

#if HAVE_POSIX_MEMALIGN
int
posix_memalign(void **__ptr, size_t __alignment, size_t __size)
{
	errno = 0;
	*__ptr = cc_memalign((cc_size_t)__alignment, (cc_size_t)__size);

	return errno;
}
#endif

#if HAVE_MEMALIGN
void *
memalign(size_t __alignment, size_t __size)
{
	return cc_memalign((cc_size_t)__alignment, (cc_size_t)__size);
}
#endif

#if HAVE_VALLOC
void *
valloc(size_t __size)
{
	return cc_valloc((cc_size_t)__size);
}
#endif

#if HAVE_PVALLOC
void *
pvalloc(size_t __size)
{
	cc_size_t psize;
	cc_size_t resid;
	cc_size_t pagesize;

	pagesize = getpagesize();
	psize = __size;
	resid = pagesize % psize;
	if(resid)
		psize += pagesize - resid;

	return cc_valloc(psize);
}
#endif

#if HAVE_MALLOC_SIZE
size_t
malloc_size(void *__ptr)
{
	int ret;
	size_t size;

	ret = cc_malloc_size(__ptr, (cc_size_t *)&size);
	if(ret != 0)
		return 0;

	return size;
}
#endif

#if HAVE_MALLOC_GOOD_SIZE
size_t
malloc_good_size(size_t __size)
{
	int ret;
	size_t goodsize;

	ret = cc_malloc_good_size(__size, &goodsize);
	if(ret != 0)
		return __size;

	return goodsize;
}
#endif

#if HAVE_MALLOC_USABLE_SIZE
size_t
malloc_usable_size(void *__ptr)
{
	int ret;
	size_t size;

	ret = cc_malloc_size(__ptr, (cc_size_t *)&size);
	if(ret != 0)
		return 0;

	return size;
}
#endif

#if HAVE_MALLOC_TRIM
int
malloc_trim(size_t __pad)
{
	/* Pretend. */
	return 1;
}
#endif

#if HAVE_MALLINFO
struct mallinfo
mallinfo(void)
{
	/* Return an empty struct. We are not GNU malloc! */
	static struct mallinfo mallinfo;
	return mallinfo;
}
#endif

#if HAVE_MALLINFO_HEAP
struct mallinfo
mallinfo_heap(int heap)
{
	/* Return an empty struct. We are not GNU malloc! */
	static struct mallinfo mallinfo;
	return mallinfo;
}
#endif

#if HAVE_MALLOPT
int
mallopt(int __param, int __val)
{
	/* Pretend. */
	return 0;
}
#endif

#if HAVE_MALLOC_GET_STATE
void *
malloc_get_state(void)
{
	return((void *)"This is not GNU malloc");
}
#endif

#if HAVE_MALLOC_SET_STATE
int
malloc_set_state(void *__ptr)
{
	return 0;
}
#endif

#if HAVE___DEFAULT_MORECORE
void *
__default_morecore(ptrdiff_t __size)
{
	/* Impossible for us to do anything else. */
	return 0;
}
void *(*__morecore)(ptrdiff_t __size) = __default_morecore;
#endif

#if HAVE___MALLOC_CHECK_INIT
void
__malloc_check_init(void)
{
	/* Do nothing. */
}
void (*__malloc_initialize_hook)(void);
void (*__free_hook)(void *__ptr, __const __malloc_ptr_t);
void *(*__malloc_hook)(size_t __size, __const __malloc_ptr_t);
void *(*__realloc_hook)(void *__ptr, size_t __size, __const __malloc_ptr_t);
void *(*__memalign_hook)(size_t __alignment, size_t __size,
    __const __malloc_ptr_t);
void (*__after_morecore_hook)(void);
#endif
