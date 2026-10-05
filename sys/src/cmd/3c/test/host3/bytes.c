#include <u.h>
#include <libc.h>

/*
 * 3c test: #t/eia0 is a byte line: every byte value out, a rune split
 * between two writes, and 256 bytes in (any values) echoed back
 */
void
main(int, char**)
{
	uchar b[256];
	int i, n, m;
	char *a;

	for(i = 0; i < 256; i++)
		b[i] = i;
	write(1, b, 256);
	a = "ä\n";		/* c3 a4: one byte, then the other */
	write(1, a, 1);
	write(1, a+1, 2);
	for(n = 0; n < 256; n += m)
		if((m = read(0, b+n, 256-n)) <= 0)
			exits("short read");
	write(1, b, 256);
	exits(nil);
}
