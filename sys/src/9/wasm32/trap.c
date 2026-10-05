#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"
#include	"../../libc/9syscall/sys.h"

/*
 * wasm32's system calls (docs/architecture.md, phase C).  A program is
 * a module of its own on its proc's Worker, its memory its own; its
 * plan9.syscall(n, a) comes here (platform.js, platuser), a the address
 * of its arguments - 4-byte words, a vlong two - in its memory.  The
 * arguments are copied into the kernel and so are the strings and
 * buffers they point at, so that 9front's sys* (sysfile.c) and the
 * devices see the kernel's addresses only; what a call wrote goes back
 * after it.
 *
 * A call's args say what its words are:
 *	i	a word
 *	v	a vlong: two words
 *	s	a string the call reads; S the same or nil
 *	r	a buffer the call reads, and the next word its length
 *	w	a buffer the call writes, its length next: as much copied out
 *		as the call returns
 *	W	a buffer the call reads and writes, its length next: all of it
 *		copied out
 *	p	an int[2] the call writes (pipe)
 *	A	an argv: its strings, to the nil, copied in (exec)
 */
enum
{
	YIELD	= 105,	/* 3l's preemption point */
	Nwords	= 8,
	Nbufs	= 4,
	Maxbuf	= 16*1024*1024,
	Maxstr	= 8192,
	Maxargv	= 1024,
	Maxargvsize	= 64*1024,
};

typedef uintptr Syscall(va_list);
Syscall	sysbind, syschdir, sysclose, sysdup, sysalarm, sysexec, sysexits,
	sysfauth, sysopen, syssleep, sysrfork, syspipe, syscreate, sysfd2path,
	sysbrk_, sysremove, sysnotify, sysnoted, sysunmount, sysfversion,
	syserrstr, sysstat, sysfstat, syswstat, sysfwstat, sysmount, sysawait,
	syspread, syspwrite, sysrendezvous, sysyield;
vlong	sysseekv(va_list);
vlong	sysnsecv(va_list);

typedef struct Sys Sys;
struct Sys
{
	uintptr	(*f)(va_list);
	vlong	(*fv)(va_list);		/* calls with a vlong result */
	char	*args;
};

static Sys systab[] =
{
[BIND]		sysbind, nil, "ssi",
[CHDIR]		syschdir, nil, "s",
[CLOSE]		sysclose, nil, "i",
[DUP]		sysdup, nil, "ii",
[ALARM]		sysalarm, nil, "i",
[EXEC]		sysexec, nil, "sA",
[EXITS]		sysexits, nil, "S",
[FAUTH]		sysfauth, nil, "is",
[OPEN]		sysopen, nil, "si",
[SLEEP]		syssleep, nil, "i",
[RFORK]		sysrfork, nil, "i",
[PIPE]		syspipe, nil, "p",
[CREATE]	syscreate, nil, "sii",
[FD2PATH]	sysfd2path, nil, "iW",
[BRK_]		sysbrk_, nil, "i",
[REMOVE]	sysremove, nil, "s",
[NOTIFY]	sysnotify, nil, "i",
[NOTED]		sysnoted, nil, "i",
[UNMOUNT]	sysunmount, nil, "Ss",
[SEEK]		nil, sysseekv, "ivi",
[FVERSION]	sysfversion, nil, "iiW",
[ERRSTR]	syserrstr, nil, "W",
[STAT]		sysstat, nil, "sw",
[FSTAT]		sysfstat, nil, "iw",
[WSTAT]		syswstat, nil, "sr",
[FWSTAT]	sysfwstat, nil, "ir",
[MOUNT]		sysmount, nil, "iisiS",
[AWAIT]		sysawait, nil, "w",
[PREAD]		syspread, nil, "iwv",
[PWRITE]	syspwrite, nil, "irv",
[_NSEC]		nil, sysnsecv, "",
[RENDEZVOUS]	sysrendezvous, nil, "ii",
[YIELD]		sysyield, nil, "",
};

