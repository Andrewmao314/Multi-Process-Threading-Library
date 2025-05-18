/*
 *   FILE: uthread_sched.c
 * AUTHOR: Peter Demoreuille
 *  DESCR: scheduling wack for uthreads
 *   DATE: Mon Oct  1 00:19:51 2001
 *
 * Modifed considerably by Tom Doeppner
 *   DATE: Sun Jan 10, 2016
 * Futher modified in January 2020 and Jan 2023
 */

#include "uthread_sched.h"

#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/errno.h>
#include <sys/time.h>

#include "uthread.h"
#include "uthread_bool.h"
#include "uthread_ctx.h"
#include "uthread_private.h"
#include "uthread_queue.h"

void uthread_runq_enqueue(uthread_t *thr);
static void uthread_runq_requeue(uthread_t *thr, int oldpri);

/* ---------- globals -- */

pthread_mutex_t runq_mtx;                     /* mutex for the runq */
static utqueue_t runq_table[UTH_MAXPRIO + 1]; /* priority runqueues */

/* Checks whether SIGVTALRM is masked */
int is_masked() {
    int ret;
    sigset_t oset;
    pthread_sigmask(SIG_BLOCK, &VTALRMmask, &oset);
    ret = sigismember(&oset, SIGVTALRM);
    pthread_sigmask(SIG_SETMASK, &oset, 0);
    return ret;
}

/* ----------- public code -- */

/*
 * uthread_yield
 *
 * Causes the currently running thread to yield use of the processor to
 * another thread. The thread is still runnable however, so it should
 * be rescheduled by the scheduler (and eventually ran again).
 * When this function returns, the thread should be executing again. A bit more
 * clearly, when this function is called, the current thread stops executing for
 * some period of time (allowing another thread to execute). Then, when the time
 * is right (i.e. when a call to uthread_switch() results in this thread
 * being swapped in), the function returns.
 */
void uthread_yield() {
    // TO DO: Make sure that the current thread is not put on the run queue
    // until after the thread has context-switched. If we were to enqueue
    // the thread, and then context switch, there would be a window during
    // which this thread may be scheduled on another processor, resulting in
    // a situation in which two processors are executing on the same stack.

    uthread_nopreempt_on();
    assert(is_masked());
    assert(ut_curthr->ut_link.l_next == NULL);

    // switch away, indicate thread needs to be put on queue
    // second param tells lwp_switch to put thread on run q
    uthread_switch(NULL, 1, NULL);

    assert(is_masked());
    uthread_nopreempt_off();
}

/*
 * uthread_wake
 *
 * If the target thread is in the UT_WAIT state, make it runnable,
 * and add it to the runq.
 */
void uthread_wake(uthread_t *uthr) {
    // TO DO: Synchronize access to the run queue.

    assert(uthr->ut_state != UT_NO_STATE);
    uthread_nopreempt_on();

    // lock runq mtx to synch access
    pthread_mutex_lock(&runq_mtx);

    if (uthr->ut_state == UT_WAIT) {
        uthr->ut_state = UT_RUNNABLE;
        uthread_runq_enqueue(uthr);
        pthread_mutex_unlock(&runq_mtx);
        uthread_nopreempt_off();
    } else {
        pthread_mutex_unlock(&runq_mtx);
        uthread_nopreempt_off();
    }
}

/*
 * uthread_setprio
 *
 * Changes the priority of the indicated thread. Note that if the thread
 * is in the UT_RUNNABLE state (it's runnable but not on CPU) you should
 * change the list it's waiting on so the effect of this call is
 * immediate.
 */
int uthread_setprio(uthread_id_t id, int prio) {
    // TO DO: Since we're potentially modifying the run queue here, we
    // also need to synchronize the invoking uthreads.
    //
    // Hint: Use the runq_mtx here to ensure that the thread is still active.
    //       Note that you do not have to lock the thread's ut_pmut, as
    //       it could result in a potential deadlock scenario.

    if ((prio > UTH_MAXPRIO) || (prio < 0)) return EINVAL;
    if ((id < 0) || (id >= UTH_MAX_UTHREADS)) return ESRCH;

    uthread_t *thr = &uthreads[id];
    uthread_nopreempt_on();

    // lock runq mutex to modify prio safely
    pthread_mutex_lock(&runq_mtx);

    if ((thr->ut_state == UT_NO_STATE) || (thr->ut_state == UT_TRANSITION) ||
        (thr->ut_state == UT_ZOMBIE)) {
        // not active thread
        pthread_mutex_unlock(&runq_mtx);
        uthread_nopreempt_off();
        return EINVAL;
    }

    if (thr->ut_prio == prio) {
        pthread_mutex_unlock(&runq_mtx);
        uthread_nopreempt_off();
        return 0;
    }

    int oldprio = thr->ut_prio;
    thr->ut_prio = prio;

    if (thr->ut_state == UT_RUNNABLE) {
        uthread_runq_requeue(thr, oldprio);  // switch runq's
    }

    if ((prio > ut_curthr->ut_prio) && (thr->ut_state == UT_RUNNABLE)) {
        // yield if its priority is better than caller's
        pthread_mutex_unlock(&runq_mtx);
        uthread_nopreempt_off();
        uthread_yield();
    } else {
        pthread_mutex_unlock(&runq_mtx);
        uthread_nopreempt_off();
    }

    return 0;
}

