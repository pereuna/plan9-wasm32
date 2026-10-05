#include	<u.h>
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"
#include	"edf.h"
#include	<trace.h>
#include	"tos.h"
#include	"ureg.h"

/*
 * wasm32's procs (docs/architecture.md, phase C): 9front's proc.c
 * without its scheduler.  A Proc is a Worker of its own - its own
 * instance of the kernel, its own m and up, the kernel's memory shared.
 * The browser schedules the Workers; there is no run queue, no
 * priority, no preemption here:
 *
 *	sleep	the proc's Worker waits on p->state (Atomics.wait)
 *	ready	p->state = Ready and a notify; for a New proc, a Worker
 *	sched	the Worker waits until it is Ready again, or ends
 *
 * Each Worker's m is a Mach of its own (splhigh, the sched label, proc);
 * its machno is 0, as port/ indexes its per-CPU tables (timers,
 * intrcount) with it: to port/, the machine has one CPU, MACHP(0) -
 * mach0, the boot Worker's, which is the clock (clock.c): ticks,
 * timers, alarms and load are its.  conf.nmach is 1.  A lock's l->m is
 * MACHP(0) on every Worker: its owner is l->p (iprint's check sees any
 * holder as itself and prints without the lock).
 */

ulong	load;

static struct Procalloc
{
	Lock;
	Proc	**tab;
	Proc	*free;
	int	nextindex;
} procalloc;

char *statename[] =
{	/* BUG: generate automatically */
	"Dead",
	"Moribund",
	"New",
	"Ready",
	"Scheding",
	"Running",
	"Queueing",
	"QueueingR",
	"QueueingW",
	"Wakeme",
	"Broken",
	"Stopped",
	"Rendez",
	"Waitrelease",
};

static void pidinit(void);
static void pidfree(Proc*);

int
kenter(Ureg*)
{
	return 0;
}

void
kexit(Ureg*)
{
}

/* a proc's part in a memory's group (rfork RFMEM) given up */
void
umemrelease(Proc *p)
{
	Umem *u;

	if((u = p->umem) == nil)
		return;
	p->umem = nil;
	p->memdone = nil;
	if(decref(u) == 0)
		free(u);
}

/* a proc gone (pexit): free; the memory's Worker told, if it runs the proc's program */
static void
procdead(Proc *p)
{
	if(p->memdone != nil){
		p->hexit = 1;
		p->hdone = 1;
		coherence();
		ainc(p->memdone);
		platwake(p->memdone, 1);
	}
	umemrelease(p);
	mmurelease(p);
	lock(&procalloc);
	p->state = Dead;
	p->qnext = procalloc.free;
	procalloc.free = p;
	up = procalloc.Lock.p = nil;
	unlock(&procalloc);
}

/* a Worker less on p (the platform's, or the memory's Worker's: coend) */
void
procrelease(Proc *p)
{
	adec(&p->workers);
	platwake(&p->workers, 1);
}

typedef struct Helping Helping;
struct Helping
{
	Proc	*p;
	long	up;
};

/*
 * an rfork(RFMEM) proc's helper that is not its own Worker: the one
 * that ran its program before the memory had more than one proc
 * (helperspawn).  Its Mach and stack are the Proc's, kept with it; the
 * memory's Worker has the KSTACK.  It returns when the proc is Dead, or
 * when it is told to go (hquit: the rfork undone): a Worker less on p
 */
static ulong
helperstart(void *v)
{
	Helping *h;
	Proc *p;

	h = v;
	p = h->p;
	m = p->hmach;
	memset(m, 0, sizeof(Mach));
	m->helper = 1;
	up = p;
	m->proc = p;
	h->up = 1;
	platwake(&h->up, 1);
	if(setlabel(&m->sched)){
		procdead(p);
		return (ulong)&p->workers;
	}
	helper();
	return (ulong)&p->workers;
}

/* p's helper, a Worker more on p: -1 if the page could not make it */
int
helperspawn(Proc *p)
{
	Helping h;
	long st;

	if(p->hmach == nil)
		p->hmach = mallocz(sizeof(Mach), 1);
	if(p->hstack == nil)
		p->hstack = malloc(KSTACK);
	if(p->hmach == nil || p->hstack == nil)
		return -1;
	h.p = p;
	h.up = 0;
	ainc(&p->workers);
	platnewproc((void(*)(void*))helperstart, &h, p->hstack + KSTACK, &h.up, Whelper);
	while((st = h.up) == 0)
		platwait(&h.up, 0, -1);
	if(st < 0){
		procrelease(p);
		return -1;
	}
	return 0;
}

/* p's helper (helperspawn) goes, and is gone when this returns: p has only its own Worker again */
void
helperquit(Proc *p)
{
	long w;

	p->hquit = 1;
	coherence();
	p->hreq = 1;
	platwake(&p->hreq, 1);
	while((w = p->workers) > 1)
		platwait(&p->workers, w, -1);
}

