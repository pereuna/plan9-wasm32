#include <u.h>
#include <libc.h>

/*
 * 3c test: rfork(RFPROC|RFMEM): the child shares the memory (globals,
 * heap), has its own stack - a copy at the same addresses, as Plan 9's
 * private stack segment: a stack address kept in a global is its own
 * there - and runs while the parent blocks and the other way round
 */
int	shared;
char	*heap;
int	*gstack;

void
child(int fd, int n)
{
	char buf[64], local[16];
	int i, r;

	strcpy(local, "child-local");
	for(i = 0; i < n; i++) {
		r = read(fd, buf, sizeof buf - 1);	/* blocks until the parent writes */
		if(r <= 0)
			break;
		buf[r] = 0;
		shared += 10;
		print("child got '%s', %s\n", buf, local);	/* shared now: as it happens */
	}
	heap[0] = 'C';
	*gstack += 100;		/* the parent's frame's address: the child's own copy */
	print("child: *gstack %d\n", *gstack);
	exits("child done");
}

void
main(int, char**)
{
	int p[2], pid, i;
	Waitmsg *w;
	char frame[32];
	int mine;

	mine = 7;
	gstack = &mine;
	heap = malloc(8);
	strcpy(heap, "h");
	strcpy(frame, "parent-frame");
	if(pipe(p) < 0)
		sysfatal("pipe: %r");
	pid = rfork(RFPROC|RFMEM);
	switch(pid) {
	case -1:
		sysfatal("rfork: %r");
	case 0:
		child(p[0], 3);
	}
	for(i = 0; i < 3; i++) {
		shared++;
		sleep(50);
		fprint(p[1], "msg%d", i);
		sleep(50);
	}
	w = wait();
	print("parent: shared %d heap %s %s mine %d, child said '%s'\n", shared, heap, frame, mine, w != nil ? w->msg : "nothing");
	exits(nil);
}