typedef struct Call Call;
struct Call
{
	ulong	u[Nwords];	/* the program's words */
	ulong	k[Nwords];	/* the kernel's: the va_list */
	struct {
		void	*k;
		ulong	u;
		long	n;
		int	out;	/* w, W, p: copied out */
		int	upto;	/* w: as much as the call returns */
	} b[Nbufs];
	int	nb;
	int	n;
	Sys	*s;
	vlong	r;
	int	failed;		/* an error before the call or in it: r -1, errstr the call's */
	int	heap;		/* malloc'd (sysprep), not on syscall's stack */
};

static int
nwords(char *a)
{
	int n;

	for(n = 0; *a != 0; a++)
		n += (*a == 'v' || *a == 'r' || *a == 'w' || *a == 'W') ? 2 : 1;
	return n;
}

static void*
callbuf(Call *c, ulong u, long n, int in, int out, int upto)
{
	void *k;

	if(n < 0 || n > Maxbuf)
		error(Etoobig);
	if(c->nb == Nbufs)
		panic("callbuf");
	k = malloc(n+1);
	if(k == nil)
		error(Enomem);
	c->b[c->nb].k = k;
	c->b[c->nb].u = u;
	c->b[c->nb].n = n;
	c->b[c->nb].out = out;
	c->b[c->nb].upto = upto;
	c->nb++;
	if(in && platcopyin(k, u, n) < 0)
		error(Ebadarg);
	((char*)k)[n] = 0;
	return k;
}

static char*
callstr(Call *c, ulong u)
{
	long n;

	n = platustrlen(u, Maxstr);
	if(n < 0)
		error(Ebadarg);
	return callbuf(c, u, n, 1, 0, 0);
}

/* an argv: the strings to its nil, the kernel's array of them */
static char**
callargv(Call *c, ulong u)
{
	char **argv, *s;
	ulong a;
	long n, used;
	int i;

	argv = callbuf(c, 0, (Maxargv+1)*sizeof(char*) + Maxargvsize, 0, 0, 0);
	s = (char*)(argv + Maxargv+1);
	used = 0;
	for(i = 0; ; i++){
		if(i == Maxargv)
			error(Etoobig);
		if(platcopyin(&a, u + i*BY2WD, BY2WD) < 0)
			error(Ebadarg);
		if(a == 0)
			break;
		n = platustrlen(a, Maxargvsize-used-1);
		if(n < 0)
			error(Etoobig);
		if(platcopyin(s+used, a, n) < 0)
			error(Ebadarg);
		s[used+n] = 0;
		argv[i] = s+used;
		used += n+1;
	}
	argv[i] = nil;
	return argv;
}

/* the program's words to the kernel's */
static void
marshal(Call *c, char *a)
{
	int i;

	for(i = 0; *a != 0; a++){
		switch(*a){
		case 'i':
			c->k[i] = c->u[i];
			i++;
			break;
		case 'v':
			c->k[i] = c->u[i];
			c->k[i+1] = c->u[i+1];
			i += 2;
			break;
		case 'S':
			if(c->u[i] == 0){
				c->k[i++] = 0;
				break;
			}
			/* fall through */
		case 's':
			c->k[i] = (ulong)callstr(c, c->u[i]);
			i++;
			break;
		case 'r':
		case 'w':
		case 'W':
			c->k[i] = (ulong)callbuf(c, c->u[i], c->u[i+1], *a != 'w', *a != 'r', *a == 'w');
			c->k[i+1] = c->u[i+1];
			i += 2;
			break;
		case 'p':
			c->k[i] = (ulong)callbuf(c, c->u[i], 2*sizeof(int), 0, 1, 0);
			i++;
			break;
		case 'A':
			c->k[i] = (ulong)callargv(c, c->u[i]);
			i++;
			break;
		}
	}
}

