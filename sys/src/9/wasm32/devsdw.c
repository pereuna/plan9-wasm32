#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

/*
 * #S: wasm32's disk (docs/architecture.md, D6), as sd names one:
 * #S/sdW0/{ctl,data}.  The disk is a file in the origin's private file
 * system (OPFS), which only a Worker can use synchronously and only one
 * at a time: the page's disk Worker owns it (platform.js, disk()).  The
 * page, as the firmware, describes the disk in BootInfo's config as a
 * machine's firmware describes a device:
 *
 *	*sdW0=regs bytes
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
 * (9front's diskparts).  data is the whole disk.
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
	Qunit,
	Qctl,
	Qdata,
	Qpart,		/* Qpart+i: part[i] */

	Npart	= 16,
	Secsize	= 512,
};

typedef struct Part Part;
struct Part
{
	char	name[KNAMELEN];
	uvlong	start;	/* sectors */
	uvlong	end;
	int	valid;
};

static struct
{
	QLock;
	long	*reg;
	uchar	*dma;
	uvlong	size;
	ulong	reqs;
	ulong	errs;
	int	dead;	/* ours: a request took too long */
	Part	part[Npart];
} disk;

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
	char *s, *f[3];
	char buf[128];
	uvlong regs, size;

	if((s = getconf("*sdW0")) == nil)
		return;
	strecpy(buf, buf+sizeof buf, s);
	if(tokenize(buf, f, nelem(f)) != 2){
		print("sdW0: *sdW0=%s: not regs and bytes\n", s);
		return;
	}
	regs = strtoull(f[0], nil, 0);
	size = strtoull(f[1], nil, 0);
	/* the registers: a page the map keeps from the kernel, in the memory */
	if(regs == 0 || (regs & (BY2PG-1)) != 0 || size == 0
	|| !sdwreserved(regs, BY2PG) || bootearlymap(regs, BY2PG) == nil){
		print("sdW0: *sdW0=%s: no such registers\n", s);
		return;
	}
	if((disk.dma = xalloc(Ndma)) == nil){
		print("sdW0: no memory for its buffer\n");
		return;
	}
	disk.reg = (long*)(uintptr)regs;
	disk.size = size;
	print("sdW0: OPFS disk, %llud bytes\n", size);
}

static int
sdwgen(Chan *c, char*, Dirtab*, int, int s, Dir *dp)
{
	Qid q;

	if(s == DEVDOTDOT){
		mkqid(&q, Qtop, 0, QTDIR);
		devdir(c, q, "#S", 0, eve, 0555, dp);
		return 1;
	}
	switch((ulong)c->qid.path){
	case Qtop:
		if(s != 0 || disk.reg == nil)
			return -1;
		mkqid(&q, Qunit, 0, QTDIR);
		devdir(c, q, "sdW0", 0, eve, 0555, dp);
		return 1;
	case Qunit:
		if(s < nelem(unitdir)){
			devdir(c, unitdir[s].qid, unitdir[s].name, s == 1 ? disk.size : 0, eve, unitdir[s].perm, dp);
			return 1;
		}
		s -= nelem(unitdir);
		if(s >= Npart)
			return -1;
		if(!disk.part[s].valid){	/* a gap in the table: skipped, not the end */
			mkqid(&q, ~0, 0, QTFILE);
			devdir(c, q, "", 0, eve, 0, dp);
			return 0;
		}
		mkqid(&q, Qpart+s, 0, QTFILE);
		devdir(c, q, disk.part[s].name, (disk.part[s].end - disk.part[s].start)*Secsize, eve, 0660, dp);
		return 1;
	default:	/* a file: itself (devstat) */
		if(s != 0)
			return -1;
		if((ulong)c->qid.path >= Qpart){
			s = (ulong)c->qid.path - Qpart;
			if(s >= Npart || !disk.part[s].valid)
				return -1;
			devdir(c, c->qid, disk.part[s].name, (disk.part[s].end - disk.part[s].start)*Secsize, eve, 0660, dp);
			return 1;
		}
		s = (ulong)c->qid.path - Qctl;
		devdir(c, unitdir[s].qid, unitdir[s].name, s == 1 ? disk.size : 0, eve, unitdir[s].perm, dp);
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
	return devopen(c, omode, nil, 0, sdwgen);
}

static void
sdwclose(Chan*)
{
}

/*
 * One request to the disk Worker, in and out of disk.dma: what it did, -1
 * an error.  The Worker ends a request even when it fails (Rstate Diskdead, the
 * page's too if the Worker itself is gone), so the wait ends; one that
 * takes longer than Diskwait makes the disk dead here.  The Worker only
 * touches disk.dma, which is never freed: if it does come back late, it
 * writes nothing the kernel uses for anything else.
 */
static long
sdwio(int op, long n, uvlong off)
{
	long *r, seq;
	ulong deadline;

	r = disk.reg;
	if(disk.dead || r[Rstate] != Diskonline)
		return -1;
	r[Rop] = op;
	r[Rlen] = n;
	r[Raddr] = (ulong)(uintptr)disk.dma;
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
			disk.dead = 1;
			print("sdW0: no answer in %d s: the disk is dead\n", Diskwait);
			disk.errs++;
			return -1;
		}
		platwait(&r[Rdone], r[Rdone], 1000);
	}
	disk.reqs++;
	if(r[Rresult] < 0)
		disk.errs++;
	return r[Rresult];
}