/* ----------- (mostly) private code -- */

/*
 * Called by uthread_create to put the newly created thread on the run queue
 */
void uthread_startonrunq(uthread_id_t id, int prio) {
    // TO DO: Make sure no other threads can access the runq while it's
    // being modified.

    uthread_t *thr = &uthreads[id];
    uthread_nopreempt_on();

    // locks runq for safe modification
    pthread_mutex_lock(&runq_mtx);

    thr->ut_prio = prio;

    if (thr->ut_state == UT_TRANSITION) {
        // newly created thread
        thr->ut_state = UT_RUNNABLE;
        uthread_runq_enqueue(thr);
    } else {
        PANIC("new thread not in UT_TRANSITION state");
    }

    pthread_mutex_unlock(&runq_mtx);
    uthread_nopreempt_off();
}

static void lwp_park(void);

/*
    The total number of threads on the runq. You will be responsible of
   maintaining runq_size when threads are enqueued / dequeued from the runq.
*/
static int runq_size;

/*
 * uthread_switch
 *
 * This is where all the magic begins. Save the uthread's context (using
 * getcontext). This is where the uthread will resume, so use a volatile local
 * variable to distinguish between first and second returns from getcontext. Put
 * any parameters into *curlwp, so the LWP can act on them after getting out of
 * uthread's context. Finally, switch to the context of the LWP using
 * setcontext.
 *
 * Parameters are:
 *     q: if non-null, a queue that the calling thread should be put on (should
 *        not be runq)
 *     saveonrunq: whether the calling thread should be put on the runq
 *     m: if non-null, a pointer to a mutex that should be unlocked once off
 *        this thread's stack
 */
void uthread_switch(utqueue_t *q, int saveonrq, pthread_mutex_t *m) {
    // thread preeemption should be off
    // TO DO:
    // 1. Lock the runq_mtx.
    //
    // 2. Initialize a volatile variable and save the uthread's context
    // using getcontext. Use this variable to keep track of the first
    // and second returns from getcontext. We use the volatile keyword
    // because this prevents gcc from optimizing it away, forcing it
    // to be reread if the same thread resumes on a new processor.
    //
    // 3. Setup the thread-local curlwp with the passed arguments to operate
    // on from lwp_switch (where we are no longer on the current threads
    // stack).

    // check thread preeemption off
    assert(is_masked());
    assert(ut_curthr->ut_no_preempt_count > 0);

    // lock runq mtx
    pthread_mutex_lock(&runq_mtx);

    // force zombie never reach run q
    if (ut_curthr->ut_state == UT_ZOMBIE) {
        saveonrq = 0;
    }

    // initialize volatile variable & save uthread's context
    volatile int first_time = 1;

    // save thread context
    getcontext(&ut_curthr->ut_ctx);

    // checks first time or returning from context switch
    if (first_time == 0) {
        // returning from switch
        // no unlock runq_mtx - already unlocked in lwp_switch

        // if zombie, SOMETHING WRONG
        if (ut_curthr->ut_state == UT_ZOMBIE) {
            PANIC("Zombie thread returned from context switch");
        }

        return;
    }

    // first time, mark that context saved
    first_time = 0;

    // setup thread-local curlwp with passed args
    curlwp->queue = q;
    curlwp->saveonrq = saveonrq;
    curlwp->pmut = m;

    // switch to lwp context
    setcontext(&curlwp->lwp_ctx);

    // should not return here
    PANIC("setcontext failed in uthread_switch");
}

