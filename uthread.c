/*
 *   FILE: uthread.c
 * AUTHOR: peter demoreuille
 *  DESCR: userland threads
 *   DATE: Sun Sep 30 23:45:00 EDT 2001
 *
 * Modifed considerably by Tom Doeppner in support of two-level threading
 *   DATE: Sun Jan 10, 2016 and Jan 2023
 */
#include "uthread.h"

#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/types.h>
#include <unistd.h>

#include "uthread_bool.h"
#include "uthread_cond.h"
#include "uthread_mtx.h"
#include "uthread_private.h"
#include "uthread_queue.h"
#include "uthread_sched.h"

/* ---------- globals -- */

__thread uthread_t *ut_curthr = 0; /* per-LWP current running thread */
__thread lwp_t *curlwp; /* per-LWP reference to the LWP context info */
uthread_t uthreads[UTH_MAX_UTHREADS]; /* threads on the system */
int lwp_cnt;                          /* number of LWPs */
sigset_t VTALRMmask;

static utqueue_t reap_queue;       /* queue for threads to be reaped */
static uthread_id_t reaper_thr_id; /* reference to reaper thread */
static uthread_mtx_t reap_mtx;
extern pthread_mutex_t runq_mtx;
static pthread_mutex_t uthreads_mtx; /* mutex for global array of uthreads when
                                        allocating thread ids */

pthread_mutexattr_t merrorattr;

/* ---------- prototypes -- */

static void create_first_thr(uthread_func_t firstFunc, long argc, char *argv[]);

static uthread_id_t uthread_alloc(void);
static void uthread_destroy(uthread_t *thread);

static char *alloc_stack(void);
static void free_stack(char *stack);

static void reaper_init(void);
static void *reaper(long a0, char *a1[]);
static void make_reapable(uthread_t *uth);
static void uthread_start_lwp(void);

/* ---------- public code -- */

/*
 * uthread_start
 *
 * This initializes everything, then becomes the first uthread and invokes its
 * first function. It does not return -- when all uthreads are done, the reaper
 * calls exit.
 */
void uthread_start(uthread_func_t firstFunc, long argc, char *argv[]) {
    // initialize VTALRMmask for use later
    sigemptyset(&VTALRMmask);
    sigaddset(&VTALRMmask, SIGVTALRM);

    for (int i = 0; i < UTH_MAX_UTHREADS; i++) {
        uthreads[i].ut_state = UT_NO_STATE;
        uthreads[i].ut_id = i;
    }

    pthread_mutexattr_init(&merrorattr);
    pthread_mutexattr_settype(&merrorattr, PTHREAD_MUTEX_ERRORCHECK);

    int err;

    if ((err = pthread_mutex_init(&runq_mtx, &merrorattr)) != 0) {
        fprintf(stderr, "%s\n", strerror(err));
        exit(1);
    }

    if ((err = pthread_mutex_init(&uthreads_mtx, &merrorattr)) != 0) {
        fprintf(stderr, "%s\n", strerror(err));
        exit(1);
    }

    /* these should go last, and in this order */
    uthread_sched_init();  // masks clock interrupts
    reaper_init();
    create_first_thr(firstFunc, argc, argv);
}

/*
 * uthread_create
 *
 * Create a uthread to execute the specified function <func> with arguments
 * <arg1> and <arg2> and initial priority <prio>. To do this, you should:
 *
 * 1. Find a valid (unused) id for the thread using uthread_alloc() (failing
 * this, return an error).
 *
 * 2. Create a stack for the thread using alloc_stack() (failing this, return an
 * error).
 *
 * 3. Set up the uthread_t struct corresponding to the newly-found id.
 *
 * 4. Create a context for the thread to execute on using uthread_makecontext().
 *
 * 5. Make the thread runnable by calling uthread_startonrunq, and return the
 *    aforementioned thread id in <uidp>.
 *
 * Return 0 on success, or an error code on failure. Refer to the man page of
 * pthread_create for the exact error codes.
 */
