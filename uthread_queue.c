/*
 *   FILE: uthread_queue.c
 * AUTHOR: Peter Demoreuille
 *  DESCR: queues of threads.
 *   DATE: Sun Sep  9 15:05:37 2001
 *
 */

#include "uthread_queue.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "uthread.h"

/*
 * utqueue_init
 *
 * Initialize the queues
 */
void utqueue_init(utqueue_t *q) {
    assert(q != NULL);

    list_init(&q->tq_waiters);
    q->tq_size = 0;
}

/*
 * utqueue_empty
 *
 * Returns whether or not the queue is empty.
 */
int utqueue_empty(utqueue_t *q) {
    assert(q != NULL);
    assert(list_empty(&q->tq_waiters) == (q->tq_size == 0));

    return (q->tq_size == 0);
}

/*
 * utqueue_enqueue
 *
 * Adds a thread to the front of the queue.
 */
void utqueue_enqueue(utqueue_t *q, uthread_t *thr) {
    assert(thr->ut_link.l_next == NULL && thr->ut_link.l_prev == NULL);

    list_insert_head(&q->tq_waiters, &thr->ut_link);
    q->tq_size++;
}

/*
 * utqueue_dequeue
 *
 * Dequeues from the queue.
 */
uthread_t *utqueue_dequeue(utqueue_t *q) {
    uthread_t *thr;
    list_link_t *link;

    assert(q != NULL);

    if (utqueue_empty(q)) {
        return NULL;
    }

    link = q->tq_waiters.l_prev;
    thr = list_item(link, uthread_t, ut_link);
    list_remove(link);

    q->tq_size--;

    return thr;
}

/*
 * utqueue_remove
 *
 * Removes the given thread from queue.
 */
void utqueue_remove(utqueue_t *q, uthread_t *thr) {
    assert(thr->ut_link.l_next != NULL && thr->ut_link.l_prev != NULL);

    list_remove(&thr->ut_link);
    q->tq_size--;
}