/*
 * a proc's Worker starts here (platform.js, platnewproc): its Mach (m)
 * and up, then what kprocchild or forkchild put in p->sched.pc.  It
 * returns when the proc is Dead: &p->workers, which the platform takes
 * 1 from once the Worker has left the kernel - until it is 0 the Proc,
 * free, and its KSTACK are not newproc's to give
 */
static ulong
procstart(void *v)
{
	Proc *p;
	void (*f)(void);

	p = v;
	m = p->mach;
	up = p;
	m->proc = p;
	p->state = Running;
	p->workerup = 1;
	platwake(&p->workerup, 1);
	if(setlabel(&m->sched)){
		/* Moribund (pexit): the proc is free, its Worker ends */
		procdead(p);
		return (ulong)&p->workers;
	}
	f = (void(*)(void))p->sched.pc;
	(*f)();
	pexit("procstart", 0);
	return 0;
}

/*
 * up gives up its Worker: Moribund ends it (back to procstart),
 * Running goes on (the browser shares its CPUs), anything else
 * waits until ready makes it Ready
 */
void
sched(void)
{
	int st;

	if(up == nil)
		panic("sched: no up on a wasm32 Worker");
	m->cs++;
	if(up->state == Moribund)
		gotolabel(&m->sched);
	if(up->state == Running)
		return;
	for(;;){
		st = up->state;
		if(st == Ready)
			break;
		platwait((long*)&up->state, st, -1);
	}
	up->state = Running;
}

/*
 * a New proc's Worker, its stack the KSTACK below it (newproc): -1 if
 * the page could not make one (the proc is still New)
 */
int
procspawn(Proc *p, int what)
{
	long st;

	if(p->mach == nil){
		p->mach = mallocz(sizeof(Mach), 1);
		if(p->mach == nil)
			panic("procspawn: no Mach");
	}
	p->mach->machno = 0;
	p->state = Ready;
	p->workers = 1;
	p->workerup = 0;
	coherence();
	platnewproc((void(*)(void*))procstart, p, p, &p->workerup, what);
	while((st = p->workerup) == 0)
		platwait(&p->workerup, 0, -1);
	if(st < 0){
		p->state = New;
		p->workers = 0;
		return -1;
	}
	return 0;
}

/* a proc that never ran (procspawn failed): undone, its parent told */
void
procunmake(Proc *p)
{
	Proc *pp;

	if((pp = p->parent) != nil){
		lock(&pp->exl);
		pp->nchild--;
		unlock(&pp->exl);
		p->parent = nil;
	}
	if(p->fgrp != nil){
		closefgrp(p->fgrp);
		p->fgrp = nil;
	}
	if(p->pgrp != nil){
		closepgrp(p->pgrp);
		p->pgrp = nil;
	}
	if(p->rgrp != nil){
		closergrp(p->rgrp);
		p->rgrp = nil;
	}
	if(p->egrp != nil){
		closeegrp(p->egrp);
		p->egrp = nil;
	}
	if(p->dot != nil){
		cclose(p->dot);
		p->dot = nil;
	}
	umemrelease(p);
	qlock(&p->debug);	/* as pexit: devproc looks at the pid under it */
	pidfree(p);
	qunlock(&p->debug);
	lock(&procalloc);
	p->state = Dead;
	p->qnext = procalloc.free;
	procalloc.free = p;
	unlock(&procalloc);
}

void
ready(Proc *p)
{
	if(p->state == New){
		if(procspawn(p, Wproc) < 0)
			panic("ready: no Worker for %s", p->text);
		return;
	}
	p->state = Ready;
	coherence();
	platwake((long*)&p->state, 1);
}

/*
 * port/'s hooks into a scheduler wasm32 has not: the clock's
 * (portclock.c) and a lock's spinning (taslock.c)
 */
void
hzsched(void)
{
}

void
yield(void)
{
}

Proc*
newproc(void)
{
	char *b;
	Proc *p, **l;
	long w;
	int held;

	for(;;){
		lock(&procalloc);
		/*
		 * a free Proc no Worker is on any more: one may still be held
		 * (its memory's Worker goes on with its other procs, or its own
		 * is on the way out: workers) - not that one
		 */
		for(l = &procalloc.free; (p = *l) != nil; l = &p->qnext)
			if(p->workers == 0)
				break;
		if(p != nil){
			*l = p->qnext;
			break;
		}
		if(procalloc.nextindex < conf.nproc){
			b = malloc(KSTACK+sizeof(Proc));
			if(b == nil){
				unlock(&procalloc);
				return nil;
			}
			p = (Proc*)(b + KSTACK);
			p->workers = 0;
			p->state = Dead;
			p->index = procalloc.nextindex++;
			procalloc.tab[p->index] = p;
			break;
		}
		held = procalloc.free != nil;
		unlock(&procalloc);
		if(!held)
			return nil;
		/* all of them free but held yet: a while */
		w = 0;
		platwait(&w, 0, 10);
	}
	assert(p->state == Dead);
	p->qnext = nil;
	unlock(&procalloc);

	p->umem = nil;
	p->memdone = nil;
	p->hcall = nil;
	p->hquit = 0;
	p->hreq = 0;
	p->hdone = 0;
	p->hexit = 0;
	p->hdie = 0;

	p->psstate = nil;
	p->state = New;
	p->fpstate = FPinit;
#ifdef KFPSTATE
	p->kfpstate = FPinit;
#endif
	p->procctl = 0;
	p->noteureg = nil;
	p->dbgreg = nil;
	p->nerrlab = 0;
	p->errstr = p->errbuf0;
	p->syserrstr = p->errbuf1;
	p->errbuf0[0] = '\0';
	p->errbuf1[0] = '\0';
	p->lastlock = nil;
	p->lastilock = nil;
	p->nlocks = 0;
	p->trace = 0;
	p->delaysched = 0;

	/* sched params: the browser's; kept for devproc */
	p->wired = 0;
	p->affinity = -1;
	procpriority(p, PriNormal, 0);
	p->cpu = 0;
	p->edf = nil;

	return p;
}

