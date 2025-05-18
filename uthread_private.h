/*
 *   FILE: uthread_private.h
 * AUTHOR: Peter Demoreuille
 *  DESCR: uthreads private stuff.
 *   DATE: Mon Oct  1 10:56:31 2001
 *
 */

#ifndef __uthread_private_h__
#define __uthread_private_h__

#include <pthread.h>

#include "uthread.h"
#include "uthread_queue.h"

/*
 * Initializes the scheduler. This is called from uthread_start.
 */
void uthread_sched_init(void);

/*
 * Will prepare the curlwp and switch into its context.
 * LWP in lwp_switch will looks for another thread and if
 * not found, it parks in lwp_park.
 *
 * Parameters are:
 *     q: if non-null, a queue that the calling thread should be put on (should
 not be runq)
 *     saveonrunq: whether the calling thread should be put on the runq
 *     m: if non-null, a pointer to a mutex that should be unlocked once off
 this thread's stack
 */
void uthread_switch(utqueue_t *q, int saveonrunq, pthread_mutex_t *m);

/*
 * Put newly created thread on runq
 */
void uthread_startonrunq(uthread_id_t, int);

#endif /* __uthread_private_h__ */
