/* the tests under gcc: main calls _main, for tools/test-3c's comparison */
#include <stdarg.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

extern void _main(void);

int
main(void)
{
	_main();
	return 0;
}