/*
 * wire this proc to a machine: to port/, wasm32 has one (MACHP(0))
 */
void
procwired(Proc *p, int)
{
	p->affinity = 0;
	p->wired = 1;
}

void
procpriority(Proc *p, int pri, int fixed)
{
	if(pri >= Npriq)
		pri = Npriq - 1;
	else if(pri < 0)
		pri = 0;
	p->basepri = pri;
	p->priority = pri;
	p->fixedpri = fixed != 0;
}

void
procinit0(void)		/* bad planning - clashes with devproc.c */
{
	procalloc.free = nil;
	/* allocate 1 extra for a nil terminator */
	procalloc.tab = xalloc((conf.nproc+1)*sizeof(Proc*));
	if(procalloc.tab == nil){
		xsummary();
		panic("cannot allocate proctab for %lud procs", conf.nproc);
	}
	pidinit();
}

/*
 *  sleep if a condition is not true.  Another process will
 *  awaken us after it sets the condition.  When we awaken
 *  the condition may no longer be true.
 *
 *  we lock both the process and the rendezvous to keep r->p
 *  and p->r synchronized.
 */
void
sleep(Rendez *r, int (*f)(void*), void *arg)
{
	int s;
	void (*pt)(Proc*, int, vlong);

	if(up->nlocks)
		print("process %s %lud sleeps with %d locks held, "
			"last lock %#p locked at pc %#p, sleep called from %#p\n",
			up->text, up->pid, up->nlocks,
			up->lastlock, up->lastlock->pc, getcallerpc(&r));

	s = splhi();
	lock(r);
	lock(&up->rlock);
	if(r->p != nil){
		iprint("double sleep called from %#p, %s %lud -> %s %lud\n",
			getcallerpc(&r), r->p->text, r->p->pid, up->text, up->pid);
		dumpstack();
	}

	/*
	 *  Wakeup only knows there may be something to do by testing
	 *  r->p in order to get something to lock on.
	 *  Flush that information out to memory in case the sleep is
	 *  committed.
	 */
	r->p = up;

	if((*f)(arg) || up->notepending){
		/*
		 *  if condition happened or a note is pending
		 *  never mind
		 */
		r->p = nil;
		unlock(&up->rlock);
		unlock(r);
	} else {
		/*
		 *  now we are committed to
		 *  change state and call scheduler
		 */
		pt = proctrace;
		if(pt != nil)
			pt(up, SSleep, 0);
		up->state = Wakeme;
		up->r = r;
		unlock(&up->rlock);
		unlock(r);
		sched();
	}

	if(up->notepending) {
		up->notepending = 0;
		splx(s);
		interrupted();
	}

	splx(s);
}

_Noreturn void
interrupted(void)
{
	if(up->procctl == Proc_exitme && up->closingfgrp != nil)
		forceclosefgrp();
	error(Eintr);
}

void
twakeup(Ureg*, Timer *t)
{
	Proc *p;
	Rendez *trend;

	p = t->ta;
	trend = p->trend;
	if(trend != nil){
		p->trend = nil;
		wakeup(trend);
	}
}

static int
tfn(void *arg)
{
	return up->trend == nil || up->tfn(arg);
}

void
tsleep(Rendez *r, int (*fn)(void*), void *arg, ulong ms)
{
	if(up->tt != nil){
		print("%s %lud: tsleep timer active: mode %d, tf %#p, pc %#p\n",
			up->text, up->pid, up->tmode, up->tf, getcallerpc(&r));
		timerdel(up);
	}
	up->tns = MS2NS(ms);
	up->tmode = Trelative;
	up->tf = twakeup;
	up->ta = up;
	up->trend = r;
	up->tfn = fn;
	timeradd(up);

	if(waserror()){
		up->trend = nil;
		timerdel(up);
		nexterror();
	}
	sleep(r, tfn, arg);
	up->trend = nil;
	timerdel(up);
	poperror();
}

/*
 *  Expects that only one process can call wakeup for any given Rendez.
 *  We hold both locks to ensure that r->p and p->r remain consistent.
 *  Richard Miller has a better solution that doesn't require both to
 *  be held simultaneously, but I'm a paranoid - presotto.
 */
