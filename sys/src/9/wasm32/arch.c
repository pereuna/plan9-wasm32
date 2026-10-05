#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

/*
 * wasm32: the machine's small part - no interrupts, no MMU, no FPU state;
 * atomics are 3l's (_tas, cas, coherence)
 */
extern	int	_tas(int*);
extern	int	cas(int*, int, int);

Mach	*machp[MAXMACH];
void	(*screenputs)(char*, int);

int
tas(void *p)
{
	return _tas(p);
}

int
cmpswap(long *p, long ov, long nv)
{
	return cas((int*)p, ov, nv);
}

/*
 * no interrupts: what spl* would keep out is other Workers, and locks do
 * that; but port/ checks the level (iunlock), so each CPU keeps one
 */
int
splhi(void)
{
	int s;

	s = m->splhigh;
	m->splhigh = 1;
	return s;
}

int
spllo(void)
{
	int s;

	s = m->splhigh;
	m->splhigh = 0;
	return s;
}

void
splx(int s)
{
	m->splhigh = s;
}

int
islo(void)
{
	return !m->splhigh;
}

void
idlehands(void)
{
	long x;

	x = 0;
	platwait(&x, 0, 10);
}

void	procsave(Proc*) { }
void	procrestore(Proc*) { }
void	procsetup(Proc *p) { p->fpstate = FPinit; }
void	procfork(Proc*) { }
void	fpunotify(Proc*) { }
void	fpunoted(Proc*) { }
void	mmuswitch(Proc*) { }
void	mmurelease(Proc*) { }
void	flushmmu(void) { }
void	mmuinit(void) { }
void	countpagerefs(ulong*, int) { }
void	evenaddr(uintptr) { }
void	validalign(uintptr, unsigned) { }

/* a kproc's start, on its Worker: procstart (proc.c) calls it */
void
kprocchild(Proc *p, void (*func)(void))
{
	p->sched.pc = (uintptr)func;
	p->sched.sp = 0;
}

uintptr	userpc(void) { return 0; }
uintptr	dbgpc(Proc*) { return 0; }
void	setkernur(Ureg*, Proc*) { }
void	setregisters(Ureg*, char*, char*, int) { }

void
delay(int ms)
{
	long x;

	x = 0;
	platwait(&x, 0, ms);
}

void
microdelay(int us)
{
	delay((us + 999) / 1000);
}

/* time: the platform's nanoseconds are the fast ticks */
uvlong
fastticks(uvlong *hz)
{
	if(hz != nil)
		*hz = 1000000000ULL;
	return platnsec();
}

ulong
perfticks(void)
{
	return platnsec() / 1000;
}

void
cycles(uvlong *t)
{
	*t = platnsec();
}

long
lcycles(void)
{
	return platnsec();
}

void
exit(int)
{
	plathalt("exit");
	for(;;)
		delay(1000);
}

void
reboot(void*, void*, ulong)
{
	exit(0);
}

void
rebootcmd(int, char**)
{
	error(Egreg);
}

int
userureg(Ureg*)
{
	return 0;
}

/*
 * what port/ wants from parts wasm32 has not (segments, dtrace,
 * the debugger) or has in its own way
 */
char	*configfile = "";
int	cpuserver;
void	(*proctrace)(Proc*, int, vlong);

void*	KADDR(uintptr pa) { return (void*)pa; }
uintptr	PADDR(void *va) { return (uintptr)va; }

uintptr
cankaddr(uintptr pa)
{
	if(pa >= 1*GiB)
		return 0;
	return 1*GiB - pa;
}

int	return0(void*) { return 0; }
void	dumpstack(void) { }
void	rdb(void) { }
void	segclock(uintptr) { }
void	putseg(Segment*) { }
void	dtracytick(Ureg*) { }

void*
vmemchr(void *s, int c, ulong n)
{
	return memchr(s, c, n);
}

ulong
µs(void)
{
	return platnsec() / 1000;
}

int
procfdprint(Chan *c, int fd, char *s, int ns)
{
	return snprint(s, ns, "%3d %.2s %C %4ld (%.16llux %lud %.2ux) %5ld %8lld %s\n",
		fd,
		&"r w rw"[(c->mode&3)<<1],
		devtab[c->type]->dc, c->dev,
		c->qid.path, c->qid.vers, c->qid.type,
		c->iounit, c->offset, c->path->s);
}

/*
 * random: the platform's (crypto.getRandomValues), as the CPU's RDRAND
 * on a PC - through hwrandbuf, so that port/bootinfo.c's
 * bootinforandinit() mixes BootInfo's seed in, once, as it does there
 */
static void
platrandbuf(void *p, ulong n)
{
	platrandom(p, n);
}

void (*hwrandbuf)(void*, ulong) = platrandbuf;

void
randominit(void)
{
}

ulong
randomread(void *p, ulong n)
{
	(*hwrandbuf)(p, n);
	return n;
}

void
genrandom(uchar *p, int n)
{
	(*hwrandbuf)(p, n);
}

/*
 * a system call's addresses are the kernel's here: the platform copied
 * what the program gave into the kernel's memory first (trap.c, C2b)
 */
void
validaddr(uintptr, ulong, int)
{
}

void	shrrenameuser(char*, char*) { }
