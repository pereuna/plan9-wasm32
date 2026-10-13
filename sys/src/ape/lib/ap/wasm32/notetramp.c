#include "../plan9/lib.h"
#include "../plan9/sys9.h"
#include <signal.h>
#include <setjmp.h>

/*
 * wasm32: a note's handler is a call the platform makes in the program
 * when a system call returns, and there is no pc to move to a handler
 * of its own (noted NSAVE): the signal's handler is called from the
 * note's, and the program goes on (plan9/signal.c: NCONT) - or it jumps
 * out, siglongjmp, as libc's notejmp
 */
void
_notetramp(int sig, void (*hdlr)(int, char*, Ureg*), Ureg *u)
{
	(*hdlr)(sig, _sigstring(sig), u);
}

void
siglongjmp(sigjmp_buf j, int ret)
{
	if(j[0])
		_psigblocked = j[1];
	longjmp(j+2, ret);
}
