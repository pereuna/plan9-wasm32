#include <u.h>
#include <libc.h>
#include <ureg.h>

/*
 * wasm32: setjmp and longjmp are 3l's (WebAssembly exceptions);
 * _tas, ainc, adec, cas, casp, casl and coherence its atomics
 */
void
notejmp(void*, jmp_buf j, int ret)
{
	longjmp(j, ret);
}