/* what the call wrote, back to the program */
static void
unmarshal(Call *c, vlong r)
{
	int i;
	long n;

	for(i = 0; i < c->nb; i++){
		if(!c->b[i].out)
			continue;
		n = c->b[i].n;
		if(c->b[i].upto && r < n)
			n = r;
		if(n > 0 && platcopyout(c->b[i].u, c->b[i].k, n) < 0)
			error(Ebadarg);
	}
}

/* the program's handler is done: noted, or it jumped out (notejmp) */
static void
notedone(Proc *p)
{
	p->notified = 0;
}

/*
 * a note that ends the proc, on the memory's Worker (rfork RFMEM): its
 * helper ends it (hcall nil) - pexit is not the memory's Worker's to do
 */
int
helperdie(void)
{
	if(up->memdone == nil || m->helper)
		return 0;
	up->hdie = 1;
	up->hcall = nil;
	up->hdone = 0;
	coherence();
	up->hreq = 1;
	platwake(&up->hreq, 1);
	return 1;
}

/*
 * a note for the program, when its call returns: its handler gets it
 * (platform.js); popnote ends the proc if it has none or is in it.  1
 * if the proc ends on its helper
 */
static int
usernote(void)
{
	char *msg;
	Note *n;

	if(up->nnote == 0)
		return 0;
	qlock(&up->debug);
	n = up->note[0];
	if(!(up->notified && n->flag == NUser) && (up->notify == nil || up->notified) && helperdie()){
		qunlock(&up->debug);
		return 1;
	}
	msg = popnote(nil);
	if(msg == nil){
		qunlock(&up->debug);
		return 0;
	}
	platnote(up->notify, msg, notedone, up);
	qunlock(&up->debug);
	return 0;
}

/* the error is the call's (errstr): 9front's syscall does the same */
static void
callerr(Call *c)
{
	char *e;

	e = up->syserrstr;
	up->syserrstr = up->errstr;
	up->errstr = e;
	c->r = -1;
	c->failed = 1;
}

/* the program's arguments into the kernel */
static void
callprep(Call *c, int n, ulong a)
{
	memset(c, 0, sizeof *c);
	c->n = n;
	up->insyscall = 1;
	up->scallnr = n;
	if(waserror()){
		callerr(c);
		return;
	}
	if(n < 0 || n >= nelem(systab) || systab[n].args == nil){
		pprint("bad sys call number %d\n", n);
		error(Ebadarg);
	}
	c->s = &systab[n];
	if(platcopyin(c->u, a, nwords(c->s->args)*BY2WD) < 0)
		error(Ebadarg);
	marshal(c, c->s->args);
	poperror();
}

/* the call itself: 9front's sys* and the devices, the kernel's addresses */
static void
callrun(Call *c)
{
	int nerrlab;

	if(c->failed)
		return;
	nerrlab = up->nerrlab;
	if(waserror()){
		callerr(c);
		return;
	}
	if(c->s->fv != nil)
		c->r = (*c->s->fv)((va_list)c->k);
	else
		c->r = (long)(*c->s->f)((va_list)c->k);
	poperror();
	if(up->nerrlab != nerrlab){
		print("bad errstack [%d]: %d extra\n", c->n, up->nerrlab - nerrlab);
		up->nerrlab = nerrlab;
	}
}

/* what it wrote, back to the program; the result */
static vlong
callfin(Call *c)
{
	int i;

	if(!c->failed){
		if(waserror())
			callerr(c);
		else{
			unmarshal(c, c->r);
			poperror();
		}
	}
	for(i = 0; i < c->nb; i++)
		free(c->b[i].k);
	up->insyscall = 0;
	return c->r;
}

/*
 * the program's call (platform.js calls it on the proc's Worker, in
 * the kernel's instance): -1 and the proc's syserrstr on an error
 */
vlong
syscall(int n, ulong a)
{
	Call c;
	vlong r;

	callprep(&c, n, a);
	up->hcall = &c;
	callrun(&c);
	up->hcall = nil;
	r = callfin(&c);
	usernote();
	return r;
}

