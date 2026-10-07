#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

/*
 * wasm32's own system calls for procs and programs: no segments, no
 * a.out - a program is a WebAssembly module the platform instantiates
 * on the proc's Worker (platform.js: platexec, platuser), its memory
 * its own.  The rest are 9front's (sysproc.c), less what wasm32 has
 * not.  Their pointers are the kernel's (trap.c copied them in).
 */

enum
{
	Maxargs		= 1024,
	Maxargsize	= 64*1024,
	Maxshargs	= 16,
};

uintptr	sysseek(va_list);

typedef struct Exec Exec;
struct Exec
{
	Chan	*c;
	uchar	*img;
	long	nimg;
	char	*args;		/* argv's strings, one after another */
	long	nargs;
	int	argc;
};

/* the whole file: a program is one module */
static void
readimg(Exec *e, char *file)
{
	long n, sz;
	uchar *b;

	e->c = namec(file, Aopen, OEXEC, 0);
	if(e->c->qid.type & QTDIR)
		error(Eisdir);
	sz = 64*1024;
	e->img = malloc(sz);
	if(e->img == nil)
		error(Enomem);
	e->nimg = 0;
	for(;;){
		if(e->nimg == sz){
			sz *= 2;
			b = realloc(e->img, sz);
			if(b == nil)
				error(Enomem);
			e->img = b;
		}
		n = devtab[e->c->type]->read(e->c, e->img+e->nimg, sz-e->nimg, e->nimg);
		if(n <= 0)
			break;
		e->nimg += n;
	}
	cclose(e->c);
	e->c = nil;
}

static void
addarg(Exec *e, char *s)
{
	long n;

	n = strlen(s)+1;
	if(e->argc >= Maxargs || e->nargs+n > Maxargsize)
		error(Etoobig);
	memmove(e->args+e->nargs, s, n);
	e->nargs += n;
	e->argc++;
}

/*
 * file the next program with argv: its module to the platform, which
 * starts it when the system call is done (or touser)
 */
static void
exec1(char *file, char **argv, int argc)
{
	Exec e;
	char line[256], *f[Maxshargs], *p;
	int i, n;

	memset(&e, 0, sizeof e);
	if(waserror()){
		if(e.c != nil)
			cclose(e.c);
		free(e.img);
		free(e.args);
		nexterror();
	}
	e.args = malloc(Maxargsize);
	if(e.args == nil)
		error(Enomem);
	readimg(&e, file);
	if(e.nimg >= 2 && e.img[0] == '#' && e.img[1] == '!'){
		/* #!interpreter args: it gets them, the file, argv[1] on */
		n = e.nimg-2 < sizeof line-1 ? e.nimg-2 : sizeof line-1;
		memmove(line, e.img+2, n);
		line[n] = 0;
		if((p = strchr(line, '\n')) == nil)
			error(Ebadexec);
		*p = 0;
		n = tokenize(line, f, nelem(f));
		if(n == 0)
			error(Ebadexec);
		for(i = 0; i < n; i++)
			addarg(&e, f[i]);
		addarg(&e, file);
		for(i = 1; i < argc; i++)
			addarg(&e, argv[i]);
		free(e.img);
		e.img = nil;
		readimg(&e, f[0]);
	}else
		for(i = 0; i < argc; i++)
			addarg(&e, argv[i]);
	if(e.nimg < 8 || memcmp(e.img, "\0asm", 4) != 0)
		error(Ebadexec);
	if(platexec(e.img, e.nimg, e.args, e.nargs, e.argc) < 0)
		error(Ebadexec);
	poperror();
	free(e.img);
	free(e.args);

	/* the old program is gone: its close-on-exec files, its notify */
	for(i = 0; i <= up->fgrp->maxfd; i++)
		fdclose(i, CCEXEC);
	up->notify = nil;
	/* its name, and no args written for the program before: under debug, as 9front's sysexec (devproc reads them) */
	p = strrchr(file, '/');
	qlock(&up->debug);
	kstrdup(&up->text, p != nil ? p+1 : file);
	kstrdup(&up->args, "");
	up->nargs = 0;
	up->setargs = 0;
	qunlock(&up->debug);
}