int uthread_create(uthread_id_t *uidp, uthread_func_t func, long arg1,
                   void *arg2, int prio) {
    uthread_id_t tid = uthread_alloc();
    *uidp = tid;
    if (tid == -1) {
        return EAGAIN;
    }

    uthread_t *thr = &uthreads[tid];
    thr->ut_stack = alloc_stack();
    if (thr->ut_stack == NULL) {
        *uidp = -1;
        return EAGAIN;
    }

    uthread_makecontext(&thr->ut_ctx, thr->ut_stack, UTH_STACK_SIZE, func, arg1,
                        arg2);

    memset(&thr->ut_link, 0, sizeof(list_link_t));
    thr->ut_prio =
        -1;  // illegal value, forcing it to be changed in uthread_setprio
    thr->ut_exit = NULL;
    utqueue_init(&thr->ut_waiter);
    pthread_mutex_init(&thr->ut_pmut, &merrorattr);

    // Thread must start with preemption disabled: this is assumed in
    // uthread_switch
    thr->ut_no_preempt_count = 1;

    uthread_startonrunq(tid, prio);  // makes new thread runnable

    return 0;
}

/*
 * uthread_exit
 *
 * Terminate the current thread. Should set all the related flags and
 * such in uthread_t.
 *
 * If this is not a detached thread, and there is a thread
 * waiting to join with it, you should wake up that thread.
 *
 * If the thread is detached, it should be put onto the reaper's dead
 * thread queue and wakeup the reaper thread by calling make_reapable().
 */
void uthread_exit(void *status) {
    // TO DO: Everything is done except for synchronization and yielding the
    // processor. As in uthreads, if the thread is detached, it calls
    // make_reapable to put itself on the reap queue and to wakeup the reaper.
    // The thread must lock ut_pmut before modifying its uthread_t. But it
    // shouldn't call uthread_mutex_lock while holding a posix mutex. Thus it
    // must lock reap_mtx before it locks ut_pmut (which means that the reap_mtx
    // is locked before calling make_reapable).
    //
    // Since once ut_pmut is unlocked the thread's stack could be freed by the
    // reaper, we must make sure that pmut is not unlocked until the thread's
    // stack is no longer in use. (Consider the last argument of
    // uthread_switch.)

    uthread_nopreempt_on();
    uthread_mtx_lock(&reap_mtx);
    pthread_mutex_lock(&ut_curthr->ut_pmut);
    // marks caller terminated
    ut_curthr->ut_state = UT_ZOMBIE;
    ut_curthr->ut_exit = status;
    if (ut_curthr->ut_detached) {
        // thread detached: notify reaper
        // make_reapable unlocks ut_pmut and reap_mtx as well as terminates
        // (detached) thread
        make_reapable(ut_curthr);
        uthread_switch(NULL, 0, &ut_curthr->ut_pmut);
        // does not return
    } else {
        // thread is joinable; wake up a waiting joiner, if exist
        uthread_t *waiter = utqueue_dequeue(&ut_curthr->ut_waiter);
        if (waiter != NULL) {
            // waiter exist -- wake up
            uthread_wake(waiter);
        }
        uthread_mtx_unlock(&reap_mtx);
        uthread_switch(NULL, 0, &ut_curthr->ut_pmut);
    }

    PANIC("returned to a dead thread");
}

/*
 * uthread_join
 *
 * Wait for the given thread to finish executing. If the thread has not
 * finished executing, the calling thread needs to block until this event
 * happens.
 *
 * Error conditions include (but are not limited to):
 * o the thread described by <uid> does not exist
 * o two threads attempting to join the same thread, etc..
 * Return an appropriate error code (found in manpage for pthread_join) in
 * these situations (and more).
 *
 * Note that if a thread finishes executing and is never uthread_join()'ed
 * (or uthread_detach()'ed) it remains in the state UT_ZOMBIE and is never
 * cleaned up.
 *
 * When you have successfully joined with the thread, set its ut_detached
 * flag to true, and then wake the reaper so it can cleanup the thread by
 * calling make_reapable
 */
