#include "gc.h"

/*
 * WebAssembly has i64 and the rest: nothing to make calls of
 */
int
machcap(Node *n)
{
	USED(n);
	return 1;
}
