#define	EXTERN
#include	"l.h"

/*
 *	3l [-k] [-o out] [-E entry] [-s stacksize] [-v] file.3 ... lib.a ...
 * -k: a kernel (sys/src/9/wasm32): its memory imported, shared, its data
 * passive (_init puts it there, once); the functions it calls and does not
 * define are imported from "platform" if they are plat* or eia*; no unwinding
 * (its procs are Workers)
 * Libraries: their members that define what is undefined, until
 * nothing more is.
 */

static	char**	libs;
static	int	nlibs;
static	int	maxlibs;
static	char*	pragmalib;	/* per lib: 1 if only #pragma lib named it (missing: no error, the undefined say so) */
void	addlib(char*);

static void
usage(void)
{
	fprint(2, "usage: 3l [-k] [-o out] [-E entry] [-s stack] [-v] file.3 ... lib.a ...\n");
	exits("usage");
}

#define	ARMAGIC	"!<arch>\n"

/* whether f is an archive (ar's magic), whatever its name */
static int
isarchive(char *f)
{
	char buf[sizeof ARMAGIC - 1];
	int fd, n;

	if((fd = open(f, OREAD)) < 0)
		return 0;
	n = read(fd, buf, sizeof buf);
	close(fd);
	return n == sizeof buf && memcmp(buf, ARMAGIC, sizeof buf) == 0;
}

void
main(int argc, char *argv[])
{
	char *f;
	int n;

	outfile = "3.out";
	entry = "_main";
	stacksize = STACK;
	ARGBEGIN{
	case 'o':
		outfile = EARGF(usage());
		break;
	case 'E':
		entry = EARGF(usage());
		break;
	case 's':
		stacksize = strtol(EARGF(usage()), nil, 0);
		break;
	case 'k':	/* a kernel: memory imported shared, the platform's functions imported */
	case 'v':
	case 'a':
		debug[ARGC()] = 1;
		break;
	default:
		usage();
	}ARGEND
	if(argc == 0)
		usage();
	maxlibs = argc + 16;
	libs = emalloc(maxlibs*sizeof(char*));
	pragmalib = emalloc(maxlibs);
	for(; argc > 0; argc--, argv++) {
		f = *argv;
		n = strlen(f);
		if(n > 2 && strcmp(f+n-2, ".a") == 0 || isarchive(f))	/* an archive by its magic too: cc.a$O */
			libs[nlibs++] = f;
		else
			ldfile(f);
	}
	lookup(entry, 0)->ref = 1;
	ldlibs();
	if(nerrors)
		exits("error");
	layout();
	if(nerrors)
		exits("error");
	asmb();
	exits(nerrors ? "error" : nil);
}

void*
emalloc(long n)
{
	void *p;

	p = malloc(n);
	if(p == nil) {
		fprint(2, "3l: out of memory\n");
		exits("memory");
	}
	memset(p, 0, n);
	return p;
}

void
diag(char *fmt, ...)
{
	char buf[1024];
	va_list arg;

	va_start(arg, fmt);
	vseprint(buf, buf+sizeof(buf), fmt, arg);
	va_end(arg);
	fprint(2, "3l: %s\n", buf);
	nerrors++;
	if(nerrors > (debug['v'] ? 200 : 20)) {
		fprint(2, "3l: too many errors\n");
		exits("error");
	}
}

Sym*
lookup(char *name, int v)
{
	Sym *s;
	ulong h;
	char *p;

	h = v;
	for(p = name; *p; p++)
		h = h*31 + (uchar)*p;
	h %= NHASH;
	for(s = hash[h]; s != nil; s = s->hash)
		if(s->version == v && strcmp(s->name, name) == 0)
			return s;
	s = emalloc(sizeof(*s));
	s->name = strdup(name);
	s->version = v;
	s->hash = hash[h];
	hash[h] = s;
	if(lastsym == nil)
		allsym = s;
	else
		lastsym->next = s;
	lastsym = s;
	return s;
}

/*
 * an object in memory
 */