int uthread_join(uthread_id_t uid, void **return_value) {
    // TO DO:
    // 1. Make sure the thread with thread id, uid, is modified in a
    // pthread-safe manner.
    //
    // 2. As with uthread_exit, reap_mtx must be locked before calling
    // make_reapable, and ut_pmut must not be locked until after reap_mtx
    // is locked.
    //
    // 3. If the target thread has not yet called uthread_exit, the calling
    // thread is placed on the target's waiter queue. Make sure that the calling
    // thread is safely on the waiter queue and no longer executing before the
    // target thread can wake it up. (Consider the last argument of
    // uthread_switch.)

    if (uid < 0 || uid >= UTH_MAX_UTHREADS) return ESRCH;

    uthread_t *thr = &uthreads[uid];
    uthread_nopreempt_on();

    // lock mtx to ensure state safety
    pthread_mutex_lock(&thr->ut_pmut);

    if (!utqueue_empty(&thr->ut_waiter)) {
        pthread_mutex_unlock(&thr->ut_pmut);
        uthread_nopreempt_off();
        return EINVAL;
    }

    if (thr->ut_detached) {
        pthread_mutex_unlock(&thr->ut_pmut);
        uthread_nopreempt_off();
        return EINVAL;
    }

    if (thr->ut_state != UT_ZOMBIE) {
        ut_curthr->ut_state = UT_WAIT;
        utqueue_enqueue(&thr->ut_waiter, ut_curthr);
        uthread_switch(NULL, 0, &thr->ut_pmut);

        // when return, target thread has terminated
        pthread_mutex_lock(&thr->ut_pmut);
    }

    // check if target has actually terminated
    if (thr->ut_state != UT_ZOMBIE) {
        // not in zombie state, something wrong happened
        pthread_mutex_unlock(&thr->ut_pmut);
        uthread_nopreempt_off();
        return EINVAL;
    }

    if (return_value) *return_value = thr->ut_exit;

    // mark as detached so no join
    thr->ut_detached = 1;

    // unlock thread mtx before locking reaper
    pthread_mutex_unlock(&thr->ut_pmut);

    uthread_mtx_lock(&reap_mtx);

    // lock thread again
    pthread_mutex_lock(&thr->ut_pmut);

    // verify still zombie
    if (thr->ut_state != UT_ZOMBIE) {
        // thread no longer zombie, been reaped
        pthread_mutex_unlock(&thr->ut_pmut);
        uthread_mtx_unlock(&reap_mtx);
        uthread_nopreempt_off();
        return 0;
    }

    make_reapable(thr);
    // handles unlocking both mtx

    uthread_nopreempt_off();
    return 0;
}

/*
 * uthread_detach
 *
 * Detach the given thread. Thus, when this thread's function has finished
 * executing, no other thread need (or should) call uthread_join() to perform
 * the necessary cleanup.
 *
 * There is also the special case if the thread has already exited and then
 * is detached (i.e. was already in the state UT_ZOMBIE when uthread_deatch()
 * is called). In this case it is necessary to call make_reapable on the
 * appropriate thread.
 *
 * There are also some errors to check for, see the man page for
 * pthread_detach (basically just invalid threads, etc).
 */