static long
sdwrw(int op, uchar *a, long n, vlong off)
{
	long m, k, done;

	if(off < 0)
		error(Ebadarg);
	if(off >= disk.size)
		return 0;
	if(n > disk.size - off)
		n = disk.size - off;
	eqlock(&disk);
	if(waserror()){
		qunlock(&disk);
		nexterror();
	}
	for(done = 0; done < n; done += m){
		k = n - done;
		if(k > Ndma)
			k = Ndma;
		if(op == Opwrite)
			memmove(disk.dma, a+done, k);
		m = sdwio(op, k, off+done);
		if(m < 0)
			error(Eio);
		if(m > k)
			m = k;
		if(op == Opread)
			memmove(a+done, disk.dma, m);
		if(m < k){
			done += m;
			break;
		}
	}
	poperror();
	qunlock(&disk);
	return done;
}

/* the partition c is, or Enonexist (deleted since) */
static Part*
partof(Chan *c)
{
	ulong i;

	i = (ulong)c->qid.path - Qpart;
	if(i >= Npart || !disk.part[i].valid)
		error(Enonexist);
	return &disk.part[i];
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
addpart(char *name, uvlong start, uvlong end)
{
	int i, free;

	if(*name == 0 || strlen(name) >= KNAMELEN || strcmp(name, "ctl") == 0 || strcmp(name, "data") == 0)
		error(Ebadarg);
	if(start >= end || end > disk.size/Secsize)
		error(Ebadarg);
	free = -1;
	for(i = 0; i < Npart; i++){
		if(disk.part[i].valid && strcmp(disk.part[i].name, name) == 0){
			if(disk.part[i].start == start && disk.part[i].end == end)
				return;	/* as it is: diskparts again */
			error(Eexist);
		}
		if(!disk.part[i].valid && free < 0)
			free = i;
	}
	if(free < 0)
		error("too many partitions");
	strecpy(disk.part[free].name, disk.part[free].name+KNAMELEN, name);
	disk.part[free].start = start;
	disk.part[free].end = end;
	disk.part[free].valid = 1;
}

static void
delpart(char *name)
{
	int i;

	for(i = 0; i < Npart; i++)
		if(disk.part[i].valid && strcmp(disk.part[i].name, name) == 0){
			disk.part[i].valid = 0;
			return;
		}
	error(Enonexist);
}

static long
sdwread(Chan *c, void *a, long n, vlong off)
{
	char buf[256 + Npart*(KNAMELEN+48)], *p, *e;
	Part *pt;
	int i;

	switch((ulong)c->qid.path){
	case Qtop:
	case Qunit:
		return devdirread(c, a, n, nil, 0, sdwgen);
	case Qctl:
		p = buf;
		e = buf+sizeof buf;
		p = seprint(p, e, "inquiry Plan2001 OPFS disk\ngeometry %llud 512\nrequests %lud errors %lud\nstate %s\n",
			disk.size/512, disk.reqs, disk.errs, disk.dead || disk.reg[Rstate] != Diskonline ? "dead" : "online");
		for(i = 0; i < Npart; i++)
			if(disk.part[i].valid)
				p = seprint(p, e, "part %s %llud %llud\n", disk.part[i].name, disk.part[i].start, disk.part[i].end);
		USED(p);
		return readstr(off, a, n, buf);
	case Qdata:
		return sdwrw(Opread, a, n, off);
	}
	if((ulong)c->qid.path >= Qpart){
		pt = partof(c);
		return sdwrw(Opread, a, partlen(pt, n, off), pt->start*Secsize + off);
	}
	error(Egreg);
	return 0;
}

static long
sdwwrite(Chan *c, void *a, long n, vlong off)
{
	Cmdbuf *cb;
	Part *pt;

	switch((ulong)c->qid.path){
	case Qctl:
		cb = parsecmd(a, n);
		if(waserror()){
			free(cb);
			nexterror();
		}
		if(cb->nf == 1 && strcmp(cb->f[0], "flush") == 0){
			eqlock(&disk);
			if(waserror()){
				qunlock(&disk);
				nexterror();
			}
			if(sdwio(Opflush, 0, 0) < 0)
				error(Eio);
			poperror();
			qunlock(&disk);
		}else if(cb->nf == 4 && strcmp(cb->f[0], "part") == 0)
			addpart(cb->f[1], strtoull(cb->f[2], nil, 0), strtoull(cb->f[3], nil, 0));
		else if(cb->nf == 2 && strcmp(cb->f[0], "delpart") == 0)
			delpart(cb->f[1]);
		else
			error(Ebadctl);
		poperror();
		free(cb);
		return n;
	case Qdata:
		return sdwrw(Opwrite, a, n, off);
	}
	if((ulong)c->qid.path >= Qpart){
		pt = partof(c);
		return sdwrw(Opwrite, a, partlen(pt, n, off), pt->start*Secsize + off);
	}
	error(Eperm);
	return 0;
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