typedef struct Obj Obj;
struct Obj
{
	uchar*	p;
	uchar*	ep;
	char*	name;
	Sym**	syms;
	int	nsyms;
	int	bad;
};

static int
get1(Obj *o)
{
	if(o->p >= o->ep) {
		o->bad = 1;
		return 0;
	}
	return *o->p++;
}

static int
get2(Obj *o)
{
	int v;

	v = get1(o);
	return v | get1(o)<<8;
}

static long
get4(Obj *o)
{
	long v;

	v = get2(o);
	return v | (long)get2(o)<<16;
}

static vlong
get8(Obj *o)
{
	uvlong v;

	v = (ulong)get4(o);
	return v | (uvlong)(ulong)get4(o)<<32;
}

static void
getadr(Obj *o, Adr *a)
{
	int n;
	union {
		uvlong	v;
		double	d;
	} u;

	memset(a, 0, sizeof(*a));
	a->type = get1(o);
	if(a->type == D_NONE)
		return;
	a->index = get1(o);
	a->reg = get4(o);
	n = get2(o);
	if(n > 0) {
		if(n >= o->nsyms || o->syms[n] == nil) {
			diag("%s: bad symbol number %d", o->name, n);
			o->bad = 1;
		} else
			a->sym = o->syms[n];
	}
	switch(a->type) {
	case D_FCONST:
		u.v = get8(o);
		a->dval = u.d;
		break;
	case D_SCONST:
		if(o->ep - o->p < NSNAME) {
			o->bad = 1;
			break;
		}
		memmove(a->sval, o->p, NSNAME);
		o->p += NSNAME;
		break;
	default:
		a->offset = get8(o);
		break;
	}
}

static void
ref(Adr *a)
{
	int t;

	t = a->type == D_ADDR ? a->index : a->type;
	if(a->sym != nil && (t == D_EXTERN || t == D_STATIC))
		a->sym->ref = 1;
}

/*
 * data: applied when all is loaded and the sizes are known
 */
typedef struct Dat Dat;
struct Dat
{
	Sym*	sym;
	long	off;
	int	width;
	Adr	val;
	Dat*	link;
};
static	Dat*	dats;
static	Dat*	lastdat;

static void
adddata(Prog *p)
{
	Dat *d;

	d = emalloc(sizeof(*d));
	d->sym = p->from.sym;
	d->off = p->from.offset;
	d->width = p->from2.offset;
	d->val = p->to;
	if(lastdat == nil)
		dats = d;
	else
		lastdat->link = d;
	lastdat = d;
}