int uthread_detach(uthread_id_t uid) {
    // TO DO: something has to be done to ensure that the reaper
    // doesn't destroy uid's stack too early.
    //
    // Hint: See the reaper code, and note that reap_queue
    // access needs to be synchronized.

    if (uid < 0 || uid >= UTH_MAX_UTHREADS) return ESRCH;

    uthread_t *thr = &uthreads[uid];
    uthread_nopreempt_on();

    // lock thread mtx for state safety
    pthread_mutex_lock(&thr->ut_pmut);

    if (!utqueue_empty(&thr->ut_waiter)) {
        pthread_mutex_unlock(&thr->ut_pmut);
        uthread_nopreempt_off();
        return EINVAL;
    }

    if (thr->ut_detached) {
        pthread_mutex_unlock(&thr->ut_pmut);
        uthread_nopreempt_off();
        return EINVAL;
    }

    thr->ut_detached = 1;

    if (thr->ut_state == UT_ZOMBIE) {
        // lock reaper mtx before mod reaper q
        uthread_mtx_lock(&reap_mtx);

        // verify thread still zombie
        if (thr->ut_state != UT_ZOMBIE) {
            // not zombie, been reaped
            pthread_mutex_unlock(&thr->ut_pmut);
            uthread_mtx_unlock(&reap_mtx);
            uthread_nopreempt_off();
            return 0;
        }

        // make_reapable unlocks reap_mtx and ut_pmut
        make_reapable(thr);
    } else {
        // thr hasn't terminated yet
        pthread_mutex_unlock(&thr->ut_pmut);
    }

    uthread_nopreempt_off();
    return 0;
}

/*
 * uthread_self
 *
 * Returns the id of the currently running thread.
 */
uthread_id_t uthread_self(void) {
    uthread_nopreempt_on();
    assert(ut_curthr != NULL);
    uthread_id_t id = ut_curthr->ut_id;
    uthread_nopreempt_off();
    return id;
}

/* ------------- private code -- */

/*
 * uthread_alloc
 *
 * find a free uthread_t, returns the id.
 */
static uthread_id_t uthread_alloc(void) {
    static uthread_id_t PrevID = -1;
    int i;
    int start = PrevID + 1;
    if (start >= UTH_MAX_UTHREADS) start = 0;
    int end = UTH_MAX_UTHREADS;
    uthread_nopreempt_on();
    pthread_mutex_lock(&uthreads_mtx);
    while (1) {
        for (i = start; i < end; i++) {
            if (uthreads[i].ut_state == UT_NO_STATE) {
                uthreads[i].ut_state = UT_TRANSITION;
                PrevID = i;
                pthread_mutex_unlock(&uthreads_mtx);
                uthread_nopreempt_off();
                return (PrevID);
            }
        }
        if (start > 0) {
            end = start;
            start = 0;
            continue;
        }
        break;
    }
    pthread_mutex_unlock(&uthreads_mtx);
    uthread_nopreempt_off();
    return -1;
}

/*
 * uthread_destroy
 *
 * Cleans up resources associated with a thread (since it's now finished
 * executing). This is called implicitly whenever a detached thread finishes
 * executing or whenever non-detached thread is uthread_join()'d.
 */
static void uthread_destroy(uthread_t *uth) {
    assert(uth->ut_state == UT_ZOMBIE);
    free_stack(uth->ut_stack);
    uth->ut_detached = 0;
    uth->ut_exit = NULL;
    utqueue_init(&uth->ut_waiter);
    pthread_mutex_destroy(&uth->ut_pmut);
    uth->ut_state = UT_NO_STATE;
}

static uthread_cond_t reap_cond;

/*
 * reaper_init
 * initialize the reap queue
 */
static void reaper_init(void) {
    utqueue_init(&reap_queue);
    uthread_mtx_init(&reap_mtx);
    uthread_cond_init(&reap_cond);
}

/*
 * reaper
 *
 * This is responsible for going through all the threads on the dead
 * threads list (which should all be in the ZOMBIE state) and then
 * cleaning up all the threads that have been detached/joined with
 * already.
 *
 * In addition, when there are no more runnable threads (besides the
 * reaper itself) it will call exit() to stop the program.
 */
