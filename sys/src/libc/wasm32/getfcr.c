#include <u.h>
#include <libc.h>

/*
 * wasm32: round to nearest, no traps, nothing to set
 */
ulong
getfcr(void)
{
	return FPRNR;
}

void
setfcr(ulong)
{
}

ulong
getfsr(void)
{
	return 0;
}

void
setfsr(ulong)
{
}
