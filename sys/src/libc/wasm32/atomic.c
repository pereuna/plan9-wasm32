#include <u.h>
#include <libc.h>

/*
 * wasm32: libc's atomics on Along, Aptr (386's atomic.s) - 3l's cas and
 * casp (atomic.rmw.cmpxchg) in a loop.  The 64-bit ones are port's
 * (atomic64.c)
 */
extern	int	cas(long*, long, long);
extern	int	casp(void**, void*, void*);

long
agetl(Along *a)
{
	long v;

	do
		v = a->v;
	while(!cas(&a->v, v, v));
	return v;
}

long
aswapl(Along *a, long n)
{
	long o;

	do
		o = a->v;
	while(!cas(&a->v, o, n));
	return o;
}

long
aincl(Along *a, long d)
{
	long o;

	do
		o = a->v;
	while(!cas(&a->v, o, o+d));
	return o+d;
}

int
acasl(Along *a, long o, long n)
{
	return cas(&a->v, o, n);
}

void*
agetp(Aptr *a)
{
	void *v;

	do
		v = a->v;
	while(!casp(&a->v, v, v));
	return v;
}

void*
aswapp(Aptr *a, void *n)
{
	void *o;

	do
		o = a->v;
	while(!casp(&a->v, o, n));
	return o;
}

int
acasp(Aptr *a, void *o, void *n)
{
	return casp(&a->v, o, n);
}