static void *reaper(long a0, char *a1[]) {
    uthread_nopreempt_on();
    uthread_mtx_lock(&reap_mtx);  // get exclusive access to reap queue

    while (1) {
        uthread_t *thread;
        int th;

        while (utqueue_empty(&reap_queue)) {
            // wait for a thread to join the reap queue
            uthread_cond_wait(&reap_cond, &reap_mtx);
        }

        while (!utqueue_empty(&reap_queue)) {
            // deal with all threads on reap queue
            thread = utqueue_dequeue(&reap_queue);
            assert(thread->ut_state == UT_ZOMBIE);
            pthread_mutex_lock(
                &thread->ut_pmut);  // wait for thread to get off its stack:
                                    // pmut is unlocked when the thread has
                                    // switched to the LWP's stack.
            pthread_mutex_unlock(
                &thread->ut_pmut);  // safe to unlock it -- the thread is
                                    // effectively gone.
            uthread_destroy(thread);
        }

        /* check and see if there are still runnable threads */
        for (th = 0; th < UTH_MAX_UTHREADS; th++) {
            if (th != reaper_thr_id && uthreads[th].ut_state != UT_NO_STATE) {
                break;
            }
        }

        if (th == UTH_MAX_UTHREADS) {
            /* we leak the reaper's stack */
            fprintf(stderr, "mthreads: no more threads.\n");
            fprintf(stderr, "mthreads: bye!\n");
            exit(0);
        }
    }
    return 0;
}

void lwp_switch(void);
extern pthread_mutex_t runq_mtx;
void uthread_runq_enqueue(uthread_t *thr);

/*
 * Turns the main context (the 'main' routine that initialized
 * this process) into a regular uthread that can be switched
 * into and out of. Must be called from the main context (i.e.,
 * by uthread_start()). Also creates reaper thread and sets up
 * contexts for all LWPs
 */
static void create_first_thr(uthread_func_t firstFunc, long argc,
                             char *argv[]) {
    assert(is_masked());
    // Set up uthread context for main thread
    // This does what uthread_create does, but without a valid ut_curthr
    uthread_id_t tid = 0;
    uthreads[0].ut_state = UT_TRANSITION;
    ut_curthr = &uthreads[tid];
    memset(&ut_curthr->ut_link, 0, sizeof(list_link_t));
    ut_curthr->ut_prio = UTH_MAXPRIO;
    ut_curthr->ut_exit = NULL;
    ut_curthr->ut_detached = 1;
    utqueue_init(&ut_curthr->ut_waiter);
    ut_curthr->ut_state = UT_RUNNABLE;
    pthread_mutex_init(&ut_curthr->ut_pmut, &merrorattr);
    ut_curthr->ut_no_preempt_count =
        1;  // thread created with clock interrupts masked
    ut_curthr->ut_stack = alloc_stack();
    if (ut_curthr->ut_stack == NULL) {
        PANIC("Could not create stack for first thread.");
    }

    uthread_makecontext(&ut_curthr->ut_ctx, ut_curthr->ut_stack, UTH_STACK_SIZE,
                        firstFunc, argc, argv);

    uthread_runq_enqueue(ut_curthr);

    // first thread is now on run q; next step is to set up the reaper thread
    reaper_thr_id = uthread_alloc();
    ut_curthr = &uthreads[reaper_thr_id];
    memset(&ut_curthr->ut_link, 0, sizeof(list_link_t));
    ut_curthr->ut_prio = UTH_MAXPRIO;
    ut_curthr->ut_exit = NULL;
    ut_curthr->ut_detached = 1;
    utqueue_init(&ut_curthr->ut_waiter);
    ut_curthr->ut_state = UT_RUNNABLE;
    pthread_mutex_init(&ut_curthr->ut_pmut, &merrorattr);
    ut_curthr->ut_no_preempt_count =
        1;  // thread created with clock interrupts masked
    ut_curthr->ut_stack = alloc_stack();
    if (ut_curthr->ut_stack == NULL) {
        PANIC("Could not create stack for reaper thread.");
    }

    uthread_makecontext(&ut_curthr->ut_ctx, ut_curthr->ut_stack, UTH_STACK_SIZE,
                        reaper, 0, 0);

    uthread_runq_enqueue(ut_curthr);

    // reaper thread is now on run q; next step is to set up the initial LWP
    pthread_mutex_lock(
        &runq_mtx);  // lock runq so that no LWPs do anything till we're ready

    lwp_cnt = 1;
    // set up additional LWPs: all inherit clock interrupt mask
    for (int i = 1; i < UTH_LWPS; i++) uthread_start_lwp();

    curlwp = (lwp_t *)malloc(sizeof(lwp_t));
    memset(curlwp, 0, sizeof(lwp_t));
    curlwp->ptid = pthread_self();

    ut_curthr = NULL;  // at the moment we don't have a current uthread --
                       // they're both on the run q.
    lwp_switch();      // will unlock runq and start things off
    PANIC("lwp returned to create_first_thr");
}

