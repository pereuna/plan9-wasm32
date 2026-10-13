/*
 * wasm32: round to nearest, no traps, nothing to set (libc's
 * wasm32/getfcr.c)
 */
unsigned long
getfcr(void)
{
	return 0;
}

void
setfcr(unsigned long)
{
}

unsigned long
getfsr(void)
{
	return 0;
}

void
setfsr(unsigned long)
{
}
