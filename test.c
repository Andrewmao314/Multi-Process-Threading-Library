/*
 *   FILE: test.c
 * AUTHOR: Sean Andrew Cannella (scannell)
 *  DESCR: a simple test program for uthreads
 *   DATE: Mon Sep 23 14:11:48 2002
 *
 * Modifed considerably by Tom Doeppner in support of two-level threading
 *   DATE: Sun Jan 10, 2016 and Jan 2023
 *
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "uthread.h"
#include "uthread_cond.h"
#include "uthread_mtx.h"
#include "uthread_sched.h"

#define NUM_THREADS 100

#define SBUFSZ 256

uthread_id_t thr[NUM_THREADS];
uthread_mtx_t mtx;
uthread_cond_t cond;

int turn;

static void *tester(long a0, char *a1[]) {
    for (int i = 0; i < 10; i++) {
        MTprintf("thread %i: hello! (%i)\n", uthread_self(), i);
        uthread_yield();

        volatile int j, k = 100000000;
        unsigned int seed = uthread_self();
        int random = rand_r(&seed) % 10000;
        for (j = 0; j < random; j++) {
            if (!(k % 101)) uthread_setprio(j % NUM_THREADS, j % UTH_MAXPRIO);
            k /= 3;
        }

        uthread_mtx_lock(&mtx);
        uthread_yield();
        while (turn != a0) {
            uthread_cond_wait(&cond, &mtx);
        }
        assert(turn == a0);
        turn++;
        if (turn == NUM_THREADS) turn = 0;
        uthread_cond_broadcast(&cond);
        uthread_mtx_unlock(&mtx);
    }

    MTprintf("thread %i exiting.\n", uthread_self());

    return (void *)a0;
}

uthread_mtx_t m;

static void *Different_tester(long a0, char *a1[]) {
    uthread_yield();
    volatile int j;
    for (j = 0; j < 100; j++) {
        uthread_setprio(j % NUM_THREADS, j % UTH_MAXPRIO);
    }
    for (int i = 0; i < 10; i++) {
        uthread_mtx_lock(&m);
        if ((i % 4) == 0) uthread_setprio(uthread_self(), i % UTH_MAXPRIO);
        uthread_mtx_unlock(&m);
    }

    uthread_yield();
    for (j = 0; j < 1000000; j++) {
        uthread_setprio(j % NUM_THREADS, j % UTH_MAXPRIO);
    }
    return 0;
}

void *mainthread(long ac, char *av[]) {
    int i;

    uthread_mtx_init(&m);
    uthread_mtx_init(&mtx);
    uthread_cond_init(&cond);

    for (i = 0; i < NUM_THREADS; i++) {
        uthread_create(&thr[i], tester, i, NULL, 0);
    }
    uthread_setprio(thr[0], 2);
    uthread_id_t dthread;
    uthread_create(&dthread, Different_tester, 0, 0, 1);
    uthread_detach(dthread);
    uthread_create(&dthread, Different_tester, 1, 0, 1);
    uthread_detach(dthread);

    for (i = 0; i < NUM_THREADS; i++) {
        long tmp;

        uthread_join(thr[i], (void **)&tmp);

        MTprintf("joined with thread %i, exited %li.\n", thr[i], tmp);
        uthread_mtx_lock(&mtx);
        uthread_cond_signal(&cond);
        uthread_mtx_unlock(&mtx);
    }

    uthread_exit(0);

    return 0;
}

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

int main(int argc, char *argv[]) {
    uthread_start(mainthread, argc, argv);
    return 0;
}
