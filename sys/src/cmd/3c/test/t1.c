/*
 * 3c test: no libc; _trap(n, args) is the system call (3l makes it)
 */
#ifdef __GNUC__
#include <unistd.h>
#include <stdlib.h>
long pwrite1(int fd, void *s, long n, long long off) { return write(fd, s, n); }
void exits1(char *s) { exit(s != 0 && *s != 0); }
#else
long long _trap(int, void*);
long pwrite1(int fd, void *s, long n, long long off) { return _trap(51, &fd); }
void exits1(char *s) { _trap(8, &s); }
#endif

enum { PWRITE = 51, EXITS = 8 };

typedef struct Pt Pt;
struct Pt
{
	int	x;
	char	c;
	double	d;
	long long v;
};

int	g = 5;
char	*msg = "hello, wasm32\n";
int	arr[6] = { 3, 1, 4, 1, 5, 9 };
Pt	gpt = { 7, 'q', 2.5, 1234567890123LL };
static int counter;

void
puts1(char *s)
{
	int n;

	for(n = 0; s[n]; n++)
		;
	pwrite1(1, s, n, -1LL);
}

void
putint(long long v)
{
	char buf[32];
	int i, neg;

	i = sizeof buf - 1;
	buf[i] = 0;
	neg = v < 0;
	if(neg)
		v = -v;
	do {
		buf[--i] = '0' + v % 10;
		v /= 10;
	} while(v);
	if(neg)
		buf[--i] = '-';
	puts1(buf+i);
}

void
show(char *what, long long v)
{
	puts1(what);
	puts1(" ");
	putint(v);
	puts1("\n");
}

int
fib(int n)
{
	if(n < 2)
		return n;
	return fib(n-1) + fib(n-2);
}

int
sum(int *p, int n)
{
	int i, s;

	s = 0;
	for(i = 0; i < n; i++)
		s += p[i];
	return s;
}

int
sw(int x)
{
	switch(x) {
	case 1:	return 10;
	case 2:	return 20;
	case 7:	return 70;
	case 100: return 1000;
	case 101: return 1010;
	case 102: return 1020;
	case 200: return 2000;
	default: return -1;
	}
}

int	add(int a, int b) { return a + b; }
int	mul(int a, int b) { return a * b; }

int
apply(int (*f)(int, int), int a, int b)
{
	return f(a, b);
}

Pt
mkpt(int x, double d)
{
	Pt p;

	p.x = x;
	p.c = 'z';
	p.d = d;
	p.v = (long long)x * 1000000000LL;
	return p;
}

void
sort(int *a, int n)
{
	int i, j, t;

	for(i = 1; i < n; i++)
		for(j = i; j > 0 && a[j-1] > a[j]; j--) {
			t = a[j];
			a[j] = a[j-1];
			a[j-1] = t;
		}
}

int
strcmp1(char *a, char *b)
{
	while(*a && *a == *b) {
		a++;
		b++;
	}
	return *(unsigned char*)a - *(unsigned char*)b;
}

void
_main(void)
{
	int i, (*fp)(int, int);
	Pt p, q;
	char c;
	unsigned u;
	short sh;
	double d;
	long long v;

	puts1(msg);
	show("g", g);
	show("fib20", fib(20));
	show("sum", sum(arr, 6));
	for(i = 0; i < 9; i++)
		show("sw", sw(i == 8 ? 101 : i));
	fp = add;
	show("apply add", apply(fp, 3, 4));
	show("apply mul", apply(mul, 3, 4));
	p = mkpt(42, 1.5);
	show("pt.x", p.x);
	show("pt.c", p.c);
	show("pt.d*10", (int)(p.d*10));
	show("pt.v", p.v);
	q = gpt;
	show("gpt.x", q.x);
	show("gpt.c", q.c);
	show("gpt.v", q.v);
	sort(arr, 6);
	for(i = 0; i < 6; i++)
		show("arr", arr[i]);
	c = 200;
	show("char 200", c);
	u = 0xffffffff;
	show("u/16", u/16);
	show("u>>28", u>>28);
	sh = -3;
	show("short", sh);
	show("(ushort)sh", (unsigned short)sh);
	d = 1.0;
	for(i = 0; i < 10; i++)
		d = d*1.5 + 0.25;
	show("d*1000", (long long)(d*1000));
	v = 1;
	for(i = 0; i < 62; i++)
		v <<= 1;
	show("1<<62", v);
	show("v/7", v/7);
	show("v%1000", v%1000);
	show("-v>>3", (-v)>>3);
	show("cmp", strcmp1("abc", "abd") < 0);
	show("ternary", g > 3 ? 111 : 222);
	show("andand", g > 3 && fib(5) == 5);
	show("oror", g < 3 || fib(6) == 8);
	for(i = 0; i < 5; i++)
		counter += i;
	show("counter", counter);
	i = 10;
	i += 5;
	i *= 3;
	i -= 1;
	i /= 2;
	i %= 7;
	i <<= 4;
	i |= 3;
	i ^= 1;
	show("asops", i);
	show("postinc", i++);
	show("preinc", ++i);
	exits1(0);
}
