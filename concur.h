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

#ifndef __CONCUR_H__
#define __CONCUR_H__

#include <stdarg.h>
#include <concur_internal.h>

void *cc_malloc(cc_size_t);
void *cc_calloc(cc_size_t, cc_size_t);
void *cc_realloc(void *, cc_size_t);
void *cc_memalign(cc_size_t, cc_size_t);
void *cc_valloc(cc_size_t);
void cc_free(void *);
void cc_get_stats(cc_stat_t *);
void cc_get_total_mem(cc_size_t *);
void cc_get_max_mem(cc_size_t *);
void cc_get_oversize_mem(cc_size_t *);
void cc_maintain(int);

int cc_malloc_good_size(cc_size_t, cc_size_t *);
int cc_malloc_size(void *, cc_size_t *);
int cc_set_num_heaps(int);
int cc_set_heapsel_func(int (*)(void));
int cc_set_lock_mem(cc_ssize_t);
int cc_set_max_mem(cc_size_t);
int cc_set_err_func(void (*)(char *, ...));

#endif /* __CONCUR_H__ */