/*
 * make_reapable
 *
 * Adds uth to the reaper's queue, and wakes up the reaper.
 * Called when a thread is completely dead (is in the state UT_ZOMBIE).
 */
static void make_reapable(uthread_t *uth) {
    // reap_mtx and uth->ut_pmut are locked by caller, but are both unlocked
    // here. Clock interrupts are masked, and since ut_pmut is locked, we
    // can't lock reap_mtx here. Caller might be a zombie.
    // TO DO:
    // 1. This function should be called with reap_mtx locked
    // to synchronize access to the reap_queue with the reaper uthread.
    // You should figure out where to unlock reap_mtx in this function.
    //
    // 2. You need to make sure that thread resources aren't destroyed
    // too early. Where should ut_pmut be unlocked?
    //
    // Hint: Consider how make_reapable should behave if uth == ut_curthr
    //       vs. when uth != ut_curthr.
    // reap_mtx and uth->ut_pmut are locked by caller

    assert(is_masked());

    // check if thread in zombie
    if (uth->ut_state != UT_ZOMBIE) {
        // not zombie, smthing went wrong
        pthread_mutex_unlock(&uth->ut_pmut);
        uthread_mtx_unlock(&reap_mtx);
        return;
    }

    // adds thread to reaper q and signals reaper
    utqueue_enqueue(&reap_queue, uth);
    uthread_cond_signal(&reap_cond);

    if (uth == ut_curthr) {
        // unlock thread & reaper mtxs before switch
        pthread_mutex_unlock(&uth->ut_pmut);
        uthread_mtx_unlock(&reap_mtx);
        // switch without queuing next
        uthread_switch(NULL, 0, NULL);
        PANIC("Zombie thread returned from context switch");
    } else {
        // if not cur thread, unlock both mtxs directly
        pthread_mutex_unlock(&uth->ut_pmut);
        uthread_mtx_unlock(&reap_mtx);
    }
}

static char *alloc_stack(void) {
    // malloc/free are pthread-safe since we're building on top
    // of pthreads, but we must protect from clock interrupts to
    // make them mthreads-safe
    uthread_nopreempt_on();
    char *stack = (char *)malloc(UTH_STACK_SIZE);
    uthread_nopreempt_off();
    return stack;
}

static void free_stack(char *stack) {
    uthread_nopreempt_on();
    free(stack);
    uthread_nopreempt_off();
}

/*
 * lwp_start
 *
 * The initial routine of all but the first LWP. It sets up its lwp struct. Both
 * ut_curthr and curlwp are thread-local storage and private to the LWP. The
 * former refers to the current uthread (if any) being run by the LWP, the
 * latter refers the LWPs private context info.
 */
static void *lwp_start(void *dummy) {
    pthread_sigmask(SIG_BLOCK, &VTALRMmask, 0);
    ut_curthr = NULL;
    curlwp = (lwp_t *)malloc(sizeof(lwp_t));
    memset(curlwp, 0, sizeof(lwp_t));
    curlwp->ptid = pthread_self();
    pthread_mutex_lock(&runq_mtx);
    lwp_cnt++;
    lwp_switch();
    PANIC("lwp returned to lwp_start");
    return 0;
}

/*
 * uthread_start_lwp
 *
 * Create a new LWP as a low-level thread to run uthreads.
 * Any number of these may be created.
 */
static void uthread_start_lwp() {
    pthread_t ptid;
    pthread_create(&ptid, 0, lwp_start, 0);
    pthread_detach(ptid);
}
