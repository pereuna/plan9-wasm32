#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

/*
 * #p: wasm32's procs, what it has of them - no segments, registers or
 * text to debug.  /proc/n/ctl (kill, private, noswap), note, notepg
 * (kbdfs's interrupt), status, args (written: what ps shows, as 9front's).  One user: no permissions to check yet.
 */
enum
{
	Qdir,
	Qctl,
	Qnote,
	Qnotepg,
	Qstatus,
	Qargs,
};

#define	QSHIFT	5
#define	QID(q)		((ulong)(q).path & ((1<<QSHIFT)-1))
#define	SLOT(q)		((((ulong)(q).path & 0x7FFFFFFF) >> QSHIFT) - 1)
#define	PID(q)		((q).vers)

static Dirtab procdir[] =
{
	"ctl",		{Qctl},		0,	0222,
	"note",		{Qnote},	0,	0222,
	"notepg",	{Qnotepg},	0,	0222,
	"status",	{Qstatus},	0,	0444,
	"args",		{Qargs},	0,	0660,
};

static int
procgen(Chan *c, char *name, Dirtab*, int, int s, Dir *dp)
{
	Qid q;
	Proc *p;
	ulong pid;
	int slot;
	Dirtab *t;

	if(s == DEVDOTDOT){
		mkqid(&q, Qdir, 0, QTDIR);
		devdir(c, q, "#p", 0, eve, DMDIR|0555, dp);
		return 1;
	}
	if(c->qid.path == Qdir){
		if(name != nil){
			/* a pid: its slot */
			pid = strtoul(name, &name, 0);
			if(*name != 0 || (slot = procindex(pid)) < 0)
				return -1;
			s = slot;
		}
		if((p = proctab(s)) == nil)
			return -1;
		if((pid = p->pid) == 0)
			return 0;
		/* devdir keeps the pointer: the name in up->genbuf, as 9front's */
		snprint(up->genbuf, sizeof up->genbuf, "%lud", pid);
		mkqid(&q, (s+1)<<QSHIFT, pid, QTDIR);
		devdir(c, q, up->genbuf, 0, p->user, DMDIR|0555, dp);
		return 1;
	}
	if(s >= nelem(procdir))
		return -1;
	t = &procdir[s];
	slot = SLOT(c->qid);
	if((p = proctab(slot)) == nil || p->pid != PID(c->qid))
		return -1;
	mkqid(&q, ((slot+1)<<QSHIFT) | t->qid.path, PID(c->qid), QTFILE);
	devdir(c, q, t->name, 0, p->user, t->perm, dp);
	return 1;
}

static Chan*
procattach(char *spec)
{
	return devattach('p', spec);
}

static Walkqid*
procwalk(Chan *c, Chan *nc, char **name, int nname)
{
	return devwalk(c, nc, name, nname, nil, 0, procgen);
}

static int
procstat(Chan *c, uchar *db, int n)
{
	return devstat(c, db, n, nil, 0, procgen);
}

/*
 * c's proc, its debug lock held, if it is still the one c was opened
 * for - as 9front's: the pid looked at under the lock, which pexit and
 * exec take too, so the proc can neither go nor be made again in its
 * slot while c's call uses it
 */
static Proc*
lockproc(Chan *c)
{
	Proc *p;

	p = proctab(SLOT(c->qid));
	if(p == nil)
		error(Eprocdied);
	eqlock(&p->debug);
	if(p->pid != PID(c->qid)){
		qunlock(&p->debug);
		error(Eprocdied);
	}
	return p;
}

static Chan*
procopen(Chan *c, int omode)
{
	Proc *p;

	if(QID(c->qid) != Qdir){
		p = lockproc(c);
		if(QID(c->qid) == Qnotepg)
			c->aux = (void*)(uintptr)p->noteid;	/* the group, as it is now: the proc may go */
		qunlock(&p->debug);
	}
	return devopen(c, omode, nil, 0, procgen);
}

static void
procclose(Chan*)
{
}

static long
procread(Chan *c, void *a, long n, vlong off)
{
	Proc *p;
	char buf[256];

	if(c->qid.type & QTDIR)
		return devdirread(c, a, n, nil, 0, procgen);
	/* what is read is copied under the lock: text and args change on exec and a write */
	p = lockproc(c);
	if(waserror()){
		qunlock(&p->debug);
		nexterror();
	}
	switch(QID(c->qid)){
	case Qstatus:
		snprint(buf, sizeof buf, "%-27s %-27s %-11s %11d %11d %11d %11d %11d %11d %11d %11d %11d %11d\n",
			p->text != nil ? p->text : "", p->user != nil ? p->user : "",
			p->psstate != nil ? p->psstate : statename[p->state],
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
		break;
	case Qargs:
		/* what the proc wrote there (9front's: rconnect writes the host), else its name */
		if(p->setargs && p->args != nil)
			strecpy(buf, buf+sizeof buf, p->args);
		else
			strecpy(buf, buf+sizeof buf, p->text != nil ? p->text : "");
		break;
	default:
		error(Egreg);
	}
	poperror();
	qunlock(&p->debug);
	return readstr(off, a, n, buf);
}

static long
procwrite(Chan *c, void *a, long n, vlong)
{
	Proc *p;
	char buf[ERRMAX];

	if(c->qid.type & QTDIR)
		error(Eisdir);
	if(n >= ERRMAX)
		n = ERRMAX-1;
	memmove(buf, a, n);
	buf[n] = 0;
	if(QID(c->qid) == Qnotepg){
		postnotepg((uintptr)c->aux, buf, NUser);
		return n;
	}
	p = lockproc(c);
	if(waserror()){
		qunlock(&p->debug);
		nexterror();
	}
	switch(QID(c->qid)){
	case Qctl:
		/*
		 * private and noswap (factotum's) hold as they are: no
		 * proc's memory can be read here and nothing is swapped
		 */
		if(strncmp(buf, "private", 7) == 0 || strncmp(buf, "noswap", 6) == 0)
			break;
		if(strncmp(buf, "kill", 4) != 0)
			error(Ebadctl);
		postnote(p, 0, "sys: killed", NExit);	/* p->debug held */
		break;
	case Qnote:
		if(!postnote(p, 0, buf, NUser))
			error("note not posted");
		break;
	case Qargs:
		kstrdup(&p->args, buf);
		p->nargs = 0;
		p->setargs = 1;
		break;
	default:
		error(Egreg);
	}
	poperror();
	qunlock(&p->debug);
	return n;
}

Dev procdevtab = {
	'p',
	"proc",

	devreset,
	devinit,
	devshutdown,
	procattach,
	procwalk,
	procstat,
	procopen,
	devcreate,
	procclose,
	procread,
	devbread,
	procwrite,
	devbwrite,
	devremove,
	devwstat,
};
