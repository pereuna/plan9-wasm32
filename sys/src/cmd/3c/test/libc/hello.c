#include <u.h>
#include <libc.h>

/*
 * 3c test with 9front's libc: hello.out is what it prints
 */
static int
cmp(void *a, void *b)
{
	return *(int*)a - *(int*)b;
}

void
main(int argc, char **argv)
{
	int i, *v;
	char *s, *f[8], buf[64];
	Rune r;
	double d;
	vlong big;

	print("hello from 3c, %d args, argv0 %s\n", argc, argv0 != nil ? "set" : "nil");
	for(i = 0; i < argc; i++)
		print("arg %d: %s\n", i, argv[i]);
	print("%d %5d|%-5d| %x %X %o %llud %lld\n", 42, 7, 7, 255, 255, 8, ~0ULL, -1234567890123LL);
	print("%f %.3f %g %e %g\n", 3.14159, 2.0/3, 1e100, 12345.678, 0.0001);
	print("%s|%10s|%-10s|%.3s\n", "str", "right", "left", "truncate");
	print("%c %C %q %q\n", 'x', 0x263a, "plain", "needs quote");
	s = smprint("smprint %d+%d=%d", 2, 3, 2+3);
	print("%s (%ld bytes)\n", s, strlen(s));
	free(s);
	v = malloc(10*sizeof(int));
	for(i = 0; i < 10; i++)
		v[i] = (i*7919) % 23;
	qsort(v, 10, sizeof(int), cmp);
	for(i = 0; i < 10; i++)
		print("%d%c", v[i], i < 9 ? ' ' : '\n');
	free(v);
	print("strtol %ld %ld %lud\n", strtol("-123", nil, 10), strtol("0x1f", nil, 0), strtoul("777", nil, 8));
	d = atof("2.5e3");
	print("atof %g sqrt %g pow %g\n", d, sqrt(2.0), pow(2.0, 10.0));
	strcpy(buf, "a b  c\td");
	i = tokenize(buf, f, nelem(f));
	print("tokenize %d: %s %s %s %s\n", i, f[0], f[1], f[2], f[3]);
	chartorune(&r, "ä");
	print("rune %ux utflen %d\n", r, utflen("äöå"));
	big = 1;
	for(i = 0; i < 40; i++)
		big *= 3;
	print("3^40 %lld\n", big);
	print("cleanname %s\n", cleanname(strcpy(buf, "/a/b/../c/./d//")));
	for(i = 0; i < 1000; i++)
		free(malloc(i*13));
	print("malloc ok\n");
	exits(nil);
}