/*
 * lwp_switch
 *
 * This is where LWPs hang out when they're looking for uthreads to run. They
 * each call this just once when they start up. Within lwp_switch they
 * immediately call getcontext to save their context, which is where they resume
 * when switched to from uthread_switch. They first do operations requested by
 * the just-switched-from uthread, then they search the runq for a runnable
 * uthread. If they don't find one, they call lwp_park where they wait until
 * released.
 *
 * In the lwp struct (found in uthread.h) are parameters from uthread_switch
 * that the LWP handles now that it's no longer running on the uthread's stack.
 */
void lwp_switch() {
    // TO DO:
    // 1. Save the context of this LWP in curlwp using getcontext
    //
    // 2. Check the arguments that are set in curlwp from uthread_switch.
    // If a given argument is not null, perform the appropriate action:
    //
    //    - For curlwp->queue, enqueue ut_curthr on the given queue
    //
    //    - For curlwp->saveonrq, make the current thread runnable. This
    //      involves modifying the appropriate run queue.
    //
    //    - For curlwp->pmut, unlock the given mutex.
    //
    // These operations are useful if you need synchronization that requires
    // doing something concerning the current uthread but only AFTER it is no
    // longer executing on its own stack.
    //
    // 3. Find a new thread to run, dequeue it and switch into its context
    // using setcontext. Don't forget to set ut_curthr (recall that this is
    // a lwp-local variable, see the declaration in uthread.c) and update its
    // state. And don't forget to unlock runq_mtx when you find a thread to run.
    // Make sure to also maintain the runq_size.
    //
    // 4. If no runnable threads are found call lwp_park. Note that lwp_park
    // is a blocking operation. You should structure your code so that once
    // lwp_release is called (e.g. utqueue_runq_enqueue), the blocked
    // lwp's can again search for a thread to run (using an infinite while
    // loop will do).
    //
    // Note that the LWP starts up by calling lwp_switch.

    // runq_mtx assumed locked
    assert(is_masked());

    // save context of lwp in curlwp using getcontext
    getcontext(&curlwp->lwp_ctx);
    curlwp->lwp_ctx.uc_sigmask = VTALRMmask;

    // lwp resumes here after uthread_switch
    // either ut_curthr is NULL or preemption is disabled
    assert((ut_curthr == NULL) || (ut_curthr->ut_no_preempt_count > 0));

    // check curlwp args
    if (curlwp->queue != NULL && ut_curthr != NULL) {
        // enqueue curthr on given queue
        utqueue_enqueue(curlwp->queue, ut_curthr);
    }

    // zombie never back on runq
    if (curlwp->saveonrq && ut_curthr != NULL &&
        ut_curthr->ut_state != UT_ZOMBIE) {
        // make curthr runnable and put on run q (only if not zombie)
        ut_curthr->ut_state = UT_RUNNABLE;
        uthread_runq_enqueue(ut_curthr);
    }

    if (curlwp->pmut != NULL) {
        // unlock given mtx
        pthread_mutex_unlock(curlwp->pmut);
    }

    // reset curlwp fields
    curlwp->queue = NULL;
    curlwp->saveonrq = 0;
    curlwp->pmut = NULL;

    // clear curthr reference, about to switch
    ut_curthr = NULL;

    // find highest prio thread to run
    while (1) {
        uthread_t *next_thread = NULL;
        for (int i = UTH_MAXPRIO; i >= 0; i--) {
            if (!utqueue_empty(&runq_table[i])) {
                next_thread = utqueue_dequeue(&runq_table[i]);
                runq_size--;
                break;
            }
        }

        if (next_thread != NULL) {
            // found runnable
            ut_curthr = next_thread;
            ut_curthr->ut_state = UT_ON_CPU;
            pthread_mutex_unlock(&runq_mtx);

            // switch to that thread's context
            setcontext(&ut_curthr->ut_ctx);
            // should never return
            PANIC("setcontext failed in lwp_switch");
        } else {
            // no runnable, park lwp
            lwp_park();

            // when return, run q mutex still locked
            // loop to check for runnable again
        }
    }
}

static void uthread_start_timer(void);

/*
 * uthread_sched_init
 *
 * Setup the scheduler. This is called once from uthread_init().
 */
void uthread_sched_init(void) {
    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&runq_mtx, &mattr);

    for (int i = 0; i <= UTH_MAXPRIO; i++) {
        utqueue_init(&runq_table[i]);
    }
    uthread_start_timer();
}

static void clock_interrupt(int);

static void uthread_start_timer() {
    // start the time-slice timer.
    // It's process-wide, not per LWP and is delivered to randomly chosen LWP.
    // The posix signal mask is per LWP.
    struct timeval interval = {0, 1};  // every microsecond
    struct itimerval timerval;
    timerval.it_value = interval;
    timerval.it_interval = interval;
    signal(SIGVTALRM, clock_interrupt);
    pthread_sigmask(SIG_BLOCK, &VTALRMmask, 0);  // initially masked
    setitimer(ITIMER_VIRTUAL, &timerval, 0);     // off we go!
}

