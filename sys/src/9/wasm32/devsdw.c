#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

/*
 * #S: wasm32's disks (docs/kernel.md), as sd names them:
 * #S/sdWn/{ctl,data}.  sdW0 is a file in the origin's private file
 * system (OPFS), which only a Worker can use synchronously and only one
 * at a time: the page's disk Worker owns it (platform.js, disk()).  sdW1
 * is read-only, a file on the server (the distribution, as a PC's CD:
 * its Worker reads it with HTTP range requests).  The page, as the
 * firmware, describes a disk in BootInfo's config as a machine's
 * firmware describes a device:
 *
 *	*sdWn=regs bytes [ro]
 *
 * regs is a page of the kernel's memory the map calls Reserved - the
 * disk's registers, words the disk Worker waits on (Atomics.wait) - and
 * bytes the disk's size.  One request at a time (the QLock): the kernel
 * fills in op, len, addr and off, adds one to seq and wakes the Worker;
 * the Worker does it into or out of the disk's buffer in the kernel's
 * memory (addr), sets result and done = seq and wakes the kernel.  A
 * Worker that fails says so (state) and the disk is dead: Eio, not a
 * kernel waiting for ever.  What it wrote goes to the
 * file (flush) once the disk has been idle for a moment, and on a ctl
 * "flush".  A file system on it is a program's: hjfs (/boot/init).
 *
 * Partitions as sd's (sd(3)): "part NAME START END" (sectors of 512
 * bytes) on ctl makes #S/sdW0/NAME, "delpart NAME" takes it away, and ctl
 * says them as "part" lines - what disk/fdisk -p and disk/prep -p write
 * (9front's diskparts).  data is the whole disk.  #S/sdctl says the
 * disks, as sd(3)'s does its controllers, and takes its config
 * messages (fshalt's) as done.  A qid's path is the
 * unit's number and the file (Qunit ...) in its low bits.
 */
enum
{
	Rseq,		/* the kernel's: a request */
	Rdone,		/* the Worker's: the request done */
	Rop,
	Rlen,
	Raddr,
	Rofflo,
	Roffhi,
	Rresult,	/* bytes, or -1 */
	Rstate,		/* the page's: Diskonline; Diskdead once the Worker has failed (it ends the request with -1) */
	Nreg,

	Diskonline	= 1,
	Diskdead	= -1,

	Ndma	= 64*1024,	/* the bounce buffer: what the Worker reads into and writes from */
	Diskwait	= 30,	/* seconds for one request, then the disk is dead */

	Opread	= 1,
	Opwrite,
	Opflush,

	Qtop	= 0,
	Qsdctl,		/* #S/sdctl: the controller, as sd(3)'s */
	Qunit,
	Qctl,
	Qdata,
	Qpart,		/* Qpart+i: part[i] */

	Npart	= 16,
	Secsize	= 512,
	Ndisk	= 2,
	Qshift	= 8,	/* a qid's path: unit<<Qshift | file */
};

#define QFILE(q)	((ulong)(q).path & ((1<<Qshift)-1))
#define QUNIT(q)	((ulong)(q).path >> Qshift)
#define QID(u, f)	((u)<<Qshift | (f))

typedef struct Part Part;
struct Part
{
	char	name[KNAMELEN];
	uvlong	start;	/* sectors */
	uvlong	end;
	int	valid;
};

typedef struct Disk Disk;
struct Disk
{
	QLock;
	char	name[8];	/* sdWn */
	long	*reg;
	uchar	*dma;
	uvlong	size;
	int	ro;
	ulong	reqs;
	ulong	errs;
	int	dead;	/* ours: a request took too long */
	Part	part[Npart];
};

static Disk disks[Ndisk];

static Dirtab unitdir[] = {
	"ctl",	{Qctl},		0,	0664,
	"data",	{Qdata},	0,	0660,
};

/* is [pa, pa+len) in an entry of the memory map the kernel keeps out of its own (Reserved)? */
static int
sdwreserved(uvlong pa, uvlong len)
{
	BootMem *m;
	int i;

	for(i = 0; i < bootinfo->mmapcount; i++){
		m = bootmem(i);
		if(m->type == BootMemReserved && pa >= m->base && pa - m->base <= m->len && len <= m->len - (pa - m->base))
			return 1;
	}
	return 0;
}