/* p's call does not return (pexit): its buffers, and it if it is malloc'd */
void
callabort(Proc *p)
{
	Call *c;
	int i;

	if((c = p->hcall) == nil)
		return;
	p->hcall = nil;
	for(i = 0; i < c->nb; i++)
		free(c->b[i].k);
	c->nb = 0;
	if(c->heap)
		free(c);
}

/*
 * rfork(RFMEM): the memory's Worker runs its procs' programs in turn.
 * A call is prepared there (sysprep: the arguments copied in), done
 * there if it is quick or the platform's (rfork, exec, brk, noted ...)
 * or else by the proc's helper, and finished there (sysfin: copied out).
 * up is the calling proc's on the memory's Worker as on the helper.
 */
static int
local(int n)
{
	switch(n){
	case RFORK:
	case BRK_:
	case NOTIFY:
	case NOTED:
	case ERRSTR:
	case ALARM:
	case _NSEC:
	case YIELD:
		return 1;
	}
	return 0;
}

/* 1 done here, 0 its helper has it */
static int
sysprep(Proc *p, int n, ulong a)
{
	Call *c;

	up = p;
	c = malloc(sizeof(Call));
	if(c == nil)
		panic("sysprep: no memory");
	callprep(c, n, a);
	c->heap = 1;
	p->hcall = c;
	p->hdone = 0;
	if(c->failed || local(n)){
		callrun(c);
		return 1;
	}
	coherence();
	p->hreq = 1;
	platwake(&p->hreq, 1);
	return 0;
}

/* 0 not yet, 1 done, 2 the proc ended */
static int
sysdone(Proc *p)
{
	if(p->hexit)
		return 2;
	return p->hdone;
}

/* 0: its result is p->hret; 1: a note ends it, on its helper (sysdone) */
static int
sysfin(Proc *p)
{
	Call *c;

	up = p;
	c = p->hcall;
	p->hcall = nil;
	p->hdone = 0;
	p->hret = callfin(c);
	free(c);
	if(p->hdie)
		return 1;
	return usernote();
}

static vlong
sysret(Proc *p)
{
	return p->hret;
}

/* the memory's Worker is done with p: a Worker less on it */
static void
coend(Proc *p)
{
	procrelease(p);
}

/* and with the memory's group */
static void
memend(Umem *u)
{
	if(decref(u) == 0)
		free(u);
}

/* an rfork(RFMEM) proc's helper: its calls, on a Worker of its own */
void
helper(void)
{
	Call *c;

	for(;;){
		while(up->hreq == 0)
			platwait(&up->hreq, 0, -1);
		up->hreq = 0;
		if(up->hquit){
			/* the rfork that made it was undone (rfmemstart) */
			up->hquit = 0;
			return;
		}
		c = up->hcall;
		if(c == nil){
			/* a note ends it (usernote, noted NDFLT): popnote's pexit, or the last note's */
			qlock(&up->debug);
			if(up->nnote > 0)
				popnote(nil);
			qunlock(&up->debug);
			pexit(up->lastnote != nil ? up->lastnote->msg : "killed",
				up->lastnote == nil || up->lastnote->flag != NDebug);
		}
		callrun(c);
		if(c->n == EXEC && !c->failed){
			/*
			 * exec: the proc leaves the memory, its new program
			 * runs on this Worker (platexec gave it here); to the
			 * memory's Worker it is gone
			 */
			callabort(up);
			up->hexit = 1;
			up->hdone = 1;
			coherence();
			ainc(up->memdone);
			platwake(up->memdone, 1);
			umemrelease(up);
			m->helper = 0;
			platuser(&ufns, up, up->pid);
		}
		up->hdone = 1;
		coherence();
		ainc(up->memdone);
		platwake(up->memdone, 1);
	}
}

/* the kernel's functions the platform calls (platform.js, runuser) */
Ufns ufns = {
	syscall,
	sysprep,
	sysdone,
	sysfin,
	sysret,
	coend,
	memend,
};