static void
loadobj(uchar *buf, long n, char *name)
{
	Obj o;
	Prog **recs, *p, *text, *last, **code;
	int nrecs, maxrecs, as, t, num, i;
	char *s;
	Sym *sym;

	memset(&o, 0, sizeof o);
	o.p = buf;
	o.ep = buf+n;
	o.name = name;
	if(n < strlen(OBJMAGIC) || memcmp(buf, OBJMAGIC, strlen(OBJMAGIC)) != 0) {
		diag("%s: not a wasm32 object", name);
		return;
	}
	o.p += strlen(OBJMAGIC);
	version++;
	o.nsyms = 64;
	o.syms = emalloc(o.nsyms*sizeof(Sym*));
	maxrecs = 1024;
	nrecs = 0;
	recs = emalloc(maxrecs*sizeof(Prog*));
	text = nil;
	last = nil;
	while(o.p < o.ep && !o.bad) {
		as = get2(&o);
		if(as == ANAME) {
			t = get1(&o);
			num = get2(&o);
			s = (char*)o.p;
			while(o.p < o.ep && *o.p != 0)
				o.p++;
			if(o.p >= o.ep) {
				o.bad = 1;
				break;
			}
			o.p++;
			while(num >= o.nsyms) {
				o.syms = realloc(o.syms, 2*o.nsyms*sizeof(Sym*));
				memset(o.syms+o.nsyms, 0, o.nsyms*sizeof(Sym*));
				o.nsyms *= 2;
			}
			if(t == D_FILE) {	/* #pragma lib (3c's outlibs) */
				addlib(s);
				continue;
			}
			o.syms[num] = lookup(s, t == D_STATIC ? version : 0);
			continue;
		}
		p = emalloc(sizeof(*p));
		p->as = as;
		p->k = get1(&o);
		p->k2 = get1(&o);
		p->lineno = get4(&o);
		getadr(&o, &p->from);
		getadr(&o, &p->from2);
		getadr(&o, &p->to);
		if(nrecs >= maxrecs) {
			maxrecs *= 2;
			recs = realloc(recs, maxrecs*sizeof(Prog*));
		}
		recs[nrecs++] = p;

		switch(as) {
		case ATEXT:
			sym = p->from.sym;
			if(sym == nil) {
				diag("%s: TEXT without a name", name);
				break;
			}
			if(sym->type == STEXT || sym->type == SDATA) {
				diag("%s: %s defined again (%s)", name, sym->name, sym->file);
				break;
			}
			sym->type = STEXT;
			sym->file = name;
			sym->text = p;
			text = p;
			last = p;
			break;
		case AGLOBL:
			sym = p->from.sym;
			if(sym->type == STEXT) {
				diag("%s: %s is text and data", name, sym->name);
				break;
			}
			if(sym->type == SNONE)
				sym->type = SBSS;
			if(p->to.offset > sym->size)
				sym->size = p->to.offset;
			sym->file = name;
			break;
		case ADATA:
			sym = p->from.sym;
			sym->type = SDATA;
			adddata(p);
			ref(&p->to);
			break;
		case AEND:
			text = nil;
			break;
		default:
			if(text == nil) {
				diag("%s: instruction %d outside a function", name, as);
				break;
			}
			last->link = p;
			last = p;
			ref(&p->from);
			ref(&p->from2);
			ref(&p->to);
			break;
		}
	}
	if(o.bad)
		diag("%s: bad object", name);

	/*
	 * branches: to a record's index; the code at or after it
	 */
	code = emalloc((nrecs+1)*sizeof(Prog*));
	p = nil;
	for(i = nrecs-1; i >= 0; i--) {
		switch(recs[i]->as) {
		case ADATA:
		case AGLOBL:
		case ATEXT:
		case AEND:
			break;
		default:
			p = recs[i];
		}
		code[i] = p;
		if(recs[i]->as == ATEXT || recs[i]->as == AEND)
			p = nil;
	}
	for(i = 0; i < nrecs; i++) {
		p = recs[i];
		if(p->to.type != D_BRANCH)
			continue;
		if(p->to.offset < 0 || p->to.offset >= nrecs || code[p->to.offset] == nil) {
			diag("%s: branch out of its function (line %ld)", name, p->lineno);
			continue;
		}
		p->targ = code[p->to.offset];
	}
	free(code);
	free(recs);
	free(o.syms);
}

static uchar*
readall(char *file, long *np)
{
	int fd;
	uchar *buf;
	long n, m;

	fd = open(file, OREAD);
	if(fd < 0) {
		diag("cannot open %s: %r", file);
		return nil;
	}
	n = seek(fd, 0, 2);
	if(n < 0 || seek(fd, 0, 0) != 0) {
		diag("%s: %r", file);
		close(fd);
		return nil;
	}
	buf = emalloc(n+1);
	for(m = 0; m < n; ) {
		long r;

		r = read(fd, buf+m, n-m);
		if(r <= 0) {
			diag("%s: short read", file);
			close(fd);
			free(buf);
			return nil;
		}
		m += r;
	}
	close(fd);
	*np = n;
	return buf;
}

void
ldfile(char *file)
{
	uchar *buf;
	long n;

	buf = readall(file, &n);
	if(buf == nil)
		return;
	loadobj(buf, n, file);
}

/*
 * libraries: ar(1) archives of .3 objects
 */
#define	ARMAG	"!<arch>\n"
enum { SAR_HDR = 60 };

typedef struct Member Member;
struct Member
{
	char	name[64];
	uchar*	p;
	long	n;
	int	loaded;
	char**	defs;	/* the extern symbols it defines */
	int	ndefs;
};