static void
sdwreset(void)
{
	char *s, *f[4], buf[128], name[16];
	uvlong regs, size;
	Disk *d;
	int n, u;

	for(u = 0; u < Ndisk; u++){
		d = &disks[u];
		snprint(d->name, sizeof d->name, "sdW%d", u);
		snprint(name, sizeof name, "*%s", d->name);
		if((s = getconf(name)) == nil)
			continue;
		strecpy(buf, buf+sizeof buf, s);
		n = tokenize(buf, f, nelem(f));
		if(n != 2 && (n != 3 || strcmp(f[2], "ro") != 0)){
			print("%s: %s=%s: not regs, bytes and ro or none\n", d->name, name, s);
			continue;
		}
		regs = strtoull(f[0], nil, 0);
		size = strtoull(f[1], nil, 0);
		/* the registers: a page the map keeps from the kernel, in the memory */
		if(regs == 0 || (regs & (BY2PG-1)) != 0 || size == 0
		|| !sdwreserved(regs, BY2PG) || bootearlymap(regs, BY2PG) == nil){
			print("%s: %s=%s: no such registers\n", d->name, name, s);
			continue;
		}
		if((d->dma = xalloc(Ndma)) == nil){
			print("%s: no memory for its buffer\n", d->name);
			continue;
		}
		d->reg = (long*)(uintptr)regs;
		d->size = size;
		d->ro = n == 3;
		print("%s: %s, %llud bytes\n", d->name, d->ro ? "the distribution (read-only)" : "OPFS disk", size);
	}
}

static int
sdwgen(Chan *c, char*, Dirtab*, int, int s, Dir *dp)
{
	Qid q;
	Disk *d;
	int u, i;

	if(s == DEVDOTDOT){
		mkqid(&q, Qtop, 0, QTDIR);
		devdir(c, q, "#S", 0, eve, 0555, dp);
		return 1;
	}
	if(QFILE(c->qid) == Qsdctl){
		if(s != 0)
			return -1;
		devdir(c, c->qid, "sdctl", 0, eve, 0664, dp);
		return 1;
	}
	if(QFILE(c->qid) == Qtop){
		if(s-- == 0){
			mkqid(&q, Qsdctl, 0, QTFILE);
			devdir(c, q, "sdctl", 0, eve, 0664, dp);
			return 1;
		}
		for(u = 0; u < Ndisk; u++)
			if(disks[u].reg != nil && s-- == 0){
				mkqid(&q, QID(u, Qunit), 0, QTDIR);
				devdir(c, q, disks[u].name, 0, eve, 0555, dp);
				return 1;
			}
		return -1;
	}
	u = QUNIT(c->qid);
	if(u >= Ndisk || disks[u].reg == nil)
		return -1;
	d = &disks[u];
	switch(QFILE(c->qid)){
	case Qunit:
		if(s < nelem(unitdir)){
			mkqid(&q, QID(u, unitdir[s].qid.path), 0, QTFILE);
			devdir(c, q, unitdir[s].name, s == 1 ? d->size : 0, eve, d->ro && unitdir[s].qid.path == Qdata ? 0440 : unitdir[s].perm, dp);
			return 1;
		}
		s -= nelem(unitdir);
		if(s >= Npart)
			return -1;
		if(!d->part[s].valid){	/* a gap in the table: skipped, not the end */
			mkqid(&q, ~0, 0, QTFILE);
			devdir(c, q, "", 0, eve, 0, dp);
			return 0;
		}
		mkqid(&q, QID(u, Qpart+s), 0, QTFILE);
		devdir(c, q, d->part[s].name, (d->part[s].end - d->part[s].start)*Secsize, eve, d->ro ? 0440 : 0660, dp);
		return 1;
	default:	/* a file: itself (devstat) */
		if(s != 0)
			return -1;
		if(QFILE(c->qid) >= Qpart){
			i = QFILE(c->qid) - Qpart;
			if(i >= Npart || !d->part[i].valid)
				return -1;
			devdir(c, c->qid, d->part[i].name, (d->part[i].end - d->part[i].start)*Secsize, eve, d->ro ? 0440 : 0660, dp);
			return 1;
		}
		i = QFILE(c->qid) - Qctl;
		devdir(c, c->qid, unitdir[i].name, i == 1 ? d->size : 0, eve, d->ro && unitdir[i].qid.path == Qdata ? 0440 : unitdir[i].perm, dp);
		return 1;
	}
}

static Chan*
sdwattach(char *spec)
{
	return devattach('S', spec);
}

static Walkqid*
sdwwalk(Chan *c, Chan *nc, char **name, int nname)
{
	return devwalk(c, nc, name, nname, nil, 0, sdwgen);
}