Proc*
wakeup(Rendez *r)
{
	Proc *p;
	int s;

	s = splhi();

	lock(r);
	p = r->p;
	if(p == nil)
		unlock(r);
	else {
		lock(&p->rlock);
		if(p->state != Wakeme || p->r != r){
			iprint("%p %p %d\n", p->r, r, p->state);
			panic("wakeup: state");
		}
		r->p = nil;
		p->r = nil;
		unlock(&p->rlock);
		unlock(r);
		/* hands off r */
		ready(p);
	}
	splx(s);

	return p;
}

/*
 *  if waking a sleeping process, this routine must hold both
 *  p->rlock and r->lock.  However, it can't know them in
 *  the same order as wakeup causing a possible lock ordering
 *  deadlock.  We break the deadlock by giving up the p->rlock
 *  lock if we can't get the r->lock and retrying.
 */
void
procinterrupt(Proc *p)
{
	QLock *q;
	int s;

	p->notepending = 1;

	/* this loop is to avoid lock ordering problems. */
	for(;;){
		Rendez *r;

		s = splhi();
		lock(&p->rlock);
		r = p->r;

		/* waiting for a wakeup? */
		if(r == nil)
			break;	/* no */

		/* try for the second lock */
		if(canlock(r)){
			if(p->state != Wakeme || r->p != p)
				panic("procinterrupt: state %d %d %d",
					r->p != p, p->r != r, p->state);
			p->r = nil;
			r->p = nil;
			unlock(&p->rlock);
			unlock(r);
			/* hands off r */
			ready(p);
			splx(s);
			return;
		}

		/* give other process time to get out of critical section and try again */
		unlock(&p->rlock);
		splx(s);

		sched();
	}
	unlock(&p->rlock);
	splx(s);

	switch(p->state){
	case Queueing:
		/* Try and pull out of a eqlock */
		if((q = p->eql) != nil){
			lock(&q->use);
			if(p->state == Queueing && p->eql == q){
				Proc *d, *l;

				for(l = nil, d = q->head; d != nil; l = d, d = d->qnext){
					if(d == p){
						if(l != nil)
							l->qnext = p->qnext;
						else
							q->head = p->qnext;
						if(p->qnext == nil)
							q->tail = l;
						p->qnext = nil;
						p->eql = nil;	/* not taken */
						unlock(&q->use);
						/* hands off q */
						ready(p);
						return;
					}
				}
			}
			unlock(&q->use);
		}
		break;
	case Rendezvous:
		/* Try and pull out of a rendezvous */
		lock(p->rgrp);
		if(p->state == Rendezvous) {
			Proc *d, **l;

			l = &REND(p->rgrp, p->rendtag);
			for(d = *l; d != nil; d = d->rendhash) {
				if(d == p) {
					*l = p->rendhash;
					p->rendval = ~0;
					unlock(p->rgrp);
					/* hands off p->rgrp */
					ready(p);
					return;
				}
				l = &d->rendhash;
			}
		}
		unlock(p->rgrp);
		break;
	}
}

/*
 *  pop a note from the calling process or suicide if theres
 *  no note handler or notify during note handling. 
 *  Called from notify() with up->debug lock held.
 */
char*
popnote(Ureg *u)
{
	up->notepending = 0;
	if(up->nnote == 0)
		return nil;
	assert(up->nnote > 0);
	assert(up->note[0] != nil);
	assert(up->note[0]->ref > 0);

	/* hold off user notes during note handling */
	if(up->notified && up->note[0]->flag == NUser)
		return nil;

	freenote(up->lastnote);
	up->lastnote = up->note[0];
	if(--up->nnote > 0)
		memmove(&up->note[0], &up->note[1], up->nnote*sizeof(Note*));
	up->note[up->nnote] = nil;

	if(u != nil && up->lastnote->ref == 1 && strncmp(up->lastnote->msg, "sys:", 4) == 0){
		int l = strlen(up->lastnote->msg);
		assert(l < ERRMAX);
		assert(userureg(u));
		snprint(up->lastnote->msg+l, ERRMAX-l, " pc=%#p", u->pc);
	}

	if(up->notify == nil || up->notified){
		qunlock(&up->debug);
		if(up->lastnote->flag == NDebug)
			pprint("suicide: %s\n", up->lastnote->msg);
		pexit(up->lastnote->msg, up->lastnote->flag!=NDebug);
	}
	up->notified = 1;

	return up->lastnote->msg;
}

static Note*
mknote(char *msg, int flag)
{
	Note *n;

	n = smalloc(sizeof(Note));
	kstrcpy(n->msg, msg, ERRMAX);
	n->flag = flag;
	n->ref = 1;
	return n;
}

int
pushnote(Proc *p, Note *n)
{
	if(p->state <= New || p->state == Broken || p->pid == 0){
		freenote(n);
		return 0;
	}
	assert(n->ref > 0);
	if(n->flag != NUser && (p->notify == nil || p->notified))
		freenotes(p);
	if(p->nnote < NNOTE){
		p->note[p->nnote++] = n;
		procinterrupt(p);
		return 1;
	}
	freenote(n);
	return 0;
}

