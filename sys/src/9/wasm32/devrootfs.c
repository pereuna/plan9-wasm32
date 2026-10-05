#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

/*
 * #R: the machine's root files, read-only - what the page gave at boot
 * (BootInfo's rdbase and rdlen, docs/boot-abi-wasm32.md: build/wasm32/root.fs, tools/build-bin3).  The
 * archive is, for each file, its path (a/b/c: no leading /, no empty
 * element, no . or ..) and a 0, its length in four bytes, little-endian,
 * and its bytes (a name ending in / an empty directory, no bytes); its
 * directories are made from the paths, and the mount
 * points init binds to besides.  The page's input: checked, a bad one a
 * panic.
 */

typedef struct Ent Ent;
struct Ent
{
	char	*name;
	int	parent;
	int	child;		/* its first; -1 none */
	int	sibling;
	int	dir;
	uchar	*data;
	ulong	len;
};

static	Ent	*ents;
static	int	nents;
static	int	aents;

static char *mounts[] = {
	"bin", "dev", "env", "srv", "n", "tmp", "proc", "fd", "net",
	/* 9front's /mnt */
	"mnt", "mnt/wsys", "mnt/term", "mnt/temp", "mnt/plumb", "mnt/acme", "mnt/exportfs", "mnt/keys", "mnt/web",
};

static int
newent(char *name, int parent, int dir)
{
	Ent *e;

	if(nents == aents){
		aents = aents ? 2*aents : 64;
		ents = realloc(ents, aents*sizeof(Ent));
		if(ents == nil)
			panic("rootfs: no memory");
	}
	e = &ents[nents];
	e->name = name;
	e->parent = parent;
	e->child = -1;
	e->sibling = -1;
	e->dir = dir;
	e->data = nil;
	e->len = 0;
	if(parent >= 0){
		/* at the end: ls shows the archive's order */
		if(ents[parent].child < 0)
			ents[parent].child = nents;
		else{
			int i;

			for(i = ents[parent].child; ents[i].sibling >= 0; i = ents[i].sibling)
				;
			ents[i].sibling = nents;
		}
	}
	return nents++;
}

static int
lookup(int d, char *name)
{
	int i;

	for(i = ents[d].child; i >= 0; i = ents[i].sibling)
		if(strcmp(ents[i].name, name) == 0)
			return i;
	return -1;
}

/* path's directories, made as needed: the last element's parent */
static int
mkdirs(char *path, char **last)
{
	char *p, *q, *s;
	int d, i;

	d = 0;
	for(p = path; (q = strchr(p, '/')) != nil; p = q+1){
		s = malloc(q-p+1);
		if(s == nil)
			panic("rootfs: no memory");
		memmove(s, p, q-p);
		s[q-p] = 0;
		if((i = lookup(d, s)) >= 0){
			if(!ents[i].dir)
				panic("rootfs: archive: %s: a file, not a directory", path);
			free(s);
			d = i;
		}else
			d = newent(s, d, 1);
	}
	*last = p;
	return d;
}

/* a/b/c: no leading /, no empty element, no . or .. */
static int
goodpath(char *s)
{
	char *q;
	int n;

	for(;;){
		q = strchr(s, '/');
		n = q != nil ? q-s : strlen(s);
		if(n == 0 || n == 1 && s[0] == '.' || n == 2 && s[0] == '.' && s[1] == '.')
			return 0;
		if(q == nil)
			return 1;
		s = q+1;
	}
}