static int
sdwstat(Chan *c, uchar *db, int n)
{
	return devstat(c, db, n, nil, 0, sdwgen);
}

static Chan*
sdwopen(Chan *c, int omode)
{
	if(QFILE(c->qid) != Qtop && QUNIT(c->qid) < Ndisk && disks[QUNIT(c->qid)].ro
	&& QFILE(c->qid) != Qctl && (omode&3) != OREAD)
		error(Eperm);
	return devopen(c, omode, nil, 0, sdwgen);
}

static void
sdwclose(Chan*)
{
}

/*
 * One request to the disk Worker, in and out of d->dma: what it did, -1
 * an error.  The Worker ends a request even when it fails (Rstate Diskdead, the
 * page's too if the Worker itself is gone), so the wait ends; one that
 * takes longer than Diskwait makes the disk dead here.  The Worker only
 * touches d->dma, which is never freed: if it does come back late, it
 * writes nothing the kernel uses for anything else.
 */
static long
sdwio(Disk *d, int op, long n, uvlong off)
{
	long *r, seq;
	ulong deadline;

	r = d->reg;
	if(d->dead || r[Rstate] != Diskonline)
		return -1;
	r[Rop] = op;
	r[Rlen] = n;
	r[Raddr] = (ulong)(uintptr)d->dma;
	r[Rofflo] = (ulong)off;
	r[Roffhi] = (ulong)(off>>32);
	r[Rresult] = -1;
	seq = r[Rseq] + 1;
	coherence();
	r[Rseq] = seq;
	platwake(&r[Rseq], 1);
	deadline = seconds() + Diskwait;
	while(r[Rdone] != seq){
		if(seconds() >= deadline){
			d->dead = 1;
			print("%s: no answer in %d s: the disk is dead\n", d->name, Diskwait);
			d->errs++;
			return -1;
		}
		platwait(&r[Rdone], r[Rdone], 1000);
	}
	d->reqs++;
	if(r[Rresult] < 0)
		d->errs++;
	return r[Rresult];
}

static long
sdwrw(Disk *d, int op, uchar *a, long n, vlong off)
{
	long m, k, done;

	if(off < 0)
		error(Ebadarg);
	if(off >= d->size)
		return 0;
	if(n > d->size - off)
		n = d->size - off;
	eqlock(d);
	if(waserror()){
		qunlock(d);
		nexterror();
	}
	for(done = 0; done < n; done += m){
		k = n - done;
		if(k > Ndma)
			k = Ndma;
		if(op == Opwrite)
			memmove(d->dma, a+done, k);
		m = sdwio(d, op, k, off+done);
		if(m < 0)
			error(Eio);
		if(m > k)
			m = k;
		if(op == Opread)
			memmove(a+done, d->dma, m);
		if(m < k){
			done += m;
			break;
		}
	}
	poperror();
	qunlock(d);
	return done;
}

/* the partition c is, or Enonexist (deleted since) */
static Part*
partof(Disk *d, Chan *c)
{
	ulong i;

	i = QFILE(c->qid) - Qpart;
	if(i >= Npart || !d->part[i].valid)
		error(Enonexist);
	return &d->part[i];
}

/* n bytes at off, cut to the partition's end */
static long
partlen(Part *pt, long n, vlong off)
{
	uvlong len;

	len = (pt->end - pt->start)*Secsize;
	if(off < 0)
		error(Ebadarg);
	if(off >= len)
		return 0;
	if(n > len - off)
		n = len - off;
	return n;
}

static void
addpart(Disk *d, char *name, uvlong start, uvlong end)
{
	int i, free;

	if(*name == 0 || strlen(name) >= KNAMELEN || strcmp(name, "ctl") == 0 || strcmp(name, "data") == 0)
		error(Ebadarg);
	if(start >= end || end > d->size/Secsize)
		error(Ebadarg);
	free = -1;
	for(i = 0; i < Npart; i++){
		if(d->part[i].valid && strcmp(d->part[i].name, name) == 0){
			if(d->part[i].start == start && d->part[i].end == end)
				return;	/* as it is: diskparts again */
			error(Eexist);
		}
		if(!d->part[i].valid && free < 0)
			free = i;
	}
	if(free < 0)
		error("too many partitions");
	strecpy(d->part[free].name, d->part[free].name+KNAMELEN, name);
	d->part[free].start = start;
	d->part[free].end = end;
	d->part[free].valid = 1;
}