/* the proc becomes its program (init's: main.c) */
_Noreturn void
touser(char **argv, int argc)
{
	exec1(argv[0], argv, argc);
	platuser(&ufns, up, up->pid);
}

/* the program's argv was copied in with the call (trap.c's A) */
uintptr
sysexec(va_list list)
{
	char *file, **argv;
	int argc;

	file = va_arg(list, char*);
	argv = va_arg(list, char**);
	for(argc = 0; argv[argc] != nil; argc++)
		;
	exec1(file, argv, argc);
	return 0;
}

uintptr
sysexits(va_list list)
{
	char *status, msg[ERRMAX+32];

	status = va_arg(list, char*);
	if(up == initp){
		/* init: the machine's end (9front panics) */
		snprint(msg, sizeof msg, "init exits%s%s", status != nil && *status ? ": " : "",
			status != nil ? status : "");
		plathalt(msg);
		for(;;)
			tsleep(&up->sleep, return0, nil, 1000000);
	}
	pexit(status, 1);
}

/* a fork's child: its program from the parent's memory (platform.js) */
static void
forkuser(void)
{
	platuser(&ufns, up, up->pid);
}

static void
abortion(void)
{
	pexit("fork aborted", 1);
}

/*
 * the platform's, when the parent has unwound and its memory is the
 * child's: the child's Worker, or the fork undone (-1, the error)
 */
static int
forkready(Proc *p)
{
	if(procspawn(p, Wfork) < 0){
		procunmake(p);
		kstrcpy(up->syserrstr, "fork: the page could not make a Worker", ERRMAX);
		return -1;
	}
	return 0;
}

/*
 * the platform's, for an rfork(RFMEM) child when the parent (up) has
 * unwound: all or nothing - the parent's helper if it has none yet, the
 * child's; then the parent is one of the memory's group, and the
 * memory's Worker holds the child and the group too.  -1: the child
 * undone, the parent as it was
 */
static int
rfmemstart(Proc *p)
{
	int first;

	first = up->memdone == nil;
	if(first && helperspawn(up) < 0){
		procunmake(p);
		kstrcpy(up->syserrstr, "rfork: the page could not make a Worker for a helper", ERRMAX);
		return -1;
	}
	if(procspawn(p, Wrfmem) < 0){
		procunmake(p);
		if(first)
			helperquit(up);
		kstrcpy(up->syserrstr, "rfork: the page could not make a Worker", ERRMAX);
		return -1;
	}
	ainc(&p->workers);
	if(first){
		up->umem = p->umem;
		incref(up->umem);	/* the parent's */
		incref(up->umem);	/* the memory's Worker's (memend) */
		up->memdone = &up->umem->done;
	}
	return 0;
}

/*
 * rfork(RFPROC): 9front's, less segments.  The child is made here, its
 * pid the result; the platform then unwinds the program, copies its
 * memory for the child and readies it (platfork), and both rewind - the
 * parent with the pid, the child with 0
 */
