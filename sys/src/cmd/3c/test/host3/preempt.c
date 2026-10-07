#include <u.h>
#include <libc.h>

/*
 * 3c test: rfork(RFMEM) procs are preempted - parent and child each wait
 * for the other in a loop without a system call; without preemption the
 * first to spin would keep the other out for ever
 */
volatile int progress, flag;

void
main(int, char**)
{
	int i;
	Waitmsg *w;

	switch(rfork(RFPROC|RFMEM)) {
	case -1:
		sysfatal("rfork: %r");
	case 0:
		for(i = 0; i < 1000; i++)
			progress++;
		while(flag == 0)
			;		/* the parent's turn */
		print("child saw the flag\n");
		exits(nil);
	}
	while(progress < 1000)
		;			/* the child's turn */
	print("parent saw progress %d\n", progress);
	flag = 1;
	w = wait();
	print("child said '%s'\n", w != nil ? w->msg : "nothing");
	exits(nil);
}
