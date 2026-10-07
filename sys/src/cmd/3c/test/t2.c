/*
 * 3c test: the harder parts
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

#ifdef __GNUC__
#include <stdarg.h>
#else
/* Plan 9's 386 va_list: what wasm32's u.h will have */
typedef char *va_list;
#define va_start(list, start) list = (sizeof(start) < 4 ? (char*)((int*)&(start)+1) : (char*)(&(start)+1))
#define va_end(list)
#define va_arg(list, mode)\
	((sizeof(mode) == 1)?\
		((list += 4), (mode*)list)[-4]:\
	(sizeof(mode) == 2)?\
		((list += 4), (mode*)list)[-2]:\
		((list += sizeof(mode)), (mode*)list)[-1])
#endif

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

/* varargs: 'd' int, 'v' vlong, 'c' char, 's' string, 'f' double */
void
vshow(char *fmt, ...)
{
	va_list a;

	va_start(a, fmt);
	for(; *fmt; fmt++)
		switch(*fmt) {
		case 'd': show("vd", va_arg(a, int)); break;
		case 'v': show("vv", va_arg(a, long long)); break;
		case 'c': show("vc", va_arg(a, int)); break;
		case 's': puts1(va_arg(a, char*)); puts1("\n"); break;
		case 'f': show("vf*100", (long long)(va_arg(a, double)*100)); break;
		}
	va_end(a);
}

typedef struct Bits Bits;
struct Bits
{
	unsigned a:3;
	int b:5;
	unsigned c:12;
	int d;
};

typedef union U U;
union U
{
	int	i;
	float	f;
	char	b[4];
};

int	add(int a, int b) { return a + b; }
int	sub(int a, int b) { return a - b; }
int	mul(int a, int b) { return a * b; }
int	(*ops[])(int, int) = { add, sub, mul };
char	*names[] = { "add", "sub", "mul" };
int	*pg;
int	gv = 77;
int	**ppg = &pg;

int
counter(void)
{
	static int n;

	return ++n;
}

int
gotos(int n)
{
	int s;

	s = 0;
again:
	if(n <= 0)
		goto out;
	s += n;
	n--;
	goto again;
out:
	return s;
}

long long
vswitch(long long v)
{
	switch(v) {
	case 1LL<<40: return 1;
	case -5: return 2;
	case 7: return 3;
	case 1LL<<33: return 4;
	case 9: return 5;
	case 11: return 6;
	}
	return 0;
}

double
dsum(double *d, int n)
{
	double s;

	s = 0;
	while(n-- > 0)
		s += *d++;
	return s;
}

typedef struct Big Big;
struct Big
{
	char	name[20];
	int	v[10];
	double	d;
};

Big
bigmk(int k)
{
	Big b;
	int i;

	for(i = 0; i < 10; i++)
		b.v[i] = k*i;
	b.d = k/4.0;
	b.name[0] = 'A'+k;
	b.name[1] = 0;
	return b;
}

int
bigsum(Big b)
{
	int i, s;

	s = 0;
	for(i = 0; i < 10; i++)
		s += b.v[i];
	return s;
}

void
_main(void)
{
	Bits bt;
	U u;
	int i, x, y;
	unsigned char uc;
	char c;
	double d[5];
	float f;
	Big b1, b2;

	vshow("dvcsf", 42, 1LL<<50, 'x', "a string", 3.25);
	vshow("ddd", -1, 0, 1);
	bt.a = 13;
	bt.b = -7;
	bt.c = 4095;
	bt.d = 5;
	show("bt.a", bt.a);
	show("bt.b", bt.b);
	show("bt.c", bt.c);
	bt.c += 2;
	show("bt.c+2", bt.c);
	bt.b++;
	show("bt.b++", bt.b);
	show("bt.d", bt.d);
	u.f = 1.0;
	show("u.i", u.i);
	u.i = 0x41424344;
	show("u.b0", u.b[0]);
	for(i = 0; i < 3; i++) {
		puts1(names[i]);
		show("", ops[i](10, 3));
	}
	pg = &gv;
	show("**ppg", **ppg);
	counter();
	counter();
	show("counter", counter());
	show("gotos", gotos(10));
	show("vswitch", vswitch(1LL<<40) + 10*vswitch(-5) + 100*vswitch(1LL<<33) + 1000*vswitch(11) + 10000*vswitch(12));
	for(i = 0; i < 5; i++)
		d[i] = i * 0.5;
	show("dsum*10", (int)(dsum(d, 5)*10));
	x = 7;
	x *= 2.5;
	show("x*=2.5", x);
	y = 100;
	y /= 0.3;
	show("y/=0.3", y);
	uc = 250;
	uc += 10;
	show("uc+=10", uc);
	c = 127;
	c++;
	show("c++", c);
	f = 1.0f/3;
	show("f*1e6", (int)(f*1000000));
	show("cmpu", (unsigned)-1 > 1u);
	show("cmps", -1 > 1);
	b1 = bigmk(3);
	b2 = b1;
	b2.v[9] = 1000;
	show("bigsum b1", bigsum(b1));
	show("bigsum b2", bigsum(b2));
	show("b1.d*4", (int)(b1.d*4));
	puts1(b2.name);
	puts1("\n");
	i = 0;
	do {
		i += 3;
		if(i == 6)
			continue;
		if(i > 20)
			break;
	} while(i < 100);
	show("dowhile", i);
	show("comma", (i = 5, i * 2));
	show("sizeof Big", sizeof(Big));
	show("sizeof Bits", sizeof(Bits));
	exits1(0);
}
