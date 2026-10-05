#include <u.h>
#include <libc.h>

/*
 * 3c test: fork (3l unwinds the stack into memory, the kernel copies the
 * memory to the child, both rewind): from deep in a recursion, with
 * registers, the stack and the heap kept; nested; wait's message
 */
int	gcount = 100;

int
deepfork(int n, int acc, char *heap)
{
	int r, pid;
	char local[32];
	double d;

	snprint(local, sizeof local, "level%d", n);
	d = n * 1.5;
	if(n > 0)
		return deepfork(n-1, acc + n, heap) + 1;
	pid = fork();
	if(pid < 0)
		sysfatal("fork: %r");
	r = acc * 1000 + (pid == 0);
	gcount++;
	heap[0] = pid == 0 ? 'c' : 'p';
	if(pid == 0) {
		print("child: acc %d local %s d %g heap %s gcount %d\n", acc, local, d, heap, gcount);
		exits(heap[0] == 'c' ? "child done" : "wrong heap");
	}
	return r;
}

void
main(int, char**)
{
	char *heap;
	int r, i;
	Waitmsg *w;

	heap = malloc(16);
	strcpy(heap, "xheap");
	r = deepfork(5, 0, heap);
	w = wait();
	if(w == nil)
		sysfatal("wait: %r");
	print("parent: r %d heap %s gcount %d, child said '%s'\n", r, heap, gcount, w->msg);
	free(w);

	/* again, and a grandchild */
	switch(fork()) {
	case -1:
		sysfatal("fork: %r");
	case 0:
		switch(fork()) {
		case 0:
			print("grandchild\n");
			exits(nil);
		default:
			w = wait();
			print("child: grandchild said '%s'\n", w->msg);
			exits("child 2 done");
		}
	default:
		w = wait();
		print("parent: child 2 said '%s'\n", w->msg);
	}
	for(i = 0; i < 3; i++)
		if(fork() == 0)
			exits(smprint("child %d", i));
	for(i = 0; i < 3; i++) {
		w = wait();
		print("reaped one: %s\n", strstr(w->msg, "child ") != nil ? "ok" : w->msg);	/* Plan 9's: "prog pid: child N" */
		free(w);
	}
	exits(nil);
}
