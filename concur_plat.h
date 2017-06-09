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

#ifndef __CONCUR_PLAT_H__
#define __CONCUR_PLAT_H__

#include <sys/types.h>
#include <stdlib.h>
#include <pthread.h>

#define cc_lock(lkp)		pthread_mutex_lock(lkp)
#define cc_unlock(lkp)		pthread_mutex_unlock(lkp)
#define cc_wait(cop, lkp)	pthread_cond_wait((cop), (lkp))
#define cc_wake(cop)		pthread_cond_broadcast(cop)
#define CC_INIT_LOCK		cc_lock(&cc_init_lock)
#define CC_INIT_UNLOCK		cc_unlock(&cc_init_lock)

typedef pthread_mutex_t cc_lock_t;
typedef pthread_cond_t cc_cond_t;
typedef size_t cc_size_t;
typedef ssize_t cc_ssize_t;

extern cc_lock_t cc_init_lock;
extern cc_size_t cc_sys_pagesize;

/* Not in concur_internal.h in case we want to make it a macro. */
void cc_lock_init(cc_lock_t *);
void cc_cond_init(cc_cond_t *);
void cc_plat_init(void);

#endif /* __CONCUR_PLAT_H__ */
