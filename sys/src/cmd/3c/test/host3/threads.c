#include <u.h>
#include <libc.h>
#include <thread.h>

/*
 * 3c test: libthread on wasm32 - threads are contexts of their proc
 * (switched by _ctxswitch), procs are rfork(RFMEM) procs; channels
 * between them, a prime sieve of threads
 */
Channel	*c, *done;

void
counter(void*)
{
	int i;

	for(i = 0; i < 5; i++)
		sendul(c, i*10);
	sendul(c, ~0);
	threadexits(nil);
}

void
procbody(void *v)
{
	Channel *r;
	int i, s;

	r = v;
	s = 0;
	for(i = 1; i <= 100; i++)
		s += i;
	sleep(20);
	sendul(r, s);
}

void
filter(void *v)
{
	Channel *in, *out;
	ulong p, n;

	in = v;
	p = recvul(in);
	if(p == ~0) {		/* the end: through all the filters */
		sendul(done, 1);
		threadexits(nil);
	}
	print("prime %lud\n", p);
	out = chancreate(sizeof(ulong), 0);
	threadcreate(filter, out, 16384);
	for(;;) {
		n = recvul(in);
		if(n == ~0) {
			sendul(out, n);
			threadexits(nil);
		}
		if(n % p)
			sendul(out, n);
	}
}

void
threadmain(int, char**)
{
	ulong v;
	Channel *r, *s;
	int i;

	c = chancreate(sizeof(ulong), 0);
	threadcreate(counter, nil, 16384);
	while((v = recvul(c)) != ~0)
		print("got %lud\n", v);

	r = chancreate(sizeof(ulong), 0);
	proccreate(procbody, r, 16384);
	print("proc says %lud\n", recvul(r));

	s = chancreate(sizeof(ulong), 0);
	done = chancreate(sizeof(ulong), 0);
	threadcreate(filter, s, 16384);
	for(i = 2; i < 30; i++)
		sendul(s, i);
	sendul(s, ~0);
	recvul(done);
	print("threads done\n");
	threadexitsall(nil);
}
