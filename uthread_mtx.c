/*
 *   FILE: uthread_mtx.c
 * AUTHOR: Peter Demoreuille
 *  DESCR: userland mutexes
 *   DATE: Sat Sep  8 12:40:00 2001
 *
 * Modifed considerably by Tom Doeppner in support of two-level threading
 *   DATE: Sun Jan 10, 2016 and Jan 2023
 *
 */

#include "uthread_mtx.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "list.h"
#include "uthread.h"
#include "uthread_private.h"
#include "uthread_sched.h"

/*
 * uthread_mtx_init
 *
 * Initialize the fields of the specified mutex.
 */
void uthread_mtx_init(uthread_mtx_t *mtx) {
    mtx->m_owner = NULL;
    utqueue_init(&mtx->m_waiters);
    pthread_mutex_init(&mtx->m_pmut, 0);
}

/*
 * uthread_mtx_lock
 *
 * Lock the mutex. This call will block if it's already locked.  When the
 * thread wakes up from waiting, it should own the mutex (see _unlock()).
 */
void uthread_mtx_lock(uthread_mtx_t *mtx) {
    // TO DO:
    // Recall that uthread_mtx_lock can be called by many uthreads
    // running on different processors at the same time. For this
    // reason you'll need to:
    //
    // 1. Synchronize access to the mtx using its m_pmut field.
    //
    // 2. Ensure that, if the mutex is already locked, neither the current
    // thread is queued nor mtx->m_pmut is unlocked until after the lwp has
    // switched off the current thread's stack. (Consider the last argument of
    // uthread_switch).
    //
    // Suppose instead we first enqueued the running thread, unlocked
    // the mtx->m_pmut, and then switched contexts. This leaves a window
    // for another thread to call uthread_mtx_unlock, dequeue the thread that
    // was just about to switch contexts, and wake it up. This results in
    // a situation where we have the same thread running on two processors
    // at once. We certainly don't want that.

    uthread_nopreempt_on();

    // lock pthread mtx
    pthread_mutex_lock(&mtx->m_pmut);

    if (mtx->m_owner == NULL) {
        // mtx is free, take ownership
        mtx->m_owner = ut_curthr;
        pthread_mutex_unlock(&mtx->m_pmut);
        uthread_nopreempt_off();
    } else {
        // mtx alr locked
        // prep thread for waiting
        ut_curthr->ut_state = UT_WAIT;

        // add thread to wait q, switch context
        // pass mtx->m_pmut to unlocked after switch to prevent race conds
        uthread_switch(&mtx->m_waiters, 0, &mtx->m_pmut);

        // when return, we own mtx
        assert(mtx->m_owner == ut_curthr);
        uthread_nopreempt_off();
    }
}

/*
 * uthread_mtx_trylock
 *
 * Try to lock the mutex, return 1 if we get the lock, 0 otherwise.
 * This call should not block.
 */
int uthread_mtx_trylock(uthread_mtx_t *mtx) {
    // TO DO: Synchronize access using mtx->m_pmut
    uthread_nopreempt_on();

    // lock pthread mtx
    pthread_mutex_lock(&mtx->m_pmut);

    if (mtx->m_owner == NULL) {
        // mtx free, take ownership
        mtx->m_owner = ut_curthr;
        pthread_mutex_unlock(&mtx->m_pmut);
        uthread_nopreempt_off();
        return 1;
    } else {
        // mtx already locked
        pthread_mutex_unlock(&mtx->m_pmut);
        uthread_nopreempt_off();
        return 0;
    }
}

/*
 * uthread_mtx_unlock
 *
 * Unlock the mutex. If there are other threads waiting to get this mutex,
 * explicitly hand off the ownership of the lock to a waiting thread and
 * then wake that thread.
 */
void uthread_mtx_unlock(uthread_mtx_t *mtx) {
    // TO DO: Synchronize access using mtx->m_pmut
    uthread_nopreempt_on();

    // lock pthread mtx
    pthread_mutex_lock(&mtx->m_pmut);

    if (utqueue_empty(&mtx->m_waiters)) {
        // no waiters, clear owner
        mtx->m_owner = NULL;
        pthread_mutex_unlock(&mtx->m_pmut);
    } else {
        // get next waiting
        uthread_t *next = utqueue_dequeue(&mtx->m_waiters);

        // make it new owner
        mtx->m_owner = next;

        // unlock pthread after waking
        pthread_mutex_unlock(&mtx->m_pmut);

        // wake thread
        uthread_wake(next);
    }

    uthread_nopreempt_off();
}