static void
rootfsreset(void)
{
	uchar *a, *p, *e, *z;
	int dir;
	char *name, *last;
	ulong len;
	long n;
	int d, i;

	newent("/", -1, 1);
	ents[0].parent = 0;
	/* the archive where the page loaded it (BootInfo's rdbase, rdlen), the kernel's for good: names and bytes stay there */
	n = bootinfo->rdbase != 0 ? bootinfo->rdlen : 0;
	if(n > 0){
		/* bootinfoinit checked it against the map, this against the memory */
		if((a = bootearlymap(bootinfo->rdbase, n)) == nil)
			panic("rootfs: archive at %#llux, %ld bytes: not in the memory", bootinfo->rdbase, n);
		for(p = a, e = a+n; p < e; p += len){
			if((z = memchr(p, 0, e-p)) == nil)
				panic("rootfs: archive: a name without its 0");
			name = (char*)p;
			dir = z > p && z[-1] == '/';	/* a directory, empty: a/b/ */
			if(dir)
				z[-1] = 0;
			if(!goodpath(name))
				panic("rootfs: archive: bad name %q", name);
			p = z+1;
			if(e-p < 4)
				panic("rootfs: archive: %s: no length", name);
			len = p[0] | p[1]<<8 | p[2]<<16 | (ulong)p[3]<<24;
			p += 4;
			if(len > e-p)
				panic("rootfs: archive: %s: %lud bytes, %ld there", name, len, (long)(e-p));
			if(dir){
				if(len != 0)
					panic("rootfs: archive: directory %q with bytes", name);
				d = mkdirs(name, &last);
				if(lookup(d, last) < 0)
					newent(last, d, 1);
				continue;
			}
			d = mkdirs(name, &last);
			if(lookup(d, last) >= 0)
				panic("rootfs: archive: %s twice", name);
			i = newent(last, d, 0);
			ents[i].data = p;
			ents[i].len = len;
		}
	}
	for(i = 0; i < nelem(mounts); i++){
		d = mkdirs(mounts[i], &last);
		if(lookup(d, last) < 0)
			newent(last, d, 1);
	}
}

static void
entdir(Chan *c, int i, char *name, Dir *dp)
{
	Qid q;

	mkqid(&q, i, 0, ents[i].dir ? QTDIR : QTFILE);
	devdir(c, q, name, ents[i].len, eve, ents[i].dir ? DMDIR|0555 : 0555, dp);
}

static int
rootfsgen(Chan *c, char*, Dirtab*, int, int s, Dir *dp)
{
	int d, i;

	d = c->qid.path;
	if(s == DEVDOTDOT){
		entdir(c, ents[d].parent, ents[d].parent == 0 ? "#R" : ents[ents[d].parent].name, dp);
		return 1;
	}
	for(i = ents[d].child; i >= 0 && s > 0; i = ents[i].sibling)
		s--;
	if(i < 0)
		return -1;
	entdir(c, i, ents[i].name, dp);
	return 1;
}

static Chan*
rootfsattach(char *spec)
{
	return devattach('R', spec);
}

static Walkqid*
rootfswalk(Chan *c, Chan *nc, char **name, int nname)
{
	return devwalk(c, nc, name, nname, nil, 0, rootfsgen);
}

static int
rootfsstat(Chan *c, uchar *db, int n)
{
	Dir d;
	int i;

	i = c->qid.path;
	entdir(c, i, i == 0 ? "#R" : ents[i].name, &d);
	n = convD2M(&d, db, n);
	if(n == 0)
		error(Ebadarg);
	return n;
}

static Chan*
rootfsopen(Chan *c, int omode)
{
	return devopen(c, omode, nil, 0, rootfsgen);
}

static void
rootfsclose(Chan*)
{
}

static long
rootfsread(Chan *c, void *a, long n, vlong off)
{
	Ent *e;

	if(c->qid.type & QTDIR)
		return devdirread(c, a, n, nil, 0, rootfsgen);
	e = &ents[c->qid.path];
	if(off < 0 || off >= e->len)
		return 0;
	if(off+n > e->len)
		n = e->len - off;
	memmove(a, e->data+off, n);
	return n;
}

static long
rootfswrite(Chan*, void*, long, vlong)
{
	error(Eperm);
}

Dev rootfsdevtab = {
	'R',
	"rootfs",

	rootfsreset,
	devinit,
	devshutdown,
	rootfsattach,
	rootfswalk,
	rootfsstat,
	rootfsopen,
	devcreate,
	rootfsclose,
	rootfsread,
	devbread,
	rootfswrite,
	devbwrite,
	devremove,
	devwstat,
};
