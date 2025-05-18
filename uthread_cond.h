/*
 *   FILE: uthread_cond.h
 * AUTHOR: Peter Demoreuille
 *  DESCR: uthreads condition variables
 *   DATE: Mon Oct  1 00:16:54 2001
 *
 */

#ifndef __uthread_cond_h__
#define __uthread_cond_h__

#include <pthread.h>

#include "uthread_queue.h"

struct uthread_mtx;

typedef struct uthread_cond {
    utqueue_t uc_waiters;
    pthread_mutex_t pmut;
} uthread_cond_t;

void uthread_cond_init(uthread_cond_t *);
void uthread_cond_wait(uthread_cond_t *, struct uthread_mtx *);
void uthread_cond_signal(uthread_cond_t *);
void uthread_cond_broadcast(uthread_cond_t *);

#endif /* __uthread_cond_h__ */