int
postnote(Proc *p, int dolock, char *msg, int flag)
{
	Note *n;
	int ret;

	if(p == nil)
		return 0;

	n = mknote(msg, flag);
	if(dolock)
		qlock(&p->debug);
	ret = pushnote(p, n);
	if(dolock)
		qunlock(&p->debug);

	return ret;
}

void
postnotepg(ulong noteid, char *msg, int flag)
{
	Note *n;
	Proc *p;
	int i;

	n = mknote(msg, flag);
	for(i = 0; (p = proctab(i)) != nil; i++){
		if(p == up || p->noteid != noteid || p->kp)
			continue;
		qlock(&p->debug);
		if(p->noteid == noteid && !p->kp){
			incref(n);
			pushnote(p, n);
		}
		qunlock(&p->debug);
	}
	freenote(n);
}

/* keep some broken processes around */
static struct {
	Lock;
	int	n;
	Proc	*p[4];
} broken;

static void
addbroken(void)
{
	lock(&broken);
	if(broken.n == nelem(broken.p)) {
		ready(broken.p[0]);
		memmove(&broken.p[0], &broken.p[1], sizeof(Proc*)*(--broken.n));
	}
	broken.p[broken.n++] = up;
	up->state = Broken;
	up->psstate = nil;
	unlock(&broken);
	sched();
}

void
unbreak(Proc *p)
{
	int i;

	lock(&broken);
	for(i=0; i < broken.n; i++){
		if(broken.p[i] == p) {
			memmove(&broken.p[i], &broken.p[i+1], sizeof(Proc*)*(broken.n-(i+1)));
			broken.p[--broken.n] = nil;
			ready(p);
			break;
		}
	}
	unlock(&broken);
}

int
freebroken(void)
{
	int i, n;

	lock(&broken);
	n = broken.n;
	broken.n = 0;
	for(i=0; i<n; i++){
		Proc *p = broken.p[i];
		broken.p[i] = nil;
		ready(p);
	}
	unlock(&broken);
	return n;
}

void
freenote(Note *n)
{
	if(n == nil || decref(n))
		return;
	free(n);
}

void
freenotes(Proc *p)
{
	while(p->nnote > 0){
		freenote(p->note[--p->nnote]);
		p->note[p->nnote] = nil;
	}
}

_Noreturn void
pexit(char *exitstr, int freemem)
{
	Proc *p;
	ulong utime, stime;
	Waitq *wq;
	Fgrp *fgrp;
	Egrp *egrp;
	Rgrp *rgrp;
	Pgrp *pgrp;
	Chan *dot;
	void (*pt)(Proc*, int, vlong);
	Segment *s;
	int i;

	up->alarm = 0;
	timerdel(up);
	pt = proctrace;
	if(pt != nil)
		pt(up, SDead, 0);

	/* nil out all the resources under lock (free later) */
	qlock(&up->debug);
	fgrp = up->fgrp;
	up->fgrp = nil;
	egrp = up->egrp;
	up->egrp = nil;
	rgrp = up->rgrp;
	up->rgrp = nil;
	pgrp = up->pgrp;
	up->pgrp = nil;
	dot = up->dot;
	up->dot = nil;
	qunlock(&up->debug);

	if(fgrp != nil)
		closefgrp(fgrp);
	if(egrp != nil)
		closeegrp(egrp);
	if(rgrp != nil)
		closergrp(rgrp);
	if(dot != nil)
		cclose(dot);
	if(pgrp != nil)
		closepgrp(pgrp);

	if(up->parentpid == 0){
		if(exitstr == nil)
			exitstr = "unknown";
		panic("boot process died: %s", exitstr);
	}

	p = up->parent;
	if(p != nil && p->pid == up->parentpid && p->state != Broken){
		wq = smalloc(sizeof(Waitq));
		wq->w.pid = up->pid;
		utime = up->time[TUser] + up->time[TCUser];
		stime = up->time[TSys] + up->time[TCSys];
		wq->w.time[TUser] = tk2ms(utime);
		wq->w.time[TSys] = tk2ms(stime);
		wq->w.time[TReal] = tk2ms(MACHP(0)->ticks - up->time[TReal]);
		if(exitstr != nil && exitstr[0])
			snprint(wq->w.msg, sizeof(wq->w.msg), "%s %lud: %s", up->text, up->pid, exitstr);
		else
			wq->w.msg[0] = '\0';

		lock(&p->exl);
		/*
		 * Check that parent is still alive.
		 */
		if(p->pid == up->parentpid && p->state != Broken) {
			p->nchild--;
			p->time[TCUser] += utime;
			p->time[TCSys] += stime;
			/*
			 * If there would be more than 128 wait records
			 * processes for my parent, then don't leave a wait
			 * record behind.  This helps prevent badly written
			 * daemon processes from accumulating lots of wait
			 * records.
		 	 */
			if(p->nwait < 128) {
				wq->next = p->waitq;
				p->waitq = wq;
				p->nwait++;
				wq = nil;
				wakeup(&p->waitr);
			}
		}
		unlock(&p->exl);
		if(wq != nil)
			free(wq);
	}

	if(!freemem){
		addbroken();
	}

	qlock(&up->seglock);
	for(i = 0; i < NSEG; i++){
		s = up->seg[i];
		if(s != nil){
			up->seg[i] = nil;
			putseg(s);
		}
	}
	qunlock(&up->seglock);

	qlock(&up->debug);

	lock(&up->exl);		/* Prevent my children from leaving waits */
	pidfree(up);
	up->parent = nil;
	up->nchild = up->nwait = 0;
	wakeup(&up->waitr);
	unlock(&up->exl);

	while((wq = up->waitq) != nil){
		up->waitq = wq->next;
		free(wq);
	}

	freenotes(up);
	freenote(up->lastnote);
	up->lastnote = nil;
	up->notified = 0;
	up->noteureg = nil;
	up->dbgreg = nil;

	/* release debuggers */
	if(up->pdbg != nil) {
		wakeup(&up->pdbg->sleep);
		up->pdbg = nil;
	}
	if(up->syscalltrace != nil) {
		free(up->syscalltrace);
		up->syscalltrace = nil;
	}
	if(up->watchpt != nil){
		free(up->watchpt);
		up->watchpt = nil;
	}
	up->nwatchpt = 0;
	qunlock(&up->debug);

	callabort(up);	/* the call that ended it does not return: its buffers */
	up->state = Moribund;
	sched();
	panic("pexit");
}

