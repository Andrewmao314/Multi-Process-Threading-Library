/*
 *   FILE: uthread_cond.c
 * AUTHOR: Peter Demoreuille
 *  DESCR: uthreads condition variables
 *   DATE: Mon Oct  1 01:59:37 2001
 *
 * Modifed considerably by Tom Doeppner in support of two-level threading
 *   DATE: Sun Jan 10, 2016 and Jan 2023
 *
 */

#include "uthread_cond.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "uthread.h"
#include "uthread_mtx.h"
#include "uthread_private.h"
#include "uthread_queue.h"
#include "uthread_sched.h"

/*
 * uthread_cond_init
 *
 * initialize the given condition variable
 */
void uthread_cond_init(uthread_cond_t *cond) {
    utqueue_init(&cond->uc_waiters);
    pthread_mutex_init(&cond->pmut, 0);
}

/*
 * uthread_cond_wait
 *
 * Should behave just like a stripped down version of pthread_cond_wait.
 * Block on the given condition variable. The caller should lock the
 * mutex and it should be locked again on return.
 */
void uthread_cond_wait(uthread_cond_t *cond, uthread_mtx_t *mtx) {
    // TO DO:
    // 1. Synchronize access to the condition variable struct using its pmut
    // member. This guarantees that the unlock and sleep operations
    // happen as one atomic step.
    //
    // 2. Ensure that the calling uthread is put on the condition
    // variable's waiters queue after it has switched contexts. The
    // potential problem is the same as it is for
    // uthread_mtx_lock/uthread_mtx_unlock, only now we're dealing
    // with uthread_cond_wait/uthread_cond_broadcast (see uthread_mtx.c
    // for more detailed explanation).

    uthread_nopreempt_on();

    // lock conditioned mutex
    pthread_mutex_lock(&cond->pmut);

    // release user mutex
    uthread_mtx_unlock(mtx);

    // mark thread as waitin
    ut_curthr->ut_state = UT_WAIT;

    // add thread to wait q, switch context
    // pass cond->pmut to unlock after switch
    uthread_switch(&cond->uc_waiters, 0, &cond->pmut);

    // when return, has been woken
    uthread_nopreempt_off();

    // reaquire mutex
    uthread_mtx_lock(mtx);

    uthread_nopreempt_on();
    assert(ut_curthr->ut_link.l_next == NULL);
    uthread_nopreempt_off();
}

/*
 * uthread_cond_broadcast
 *
 * Wake up all the threads waiting on this condition variable.
 * Note there may be no threads waiting.
 */
void uthread_cond_broadcast(uthread_cond_t *cond) {
    // TO DO: Synchronize access using cond->pmut

    uthread_nopreempt_on();

    // lock cond variable mtx
    pthread_mutex_lock(&cond->pmut);

    // wake all waiting
    while (!utqueue_empty(&cond->uc_waiters)) {
        uthread_t *waiter = utqueue_dequeue(&cond->uc_waiters);
        uthread_wake(waiter);
    }

    // unlock cond mutex
    pthread_mutex_unlock(&cond->pmut);

    uthread_nopreempt_off();
}

/*
 * uthread_cond_signal
 *
 * Wake up just one thread waiting on the condition variable.
 * Note there may be no threads waiting.
 */
void uthread_cond_signal(uthread_cond_t *cond) {
    // TO DO: Synchronize access using cond->pmut

    uthread_nopreempt_on();

    // lock cond mtx
    pthread_mutex_lock(&cond->pmut);

    // wake 1 thread, if any exist
    uthread_t *thr = utqueue_dequeue(&cond->uc_waiters);
    if (thr != NULL) {
        uthread_wake(thr);
    }

    // unlock cond mtx
    pthread_mutex_unlock(&cond->pmut);

    uthread_nopreempt_off();
}
