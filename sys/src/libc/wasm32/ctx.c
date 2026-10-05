/*
 * wasm32: contexts, the platform's own system calls (monolith
 * wasm32host/host3js.c): a proc's stacks it switches between itself -
 * libthread's threads (libthread/wasm32).  Not with libc.h: the system
 * call stubs' way.
 */
typedef long long vlong;
extern vlong _trap(int, void*);

/* a context: fn(arg) on [stk, stk+n), the saved frames at its bottom */
int
_ctxnew(void (*fn)(void*), void *arg, void *stk, unsigned long n)
{
	return _trap(100, &fn);
}

/* to context id: this one unwound, that one rewound or started */
int
_ctxswitch(int id)
{
	return _trap(101, &id);
}

int
_ctxfree(int id)
{
	return _trap(102, &id);
}

int
_ctxself(void)
{
	int x;

	return _trap(103, &x);
}
