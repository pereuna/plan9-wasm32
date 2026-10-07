#include <u.h>
#include <libc.h>
#include <thread.h>
#include "threadimpl.h"

/*
 * wasm32: a thread is a context of its proc (libc/wasm32/ctx.c): its
 * stack t->stk, switched to by _ctxswitch, not longjmp; t->sched[0] is
 * the context
 */
extern	int	_ctxnew(void (*)(void*), void*, void*, ulong);

static void
launcher(void *v)
{
	void **a;
	void (*f)(void*);

	a = v;
	f = (void(*)(void*))a[0];
	(*f)(a[1]);
	threadexits(nil);
}

void
_threadinitstack(Thread *t, void (*f)(void*), void *arg)
{
	void **a;
	ulong n;

	n = (t->stksize - 16) & ~7;
	a = (void**)&t->stk[n];
	a[0] = (void*)f;
	a[1] = arg;
	t->sched[0] = _ctxnew(launcher, a, t->stk, n);
}