static uintptr
forkproc(ulong flag)
{
	static char nomntdevs[] = "|decp";
	Proc *p;
	Umem *u;
	ulong pid;

	if((p = newproc()) == nil)
		error("no procs");

	qlock(&up->debug);
	qlock(&p->debug);
	p->scallnr = up->scallnr;
	p->s = up->s;
	p->slash = up->slash;
	p->dot = up->dot;
	incref(p->dot);
	p->nnote = 0;
	p->notify = up->notify;
	p->notified = 0;
	p->notepending = 0;
	p->lastnote = nil;
	if((flag & RFNOTEG) == 0)
		p->noteid = up->noteid;
	p->procmode = up->procmode;
	p->privatemem = up->privatemem;
	p->noswap = up->noswap;
	p->hang = up->hang;
	p->kp = 0;
	kprocchild(p, (flag & RFMEM) ? helper : forkuser);
	kstrdup(&p->text, up->text);
	kstrdup(&p->user, up->user);
	kstrdup(&p->args, "");
	p->nargs = 0;
	p->setargs = 0;
	p->insyscall = 0;
	memset(p->time, 0, sizeof(p->time));
	p->time[TReal] = MACHP(0)->ticks;
	pid = pidalloc(p);
	qunlock(&p->debug);
	qunlock(&up->debug);

	/* the child ends on an error */
	if(waserror()){
		p->kp = 1;
		kprocchild(p, abortion);
		ready(p);
		nexterror();
	}
	if(flag & (RFFDG|RFCFDG))
		p->fgrp = dupfgrp((flag & RFFDG) ? up->fgrp : nil);
	else{
		p->fgrp = up->fgrp;
		incref(up->fgrp);
	}
	if(flag & (RFNAMEG|RFCNAMEG)){
		p->pgrp = newpgrp();
		pgrpcpy(p->pgrp, up->pgrp, flag);
	}else{
		p->pgrp = up->pgrp;
		incref(up->pgrp);
	}
	if(flag & RFREND)
		p->rgrp = newrgrp();
	else{
		p->rgrp = up->rgrp;
		incref(up->rgrp);
	}
	if(flag & (RFENVG|RFCENVG)){
		p->egrp = newegrp();
		if(flag & RFENVG)
			envcpy(p->egrp, up->egrp);
	}else{
		p->egrp = up->egrp;
		incref(up->egrp);
	}
	poperror();

	if(flag & RFNOMNT)
		devmask(p->pgrp, 1, nomntdevs);
	if((flag & RFNOWAIT) == 0){
		p->parent = up;
		lock(&up->exl);
		up->nchild++;
		unlock(&up->exl);
	}
	procpriority(p, up->basepri, up->fixedpri);
	if(flag & RFMEM){
		/*
		 * the memory's procs: their programs on its Worker, their calls
		 * on their helpers.  The parent joins the group in rfmemstart
		 */
		u = up->umem;
		if(u != nil)
			incref(u);
		else{
			u = mallocz(sizeof(Umem), 1);
			if(u == nil){
				p->kp = 1;
				kprocchild(p, abortion);
				ready(p);
				error(Enomem);
			}
			u->ref = 1;
		}
		p->umem = u;
		p->memdone = &u->done;
		platrfmem(p, rfmemstart, pid, u, &u->done);
	}else
		platfork(p, forkready, pid);
	return pid;
}

uintptr
sysrfork(va_list list)
{
	static char nomntdevs[] = "|decp";
	ulong flag;
	Fgrp *ofg;
	Pgrp *opg;
	Rgrp *org;
	Egrp *oeg;

	flag = va_arg(list, ulong);
	if((flag & (RFFDG|RFCFDG)) == (RFFDG|RFCFDG))
		error(Ebadarg);
	if((flag & (RFNAMEG|RFCNAMEG)) == (RFNAMEG|RFCNAMEG))
		error(Ebadarg);
	if((flag & (RFENVG|RFCENVG)) == (RFENVG|RFCENVG))
		error(Ebadarg);
	if(flag & RFPROC)
		return forkproc(flag);
	if(flag & (RFMEM|RFNOWAIT))
		error(Ebadarg);

	ofg = up->fgrp;
	opg = up->pgrp;
	org = up->rgrp;
	oeg = up->egrp;
	if(waserror()){
		if(up->fgrp != ofg){
			closefgrp(up->fgrp);
			up->fgrp = ofg;
		}
		if(up->pgrp != opg){
			closepgrp(up->pgrp);
			up->pgrp = opg;
		}
		if(up->rgrp != org){
			closergrp(up->rgrp);
			up->rgrp = org;
		}
		if(up->egrp != oeg){
			closeegrp(up->egrp);
			up->egrp = oeg;
		}
		nexterror();
	}
	if(flag & (RFFDG|RFCFDG))
		up->fgrp = dupfgrp((flag & RFFDG) ? ofg : nil);
	if(flag & (RFNAMEG|RFCNAMEG)){
		up->pgrp = newpgrp();
		pgrpcpy(up->pgrp, opg, flag);
	}
	if(flag & RFREND)
		up->rgrp = newrgrp();
	if(flag & (RFENVG|RFCENVG)){
		up->egrp = newegrp();
		if(flag & RFENVG)
			envcpy(up->egrp, oeg);
	}
	if(ofg != up->fgrp)
		closefgrp(ofg);
	if(opg != up->pgrp)
		closepgrp(opg);
	if(org != up->rgrp)
		closergrp(org);
	if(oeg != up->egrp)
		closeegrp(oeg);
	poperror();
	if(flag & RFNOMNT)
		devmask(up->pgrp, 1, nomntdevs);
	if(flag & RFNOTEG){
		qlock(&up->debug);
		setnoteid(up, 0);
		qunlock(&up->debug);
	}
	return 0;
}

