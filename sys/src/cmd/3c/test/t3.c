/*
 * 3c test: the MOVs (3l's emov) - every width loaded and stored, sign and
 * zero extension, truncation on store, offsets through pointers (also
 * past 2^16, and before the pointer: negative), unaligned addresses,
 * register to register narrowing, struct copies
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

typedef struct Mix Mix;
struct Mix
{
	char	c;
	unsigned char uc;
	short	s;
	unsigned short us;
	int	i;
	unsigned u;
	long long v;
	unsigned long long uv;
	float	f;
	double	d;
};

unsigned char	mem[70000];
Mix	gm, gm2;
signed char	gc = -100;
unsigned char	guc = 200;
short	gs = -30000;
unsigned short	gus = 60000;
long long	gv = -1234567890123456789LL;
float	gf = -2.5f;
double	gd = 1e-300;

/* the same bytes through each width: what loads see */
void
widths(unsigned char *p)
{
	show("char", *(signed char*)p);
	show("uchar", *(unsigned char*)p);
	show("short", *(short*)p);
	show("ushort", *(unsigned short*)p);
	show("int", *(int*)p);
	show("uint", *(unsigned*)p);
	show("vlong", *(long long*)p);
	show("uvlong>>1", (long long)(*(unsigned long long*)p >> 1));
}

/* stores truncate: a wide value into each narrow place */
void
stores(unsigned char *p, long long v)
{
	int i;

	for(i = 0; i < 16; i++)
		p[i] = 0xee;
	*(signed char*)p = v;
	*(short*)(p+2) = v;
	*(int*)(p+4) = v;
	*(long long*)(p+8) = v;
	for(i = 0; i < 16; i++)
		show("byte", p[i]);
}

int
negoff(int *p)
{
	return p[-1] + p[-2]*10 + p[1]*100;
}

Mix
mixcopy(Mix m)
{
	m.i++;
	return m;
}

void
_main(void)
{
	int i, a[4];
	unsigned char *p;
	char c;
	short s;
	unsigned short us;
	long long v;
	float f;
	double d;
	Mix m, m2;

	/* globals of every width */
	show("gc", gc);
	show("guc", guc);
	show("gs", gs);
	show("gus", gus);
	show("gv", gv);
	show("gf*10", (int)(gf*10));
	show("gd*1e300", (int)(gd*1e300+0.5));

	/* loads of every width, aligned and not, near and far */
	for(i = 0; i < 16; i++)
		mem[i] = 0xf0 + i;
	widths(mem);
	widths(mem+1);
	widths(mem+3);
	for(i = 0; i < 16; i++)
		mem[65537+i] = 0x81 + 7*i;
	widths(mem+65537);

	/* stores truncate */
	stores(mem+100, -2);
	stores(mem+201, 0x0102030405060708LL);

	/* register to register: narrowing and widening */
	i = 0x1ff;
	c = i;
	show("c", c);
	s = 0x18000;
	show("s", s);
	us = -1;
	show("us", us);
	i = c;
	show("i=c", i);
	v = us;
	show("v=us", v);
	v = s;
	show("v=s", v);
	v = (unsigned)0x80000000;
	show("v=u", v);
	v = (int)0x80000000;
	show("v=i", v);
	f = v;
	show("f=v", (long long)f);
	d = f;
	show("d=f", (long long)d);
	i = d / 1024;
	show("i=d", i);

	/* negative and positive offsets through a pointer */
	a[0] = 1; a[1] = 2; a[2] = 3; a[3] = 4;
	show("negoff", negoff(a+2));

	/* structs: every field through copies */
	m.c = -5; m.uc = 250; m.s = -1000; m.us = 65000; m.i = -70000; m.u = 4000000000u;
	m.v = -5000000000LL; m.uv = 18000000000000000000ULL; m.f = 0.75f; m.d = -3.125;
	gm = m;
	m2 = mixcopy(gm);
	gm2 = m2;
	show("m.c", gm2.c);
	show("m.uc", gm2.uc);
	show("m.s", gm2.s);
	show("m.us", gm2.us);
	show("m.i", gm2.i);
	show("m.u", gm2.u);
	show("m.v", gm2.v);
	show("m.uv>>4", (long long)(gm2.uv >> 4));
	show("m.f*100", (int)(gm2.f*100));
	show("m.d*1000", (int)(gm2.d*1000));
	p = (unsigned char*)&gm2;
	for(i = 0; i < (int)sizeof gm2; i += 8)
		show("raw", *(long long*)(p+i) & 0xffffffffffLL);
	exits1(0);
}