static int
haswaitq(void *x)
{
	return ((Proc*)x)->waitq != nil;
}

ulong
pwait(Waitmsg *w)
{
	ulong cpid;
	Waitq *wq;

	if(!canqlock(&up->qwaitr))
		error(Einuse);

	if(waserror()) {
		qunlock(&up->qwaitr);
		nexterror();
	}

	lock(&up->exl);
	while(up->waitq == nil) {
		if(up->nchild == 0) {
			unlock(&up->exl);
			error(Enochild);
		}
		unlock(&up->exl);
		sleep(&up->waitr, haswaitq, up);
		lock(&up->exl);
	}
	wq = up->waitq;
	up->waitq = wq->next;
	up->nwait--;
	unlock(&up->exl);

	qunlock(&up->qwaitr);
	poperror();

	if(w != nil)
		memmove(w, &wq->w, sizeof(Waitmsg));
	cpid = wq->w.pid;
	free(wq);
	return cpid;
}

Proc*
proctab(int i)
{
#define proctab(x) (procalloc.tab[(x)])
	return proctab(i);
}

void
dumpaproc(Proc *p)
{
	ulong bss;
	char *s;

	if(p == nil)
		return;

	bss = 0;
	if(p->seg[BSEG] != nil)
		bss = p->seg[BSEG]->top;

	s = p->psstate;
	if(s == nil)
		s = statename[p->state];
	print("%3lud:%10s pc %#p dbgpc %#p  %8s (%s) ut %ld st %ld bss %lux qpc %#p nl %d nd %lud lpc %#p pri %lud\n",
		p->pid, p->text, p->pc, dbgpc(p),  s, statename[p->state],
		p->time[0], p->time[1], bss, p->qpc, p->nlocks, p->delaysched,
		p->lastlock ? p->lastlock->pc : 0, p->priority);
}

/*
 *  wait till all matching processes have flushed their mmu
 */
static void
procflushmmu(int (*match)(Proc*, void*), void *a)
{
	Proc *await[MAXMACH];
	int i, nm, nwait;
	Proc *p;

	/*
	 *  tell all matching processes to flush their mmu's
	 */
	memset(await, 0, conf.nmach*sizeof(await[0]));
	nwait = 0;
	for(i = 0; (p = proctab(i)) != nil; i++){
		if(p->state > New && (*match)(p, a)){
			p->newtlb = 1;
			for(nm = 0; nm < conf.nmach; nm++){
				if(MACHP(nm)->proc == p){
					coherence();
					MACHP(nm)->flushmmu = 1;
					if(await[nm] == nil)
						nwait++;
					await[nm] = p;
				}
			}
		}
	}

	/*
	 *  wait for all other processors to take a clock interrupt
	 *  and flush their mmu's
	 */
	for(;;){
		if(nwait == 0 || nwait == 1 && await[m->machno] != nil)
			break;

		sched();

		for(nm = 0; nm < conf.nmach; nm++){
			p = await[nm];
			if(p != nil && (MACHP(nm)->proc != p || MACHP(nm)->flushmmu == 0)){
				await[nm] = nil;
				nwait--;
			}
		}
	}
}

static int
matchseg(Proc *p, void *a)
{
	int ns;

	for(ns = 0; ns < NSEG; ns++){
		if(p->seg[ns] == a)
			return 1;
	}
	return 0;
}
void
procflushseg(Segment *s)
{
	procflushmmu(matchseg, s);
}