static void
memberdefs(Member *m)
{
	uchar *p, *ep;
	int as, t, num, maxdefs, i;
	static char *names[65536];
	char *s;

	/* names by number, then TEXT/GLOBL/DATA of extern names */
	memset(names, 0, sizeof names);
	maxdefs = 16;
	m->defs = emalloc(maxdefs*sizeof(char*));
	p = m->p + strlen(OBJMAGIC);
	ep = m->p + m->n;
	while(p + 2 <= ep) {
		as = p[0] | p[1]<<8;
		p += 2;
		if(as == ANAME) {
			if(p + 3 > ep)
				return;
			t = p[0];
			num = p[1] | p[2]<<8;
			p += 3;
			s = (char*)p;
			while(p < ep && *p)
				p++;
			p++;
			if(t != D_FILE)
				names[num] = t == D_STATIC ? nil : s;
			continue;
		}
		/* k k2 lineno, then from: type index reg sym */
		if(p + 6 > ep)
			return;
		p += 6;
		for(i = 0; i < 3; i++) {
			int type, sym;

			if(p >= ep)
				return;
			type = *p++;
			if(type == D_NONE)
				continue;
			if(p + 7 > ep)
				return;
			sym = p[5] | p[6]<<8;
			p += 7 + 8;
			if(i == 0 && (as == ATEXT || as == AGLOBL || as == ADATA) && sym && names[sym]) {
				if(m->ndefs >= maxdefs) {
					maxdefs *= 2;
					m->defs = realloc(m->defs, maxdefs*sizeof(char*));
				}
				m->defs[m->ndefs++] = names[sym];
			}
		}
	}
}

static int
needed(Member *m)
{
	int i;
	Sym *s;

	for(i = 0; i < m->ndefs; i++) {
		s = lookup(m->defs[i], 0);
		if(s->ref && s->type == SNONE)
			return 1;
	}
	return 0;
}

/*
 * a library a program's #pragma lib names: /$objtype/lib/NAME (the loader's
 * own place, as Plan 9's), or NAME if it is a path; not if one of that
 * name is there already - given on the command line, as tools/build-*
 * give theirs with their paths
 */
void
addlib(char *name)
{
	char *path, *b, *ob, *ot, *d, buf[256];
	int i;

	/* $O is the object's letter, as Plan 9's loaders read it (cc.h: ../cc/cc.a$O) */
	for(d = buf; *name && d < buf+sizeof buf-2; name++)
		if(name[0] == '$' && name[1] == 'O') {
			*d++ = '3';
			name++;
		} else
			*d++ = *name;
	*d = 0;
	name = buf;
	if(name[0] == '/' || strncmp(name, "./", 2) == 0 || strncmp(name, "../", 3) == 0)
		path = strdup(name);
	else {
		ot = getenv("objtype");
		path = smprint("/%s/lib/%s", ot != nil && *ot ? ot : "wasm32", name);	/* ot not freed: POSIX's getenv is not malloc'd */
	}
	b = strrchr(path, '/');
	b = b ? b+1 : path;
	for(i = 0; i < nlibs; i++) {
		ob = strrchr(libs[i], '/');
		ob = ob ? ob+1 : libs[i];
		if(strcmp(ob, b) == 0) {
			free(path);
			return;
		}
	}
	if(nlibs >= maxlibs) {
		maxlibs *= 2;
		libs = realloc(libs, maxlibs*sizeof(char*));
		pragmalib = realloc(pragmalib, maxlibs);
	}
	pragmalib[nlibs] = 1;
	libs[nlibs++] = path;
}

typedef struct Lib Lib;
struct Lib
{
	Member*	ms;
	int	nm;
};