static void clock_interrupt(int sig) {
    // handler for SIGVTALRM
    assert(ut_curthr != 0);
    assert(ut_curthr->ut_state == UT_ON_CPU);
    assert(ut_curthr->ut_no_preempt_count == 0);
    ut_curthr->ut_no_preempt_count = 1;
    uthread_yield();
    assert(ut_curthr->ut_no_preempt_count == 1);
    assert(is_masked());
    ut_curthr->ut_no_preempt_count = 0;
    // SIGVTALRM will be unmasked on return from this signal handler
}

void uthread_nopreempt_on() {
    // mask clock interrupts for current thread; calls may be nested
    // must not refer to TLS variables (e.g., ut_curthr, masked) without clock
    // interrupts masked
    assert(!is_masked() || ut_curthr->ut_no_preempt_count);
    pthread_sigmask(SIG_BLOCK, &VTALRMmask, 0);
    ut_curthr->ut_no_preempt_count++;
    assert(ut_curthr->ut_no_preempt_count > 0);
}

void uthread_nopreempt_off() {
    // unmask clock interrupts for current thread; since calls may be nested,
    // must keep track of whether this the "last" call to turn on clock
    // interrupts
    assert(is_masked());
    assert(ut_curthr != NULL);
    assert(ut_curthr->ut_no_preempt_count > 0);
    if (--ut_curthr->ut_no_preempt_count == 0) {
        pthread_sigmask(SIG_UNBLOCK, &VTALRMmask, 0);
    }
}

static pthread_cond_t lwp_park_cond = PTHREAD_COND_INITIALIZER;
static int lwp_parked_cnt;

/*
 *
 * lwp_park
 *
 * This is where LWPs hang out when they have nothing to do.
 */

static void lwp_park() {
    assert(ut_curthr == NULL);
    // TO DO: Block the lwp on the lwp_park_cond condition variable until
    // lwp_release is called. Remember that you should call
    // pthread_cond_wait in a while loop. All bookkeeping for lwp_parked_cnt
    // is done here. Note that runq_mtx is locked on entry, and
    // it should by locked on exit (recall that switching into a
    // thread in lwp_switch involves unlocking the runq mutex)
    //
    // You should maintain a count of the number of LWPs that are currently
    // in this function (lwp_parked_cnt). Its value should always be less
    // lwp_cnt (the total number of LWPs). Why is this so?
    // Why shouldn't lwp_parked_cnt == lwp_cnt? What does this mean?

    // runq mutex is locked on entry
    if (++lwp_parked_cnt == lwp_cnt) {
        // it's all over (and something went wrong -- this shouldn't happen)
        // do not remove this panic!
        PANIC("All LWPs parked -- stuck!\n");
    }

    // wait for runnable thread
    while (runq_size == 0) {
        // wait on condition
        pthread_cond_wait(&lwp_park_cond, &runq_mtx);
    }

    // been woken, decrement parked count
    lwp_parked_cnt--;

    // runq mutex should be locked on exit
}

/*
 *
 * lwp_release
 *
 * A uthread has become runnable. Wake up an LWP to deal with it.
 */
static void lwp_release() { pthread_cond_signal(&lwp_park_cond); }

void uthread_runq_enqueue(uthread_t *thr) {
    // TO DO: Enqueue the uthread on its appropriate runq.
    // Remember to bump runq_size and call lwp_release if
    // runq_size was just zero (since now there's a thread to
    // run within an lwp).
    //
    // You may assume that preemption has been disabled and
    // that runq_mtx locking takes place outside of this function.

    // add thread to queue of the given prio
    utqueue_enqueue(&runq_table[thr->ut_prio], thr);

    // increment runnable threads queue count
    runq_size++;

    // if first to become runnable, wake a parked lwp
    if (runq_size == 1) {
        lwp_release();
    }
}

static void uthread_runq_requeue(uthread_t *thr, int oldprio) {
    // TO DO: Remove thr from the run queue for oldprio priority
    // uthreads, and enqueue it on the run queue associated with
    // thr's current priority.
    //
    // You may assume that preemption has been disabled and
    // that runq_mtx locking takes place outside of this function.
    // remove from old prio q
    utqueue_remove(&runq_table[oldprio], thr);

    // add to new prio q
    utqueue_enqueue(&runq_table[thr->ut_prio], thr);
}