static int
matchpseg(Proc *p, void *a)
{
	Segment *s;
	int ns;

	for(ns = 0; ns < NSEG; ns++){
		s = p->seg[ns];
		if(s != nil && s->pseg == a)
			return 1;
	}
	return 0;
}
void
procflushpseg(Physseg *ps)
{
	procflushmmu(matchpseg, ps);
}

static int
matchother(Proc *p, void *a)
{
	return p != a;
}
void
procflushothers(void)
{
	procflushmmu(matchother, up);
}

static void
linkproc(void)
{
	spllo();
	(*up->kpfun)(up->kparg);
	pexit("kproc exiting", 0);
}

void
kproc(char *name, void (*func)(void *), void *arg)
{
	static Pgrp *kpgrp;
	Proc *p;

	while((p = newproc()) == nil){
		freebroken();
		resrcwait("no procs for kproc");
	}

	qlock(&p->debug);
	if(up != nil){
		p->slash = up->slash;
		p->dot = up->slash;	/* unlike fork, do not inherit the dot for kprocs */
		if(p->dot != nil)
			incref(p->dot);
	} else {
		p->slash = nil;
		p->dot = nil;
	}

	p->nnote = 0;
 	p->notify = nil;
	p->notified = 0;
	p->notepending = 0;
	p->lastnote = nil;

	p->procmode = 0640;
	p->privatemem = 1;
	p->noswap = 1;
	p->hang = 0;
	p->kp = 1;

	p->kpfun = func;
	p->kparg = arg;
	kprocchild(p, linkproc);

	kstrdup(&p->text, name);
	kstrdup(&p->user, eve);
	kstrdup(&p->args, "");
	p->nargs = 0;
	p->setargs = 0;

	if(kpgrp == nil)
		kpgrp = newpgrp();
	p->pgrp = kpgrp;
	incref(kpgrp);

	p->insyscall = 1;
	memset(p->time, 0, sizeof(p->time));
	p->time[TReal] = MACHP(0)->ticks;
	cycles(&p->kentry);
	p->pcycles = -p->kentry;

	pidalloc(p);

	qunlock(&p->debug);

	procpriority(p, PriKproc, 0);

	ready(p);
}

/*
 *  called splhi() by notify().  See comment in notify for the
 *  reasoning.
 */
void
procctl(void)
{
	char *state;

	switch(up->procctl) {
	case Proc_exitbig:
		spllo();
		print("%s %lud: Killed: Insufficient physical memory\n", up->text, up->pid);
		pexit("Killed: Insufficient physical memory", 1);

	case Proc_exitme:
		spllo();		/* pexit has locks in it */
		pexit("Killed", 1);

	case Proc_traceme:
		if(up->nnote == 0)
			return;
		/* No break */

	case Proc_stopme:
		up->procctl = 0;
		state = up->psstate;
		up->psstate = statename[Stopped];
		/* free a waiting debugger */
		spllo();
		qlock(&up->debug);
		if(up->pdbg != nil) {
			wakeup(&up->pdbg->sleep);
			up->pdbg = nil;
		}
		qunlock(&up->debug);
		splhi();
		up->state = Stopped;
		sched();
		up->psstate = state;
		return;
	}
}

#include "errstr.h"

_Noreturn void
error(char *err)
{
	spllo();

	assert(up->nerrlab < NERR);
	kstrcpy(up->errstr, err, ERRMAX);
	setlabel(&up->errlab[NERR-1]);
	nexterror();
}

_Noreturn void
nexterror(void)
{
	assert(up->nerrlab > 0);
	gotolabel(&up->errlab[--up->nerrlab]);
}

void
exhausted(char *resource)
{
	char buf[ERRMAX];

	snprint(buf, sizeof buf, "no free %s", resource);
	iprint("%s\n", buf);
	error(buf);
}

ulong
procpagecount(Proc *p)
{
	Segment *s;
	ulong pages;
	int i;

	pages = 0;
	for(i=0; i<NSEG; i++){
		if((s = p->seg[i]) != nil)
			pages += s->used;
	}
	return pages;
}

/*
 *  called with p->debug locked.
 */
void
killproc(Proc *p, int ctl)
{
	static Note killnote = {
		"sys: killed",
		NExit,
		1,
	};

	if(p->state <= New || p->pid == 0 || p->kp)
		return;
	if(p->state == Broken){
		unbreak(p);
		return;
	}
	if(ctl != 0)
		p->procctl = ctl;
	incref(&killnote);
	pushnote(p, &killnote);
	if(p->state == Stopped)
		ready(p);
}

/*
 *  change ownership to 'new' of all processes owned by 'old'.  Used when
 *  eve changes.
 */
void
renameuser(char *old, char *new)
{
	Proc *p;
	int i;

	for(i = 0; (p = proctab(i)) != nil; i++){
		qlock(&p->debug);
		if(p->user != nil && strcmp(old, p->user) == 0)
			kstrdup(&p->user, new);
		qunlock(&p->debug);
	}
}

void
procsetuser(char *new)
{
	qlock(&up->debug);
	kstrdup(&up->user, new);
	qunlock(&up->debug);
	procpriority(up, PriNormal, 0);
}