/* the archive's members and what each defines */
static void
readlib(char *file, int quiet, Lib *lb)
{
	uchar *buf, *p, *ep;
	long n, size;
	int maxm;
	char *s;

	lb->ms = nil;
	lb->nm = 0;
	if(quiet && access(file, 0) < 0)
		return;
	buf = readall(file, &n);
	if(buf == nil)
		return;
	if(n < strlen(ARMAG) || memcmp(buf, ARMAG, strlen(ARMAG)) != 0) {
		diag("%s: not an archive", file);
		return;
	}
	maxm = 64;
	lb->ms = emalloc(maxm*sizeof(Member));
	p = buf + strlen(ARMAG);
	ep = buf + n;
	while(p + SAR_HDR <= ep) {
		size = strtol((char*)p+48, nil, 10);
		if(lb->nm >= maxm) {
			maxm *= 2;
			lb->ms = realloc(lb->ms, maxm*sizeof(Member));
		}
		memset(&lb->ms[lb->nm], 0, sizeof(Member));
		memmove(lb->ms[lb->nm].name, p, 16);
		for(s = lb->ms[lb->nm].name+15; s >= lb->ms[lb->nm].name && (*s == ' ' || *s == '/' || *s == 0); s--)
			*s = 0;
		lb->ms[lb->nm].p = p + SAR_HDR;
		lb->ms[lb->nm].n = size;
		p += SAR_HDR + size + (size & 1);
		if(lb->ms[lb->nm].n >= strlen(OBJMAGIC) && memcmp(lb->ms[lb->nm].p, OBJMAGIC, strlen(OBJMAGIC)) == 0) {
			memberdefs(&lb->ms[lb->nm]);
			lb->nm++;
		}
	}
}

/*
 * the libraries, until none has a member that defines something wanted:
 * all of them each time round, as a library may need one named before it
 * (libthread, then libc again) - and #pragma lib may add libraries as
 * members load
 */
void
ldlibs(void)
{
	Lib *lb;
	int l, i, nread, maxl, change;
	char *s;

	maxl = nlibs + 16;
	lb = emalloc(maxl*sizeof(Lib));
	nread = 0;
	do {
		change = 0;
		for(; nread < nlibs; nread++) {
			if(nread >= maxl) {
				maxl *= 2;
				lb = realloc(lb, maxl*sizeof(Lib));
			}
			readlib(libs[nread], pragmalib[nread], &lb[nread]);
		}
		for(l = 0; l < nread; l++)
			for(i = 0; i < lb[l].nm; i++) {
				if(lb[l].ms[i].loaded || !needed(&lb[l].ms[i]))
					continue;
				lb[l].ms[i].loaded = 1;
				s = smprint("%s(%s)", libs[l], lb[l].ms[i].name);
				loadobj(lb[l].ms[i].p, lb[l].ms[i].n, s);
				change = 1;
			}
	} while(change || nread < nlibs);
}

/*
 * after loading: data into the symbols
 */
void
applydata(void)
{
	Dat *d;
	Sym *s;
	Rel *r;
	union {
		float	f;
		ulong	l;
	} uf;
	union {
		double	d;
		uvlong	v;
	} ud;
	uvlong v;
	int i;

	for(d = dats; d != nil; d = d->link) {
		s = d->sym;
		if(s->data == nil) {
			if(s->size <= 0) {
				diag("%s: data but no size", s->name);
				continue;
			}
			s->data = emalloc(s->size);
		}
		if(d->off < 0 || d->off + d->width > s->size) {
			diag("%s: data at %ld+%d outside its %ld bytes", s->name, d->off, d->width, s->size);
			continue;
		}
		switch(d->val.type) {
		case D_SCONST:
			memmove(s->data + d->off, d->val.sval, d->width);
			break;
		case D_FCONST:
			if(d->width == 4) {
				uf.f = d->val.dval;
				v = uf.l;
			} else {
				ud.d = d->val.dval;
				v = ud.v;
			}
			goto put;
		case D_CONST:
			v = d->val.offset;
		put:
			for(i = 0; i < d->width; i++)
				s->data[d->off + i] = v >> 8*i;
			break;
		case D_ADDR:
			r = emalloc(sizeof(*r));
			r->off = d->off;
			r->width = d->width;
			r->sym = d->val.sym;
			r->add = d->val.offset;
			r->link = s->rel;
			s->rel = r;
			break;
		default:
			diag("%s: bad data", s->name);
		}
	}
}
