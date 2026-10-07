/*
 * 3c test: setjmp and longjmp (3l: WebAssembly exceptions), the atomics
 */
#ifdef __GNUC__
#include <unistd.h>
#include <stdlib.h>
#include <setjmp.h>
long pwrite1(int fd, void *s, long n, long long off) { return write(fd, s, n); }
void exits1(char *s) { exit(s != 0 && *s != 0); }
int _tas(int *p) { int v = *p; *p = 0xdeadead; return v; }
long ainc(long *p) { return ++*p; }
long adec(long *p) { return --*p; }
int cas(int *p, int ov, int nv) { if(*p != ov) return 0; *p = nv; return 1; }
#else
long long _trap(int, void*);
long pwrite1(int fd, void *s, long n, long long off) { return _trap(51, &fd); }
void exits1(char *s) { _trap(8, &s); }
typedef long jmp_buf[2];
int setjmp(jmp_buf);
void longjmp(jmp_buf, int);
int _tas(int*);
long ainc(long*);
long adec(long*);
int cas(int*, int, int);
#endif

void
puts1(char *s)
{
	int n;

	for(n = 0; s[n]; n++)
		;
	pwrite1(1, s, n, -1LL);
}

void
putint(long long v)
{
	char buf[32];
	int i, neg;

	i = sizeof buf - 1;
	buf[i] = 0;
	neg = v < 0;
	if(neg)
		v = -v;
	do {
		buf[--i] = '0' + v % 10;
		v /= 10;
	} while(v);
	if(neg)
		buf[--i] = '-';
	puts1(buf+i);
}

void
show(char *what, long long v)
{
	puts1(what);
	puts1(" ");
	putint(v);
	puts1("\n");
}

jmp_buf	top;
int	depth;

void
deep(int n, int v)
{
	char pad[200];

	pad[0] = n;
	depth++;
	if(n == 0)
		longjmp(top, v);
	deep(n-1, v + pad[0]*0);
}

/* the kernel's way: a stack of labels, waserror() and nexterror() */
jmp_buf	errlab[8];
int	nerrlab;
#define	waserror()	(nerrlab++, setjmp(errlab[nerrlab-1]))
#define	poperror()	nerrlab--

void
error(int e)
{
	longjmp(errlab[--nerrlab], e);
}

void
nexterror(void)
{
	longjmp(errlab[--nerrlab], 1);
}

int
inner(int x)
{
	if(waserror()) {
		show("inner caught, passing on", x);
		nexterror();
	}
	if(x > 2)
		error(x);
	poperror();
	return x * 10;
}

int
outer(int x)
{
	int r;

	if(waserror())
		return -1;
	r = inner(x);
	poperror();
	return r;
}

/* locals in registers across the longjmp: this frame's, still there */
int
regs(void)
{
	int a, b;
	volatile int r;

	a = 7;
	b = 11;
	r = setjmp(top);
	if(r < 3)
		deep(5, r + 1);
	return a * b + r;
}

void
_main(void)
{
	int r, i, lock;
	long n;

	r = setjmp(top);
	show("setjmp first", r);
	if(r == 0)
		deep(10, 42);
	show("depth", depth);
	r = setjmp(top);
	if(r == 0)
		longjmp(top, 0);
	show("longjmp 0 gives", r);

	/* SP must come back: many longjmps from deep would overflow the stack */
	for(i = 0; i < 20000; i++) {
		depth = 0;
		if(setjmp(top) == 0)
			deep(20, 1);
	}
	show("20000 longjmps, last depth", depth);

	show("outer 1", outer(1));
	show("outer 5", outer(5));
	show("nerrlab", nerrlab);
	show("regs", regs());

	lock = 0;
	show("tas", _tas(&lock));
	show("tas again", _tas(&lock) != 0);
	n = 5;
	show("ainc", ainc(&n));
	show("adec", adec(&n));
	show("adec", adec(&n));
	show("n", n);
	lock = 3;
	show("cas 3->4", cas(&lock, 3, 4));
	show("cas 3->5", cas(&lock, 3, 5));
	show("lock", lock);
	exits1(0);
}
