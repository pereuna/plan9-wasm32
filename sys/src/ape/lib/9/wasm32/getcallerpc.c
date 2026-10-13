/*
 * wasm32: a function's caller is not in memory - none, as libc's
 * (port/getcallerpc.c)
 */
unsigned long
getcallerpc(void*)
{
	return 0;
}
