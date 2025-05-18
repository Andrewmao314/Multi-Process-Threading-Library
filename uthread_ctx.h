/*
 *   FILE: uthread_ctx_linux.h
 * AUTHOR: Peter Demoreuille
 *  DESCR: userland thread context switching for solaris
 *   DATE: Sat Sep  8 11:11:43 2001
 *
 */

#ifndef __uthread_linux_ctx_h__
#define __uthread_linux_ctx_h__

#ifdef _REENTRANT
//#error Compiling -mt is NOT supported
#endif

#include <ucontext.h>

typedef ucontext_t uthread_ctx_t;

/* Sets up the given context with a stack and a function to execute with
 * provided arguments. A call to uthread_(set/swap)context will cause
 * func to begin executing.
 */
void uthread_makecontext(uthread_ctx_t *ctx, char *stack, int stacksz,
                         void *(*func)(), long arg1, void *arg2);

#endif /* __uthread_ctx_linux_h__ */
