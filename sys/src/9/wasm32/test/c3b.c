#include <u.h>
#include <libc.h>

/*
 * C3b (tools/test-9wasm32): an rfork(RFMEM) whose Worker the page does
 * not make (?failhelper=1: the parent's helper, ?failrfmem=1: the
 * child's) gives -1, and the parent is as it was: it can exec.
 */
void
main(void)
{
	int pid;

	pid = rfork(RFPROC|RFMEM);
	if(pid == 0)
		exits("the child ran");
	print("rfork(RFMEM): %d %r\n", pid);
	execl("/bin/echo", "echo", "exec after it: ok", nil);
	print("exec: %r\n");
	exits("exec");
}
