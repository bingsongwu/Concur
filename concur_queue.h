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

#ifndef _CONCUR_QUEUE_H_
#define _CONCUR_QUEUE_H_

typedef struct cc_queue {
        struct cc_queue *cq_next;
        struct cc_queue *cq_last;
        void *cq_assoc;
} cc_queue_t;


#define cc_initq(cq, assoc) \
{ \
	(cq)->cq_next = (cq); \
	(cq)->cq_last = (cq); \
	(cq)->cq_assoc = (assoc); \
}


#define cc_nq(head, cq) \
{ \
	(cq)->cq_next = (head); \
	(cq)->cq_last = (head)->cq_last; \
	(cq)->cq_last->cq_next = (cq); \
	(head)->cq_last = (cq); \
}


#define cc_nq_front(head, cq) \
{ \
	(cq)->cq_last = (head); \
	(cq)->cq_next = (head)->cq_next; \
	(cq)->cq_next->cq_last = (cq); \
	(head)->cq_next = (cq); \
}


#define cc_dq(cq) \
{ \
	(cq)->cq_next->cq_last = (cq)->cq_last; \
	(cq)->cq_last->cq_next = (cq)->cq_next; \
}


#define cc_nextq(cq) ((cq)->cq_next)
#define cc_lastq(cq) ((cq)->cq_last)
#define cc_emptyq(cq) ((cq)->cq_next == cq)
#define cc_get_assocq(cq) ((cq)->cq_assoc)
#define cc_set_assocq(cq, assoc) ((cq)->cq_assoc = (void *)(long)(assoc))

#endif /* _CONCUR_QUEUE_H_ */