/*
 *  time accounting called by hzclock: wasm32's clock is a Worker of
 *  its own and sees no proc running - neither a proc's time nor the
 *  load is kept
 */
void
accounttime(void)
{
}

/*
 *  A Pid structure is a reference counted hashtable entry
 *  with "pid" being the key and "procindex" being the value.
 *  A entry is allocated atomically by changing the key from
 *  negative or zero to the positive process id number.
 *  Pid's outlive ther Proc's as long as other processes hold
 *  a reference to them such as noteid or parentpid.
 *  This prevents pid reuse when the pid generator wraps.
 */
typedef struct Pid Pid;
struct Pid
{
	Ref;
	long	pid;
	int	procindex;
};

enum {
	PIDMASK = 0x7FFFFFFF,
	PIDSHIFT = 4,	/* log2 bucket size of the hash table */
};

static Pid *pidhashtab;
static ulong pidhashmask;

static void
pidinit(void)
{
	/*
	 * allocate 3 times conf.nproc Pid structures for the hash table
	 * and round up to a power of two as each process can reference
	 * up to 3 unique Pid structures:
	 *	- pid
	 *	- noteid
	 *	- parentpid
	 */
	pidhashmask = 1<<PIDSHIFT;
	while(pidhashmask < conf.nproc*3)
		pidhashmask <<= 1;

	pidhashtab = xalloc(pidhashmask * sizeof(pidhashtab[0]));
	if(pidhashtab == nil){
		xsummary();
		panic("cannot allocate pid hashtable of size %lud", pidhashmask);
	}

	/* make it a mask */
	pidhashmask--;
}

static Pid*
pidlookup(long pid)
{
	Pid *i, *e;
	long o;

	i = &pidhashtab[(pid<<PIDSHIFT) & pidhashmask];
	for(e = &i[1<<PIDSHIFT]; i < e; i++){
		o = i->pid;
		if(o == pid)
			return i;
		if(o == 0)
			break;
	}
	return nil;
}

/*
 *  increment the reference count of a pid (pid>0)
 *  or allocate a new one (pid<=0)
 */
static Pid*
pidadd(long pid)
{
	Pid *i, *e;
	long o;

	if(pid > 0){
		i = pidlookup(pid);
		if(i != nil)
			incref(i);
		return i;
	}
Again:
	do {
		static Ref gen;
		pid = incref(&gen) & PIDMASK;
	} while(pid == 0 || pidlookup(pid) != nil);

	i = &pidhashtab[(pid<<PIDSHIFT) & pidhashmask];
	for(e = &i[1<<PIDSHIFT]; i < e; i++){
		while((o = i->pid) <= 0){
			if(cmpswap(&i->pid, o, pid)){
				incref(i);
				return i;
			}
		}
	}
	/* bucket full, try a different pid */
	goto Again;
}

/*
 *  decrement reference count of a pid and free it
 *  when no references are remaining
 */
static void
piddel(Pid *i)
{
	if(decref(i))
		return;
	i->pid = -1;	/* freed */
}

int
procindex(ulong pid)
{
	Proc *p;
	Pid *i;
	int x;

	i = pidlookup(pid);
	if(i != nil){
		x = i->procindex;
		p = proctab(x);
		if(p != nil && p->pid == pid)
			return x;
	}
	return -1;
}

/*
 *  for the following functions:
 *    setnoteid(), pidalloc() and pidfree()
 *
 *  They need to be called with p->debug held
 *  to prevent devproc's changenoteid() from
 *  changing the noteid under us and make the
 *  update atomic.
 */

/*
 *  change the noteid of a process
 */
ulong
setnoteid(Proc *p, ulong noteid)
{
	Pid *i, *o;

	/*
	 * avoid allocating a new pid when we are the only
	 * user of the noteid
	 */
	o = pidlookup(p->noteid);
	if(noteid == 0 && o->ref == 1)
		return o->pid;

	i = pidadd(noteid);
	if(i == nil)
		error(Ebadarg);
	piddel(o);
	return p->noteid = i->pid;
}

/*
 *  allocate pid, noteid and parentpid to a process
 */
ulong
pidalloc(Proc *p)
{
	Pid *i;

	/* skip for the boot process */
	if(up != nil){
		i = pidadd(up->pid);
		p->parentpid = i->pid;
	} else
		p->parentpid = 0;

	i = pidadd(0);
	i->procindex = p->index;

	if(p->noteid == 0){
		incref(i);
		p->noteid = i->pid;
	} else
		pidadd(p->noteid);

	return p->pid = i->pid;
}

/*
 *  release pid, noteid and parentpid from a process
 */
static void
pidfree(Proc *p)
{
	Pid *i;

	i = pidlookup(p->pid);
	piddel(i);

	if(p->noteid != p->pid)
		i = pidlookup(p->noteid);
	piddel(i);

	if(p->parentpid != 0)
		piddel(pidlookup(p->parentpid));

	p->pid = p->noteid = p->parentpid = 0;
}