uintptr
syssleep(va_list list)
{
	long ms;

	ms = va_arg(list, long);
	if(ms > 0)
		tsleep(&up->sleep, return0, 0, ms);
	return 0;
}

uintptr
sysalarm(va_list list)
{
	return procalarm(va_arg(list, ulong));
}

uintptr
sysawait(va_list list)
{
	char *p;
	Waitmsg w;
	uint n;

	p = va_arg(list, char*);
	n = va_arg(list, uint);
	pwait(&w);
	return (uintptr)snprint(p, n, "%d %lud %lud %lud %q",
		w.pid,
		w.time[TUser], w.time[TSys], w.time[TReal],
		w.msg);
}

void
werrstr(char *fmt, ...)
{
	va_list va;

	if(up == nil)
		return;

	va_start(va, fmt);
	vseprint(up->syserrstr, up->syserrstr+ERRMAX, fmt, va);
	va_end(va);
}

uintptr
syserrstr(va_list list)
{
	char *buf, *err;
	uint nbuf;

	buf = va_arg(list, char*);
	nbuf = va_arg(list, uint);
	if(nbuf == 0)
		error(Ebadarg);
	if(nbuf > ERRMAX)
		nbuf = ERRMAX;

	err = up->errstr;
	utfecpy(err, err+nbuf, buf);
	utfecpy(buf, buf+nbuf, up->syserrstr);

	up->errstr = up->syserrstr;
	up->syserrstr = err;
	return 0;
}

/* the program's memory: the platform's */
uintptr
sysbrk_(va_list list)
{
	if(platbrk(va_arg(list, ulong)) < 0)
		error(Enovmem);
	return 0;
}

/* the program's handler: a function in its table */
uintptr
sysnotify(va_list list)
{
	up->notify = va_arg(list, void*);
	return 0;
}

/*
 * noted, from the program's notify handler (platform.js calls it with
 * the note when a system call returns, trap.c): NCONT and NRSTR go back
 * to where the program was; NDFLT is the note's default, the end
 */
uintptr
sysnoted(va_list list)
{
	int arg;

	arg = va_arg(list, int);
	if(!up->notified)
		error(Egreg);
	switch(arg){
	case NCONT:
	case NRSTR:
		up->notified = 0;
		platnoted();
		return 0;
	case NSAVE:
		error("noted NSAVE: not on wasm32");
	}
	if(helperdie())
		return 0;	/* the memory's Worker: its helper ends it */
	if(up->lastnote->flag == NDebug)
		pprint("suicide: %s\n", up->lastnote->msg);
	pexit(up->lastnote->msg, up->lastnote->flag != NDebug);
}

uintptr
sysrendezvous(va_list list)
{
	uintptr tag, val, new;
	Proc *p, **l;

	tag = va_arg(list, uintptr);
	new = va_arg(list, uintptr);
	l = &REND(up->rgrp, tag);

	lock(up->rgrp);
	for(p = *l; p != nil; p = p->rendhash) {
		if(p->rendtag == tag) {
			*l = p->rendhash;
			val = p->rendval;
			p->rendval = new;
			unlock(up->rgrp);

			ready(p);

			return val;
		}
		l = &p->rendhash;
	}

	/* going to sleep here */
	up->rendtag = tag;
	up->rendval = new;
	up->rendhash = *l;
	*l = up;
	up->state = Rendezvous;
	unlock(up->rgrp);

	sched();

	return up->rendval;
}

/* 3l's preemption point (call 105): the browser preempts; notes are looked at */
uintptr
sysyield(va_list)
{
	return 0;
}

/* wasm32's seek and nsec give their vlong as the result */
vlong
sysseekv(va_list list)
{
	vlong v;
	ulong a[5];

	a[0] = (ulong)&v;
	memmove(&a[1], list, 4*BY2WD);	/* fd, the offset's two words, type */
	sysseek((va_list)a);
	return v;
}

vlong
sysnsecv(va_list)
{
	return todget(nil, nil);
}
