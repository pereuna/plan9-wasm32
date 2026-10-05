#include <u.h>
#include <libc.h>

/*
 * C3a (tools/test-9wasm32): notes - an alarm in a sleep, the handler's
 * noted(NCONT) and notejmp, no handler the end; rendezvous with a fork's
 * child; with -f, a fork whose child gets no Worker (the page's failfork)
 */
static jmp_buf jb;
static int jumping;

static void
handler(void*, char *msg)
{
	print("note: %s\n", msg);
	if(jumping)
		notejmp(nil, jb, 1);
	noted(NCONT);
}

void
main(int argc, char **argv)
{
	int pid;
	long r;
	uintptr v;
	Waitmsg *w;

	if(argc > 1 && strcmp(argv[1], "-f") == 0){
		pid = fork();
		if(pid == 0)
			exits("the child ran");
		print("fork: %d %r\n", pid);
		pid = fork();
		if(pid == 0)
			exits(nil);
		w = wait();
		print("the next fork: %s\n", w != nil && w->pid == pid ? "ok" : "wrong");
		exits(nil);
	}

	notify(handler);
	alarm(100);
	r = sleep(5000);
	print("sleep: %ld %r\n", r);

	jumping = 1;
	if(setjmp(jb) == 0){
		alarm(100);
		sleep(5000);
		print("not jumped\n");
	}else
		print("jumped out of the handler\n");
	jumping = 0;

	alarm(100);
	r = sleep(5000);
	print("again: %ld %r\n", r);

	/* fork is RFREND: the child's rendezvous group its own */
	switch(pid = rfork(RFFDG|RFPROC)){
	case -1:
		sysfatal("fork: %r");
	case 0:
		v = (uintptr)rendezvous((void*)7, (void*)22);
		print("child got %lud\n", (ulong)v);
		exits(nil);
	}
	v = (uintptr)rendezvous((void*)7, (void*)11);
	w = wait();
	print("parent got %lud, child %s\n", (ulong)v, w != nil && w->pid == pid ? "reaped" : "lost");
	free(w);

	if((pid = fork()) == 0){
		notify(nil);
		alarm(50);
		sleep(5000);
		exits("not killed");
	}
	w = wait();
	print("no handler: %s\n", w != nil && w->pid == pid ? w->msg : "lost");
	exits(nil);
}