static void
delpart(Disk *d, char *name)
{
	int i;

	for(i = 0; i < Npart; i++)
		if(d->part[i].valid && strcmp(d->part[i].name, name) == 0){
			d->part[i].valid = 0;
			return;
		}
	error(Enonexist);
}

static long
sdwread(Chan *c, void *a, long n, vlong off)
{
	char buf[256 + Npart*(KNAMELEN+48)], *p, *e;
	Part *pt;
	Disk *d;
	int i;

	if(QFILE(c->qid) == Qtop)
		return devdirread(c, a, n, nil, 0, sdwgen);
	if(QFILE(c->qid) == Qsdctl){
		p = buf;
		e = buf+sizeof buf;
		for(i = 0; i < Ndisk; i++)
			if(disks[i].reg != nil)
				p = seprint(p, e, "%s wasm32 %s\n", disks[i].name, disks[i].ro ? "distribution" : "opfs");
		USED(p);
		return readstr(off, a, n, buf);
	}
	d = &disks[QUNIT(c->qid)];
	switch(QFILE(c->qid)){
	case Qunit:
		return devdirread(c, a, n, nil, 0, sdwgen);
	case Qctl:
		p = buf;
		e = buf+sizeof buf;
		p = seprint(p, e, "inquiry Plan9-wasm32 %s\ngeometry %llud 512\nrequests %lud errors %lud\nstate %s\n",
			d->ro ? "distribution" : "OPFS disk",
			d->size/512, d->reqs, d->errs, d->dead || d->reg[Rstate] != Diskonline ? "dead" : "online");
		for(i = 0; i < Npart; i++)
			if(d->part[i].valid)
				p = seprint(p, e, "part %s %llud %llud\n", d->part[i].name, d->part[i].start, d->part[i].end);
		USED(p);
		return readstr(off, a, n, buf);
	case Qdata:
		return sdwrw(d, Opread, a, n, off);
	}
	if(QFILE(c->qid) >= Qpart){
		pt = partof(d, c);
		return sdwrw(d, Opread, a, partlen(pt, n, off), pt->start*Secsize + off);
	}
	error(Egreg);
	return 0;
}

static long
sdwwrite(Chan *c, void *a, long n, vlong off)
{
	Cmdbuf *cb;
	Part *pt;
	Disk *d;

	if(QFILE(c->qid) == Qtop)
		error(Eperm);
	if(QFILE(c->qid) == Qsdctl)	/* sd(3)'s config ... (fshalt: switch off): nothing to do */
		return n;
	d = &disks[QUNIT(c->qid)];
	switch(QFILE(c->qid)){
	case Qctl:
		cb = parsecmd(a, n);
		if(waserror()){
			free(cb);
			nexterror();
		}
		if(cb->nf == 1 && strcmp(cb->f[0], "flush") == 0){
			eqlock(d);
			if(waserror()){
				qunlock(d);
				nexterror();
			}
			if(!d->ro && sdwio(d, Opflush, 0, 0) < 0)
				error(Eio);
			poperror();
			qunlock(d);
		}else if(cb->nf == 4 && strcmp(cb->f[0], "part") == 0)
			addpart(d, cb->f[1], strtoull(cb->f[2], nil, 0), strtoull(cb->f[3], nil, 0));
		else if(cb->nf == 2 && strcmp(cb->f[0], "delpart") == 0)
			delpart(d, cb->f[1]);
		else
			error(Ebadctl);
		poperror();
		free(cb);
		return n;
	case Qdata:
		if(d->ro)
			error(Eperm);
		return sdwrw(d, Opwrite, a, n, off);
	}
	if(QFILE(c->qid) >= Qpart){
		if(d->ro)
			error(Eperm);
		pt = partof(d, c);
		return sdwrw(d, Opwrite, a, partlen(pt, n, off), pt->start*Secsize + off);
	}
	error(Eperm);
	return 0;
}

/* what the disks have been given goes to their files: before a reboot (arch.c) */
void
sdwflushall(void)
{
	Disk *d;

	for(d = disks; d < disks+Ndisk; d++){
		if(d->reg == nil || d->ro || !canqlock(d))
			continue;
		sdwio(d, Opflush, 0, 0);
		qunlock(d);
	}
}

Dev sdwdevtab = {
	'S',
	"sdw",

	sdwreset,
	devinit,
	devshutdown,
	sdwattach,
	sdwwalk,
	sdwstat,
	sdwopen,
	devcreate,
	sdwclose,
	sdwread,
	devbread,
	sdwwrite,
	devbwrite,
	devremove,
	devwstat,
};
