#include	"l.h"

/*
 * the module:
 *	memory	[0, GUARD) nothing, [GUARD, stacktop) the stack, then data,
 *		bss, end; the heap from end (brk grows the memory)
 *	globals	SP, RET.w RET.v RET.f RET.d
 *	table	function pointers: index 1 on (0 is nil)
 *	import	plan9.syscall(number i32, args i32) -> i64
 *	tag	0, longjmp's (buf, v): a function calling setjmp catches it
 *	export	memory, sp, _start (calls the entry: -E, default _main; the
 *		kernel puts argc, argv[0] ... nil at sp first)
 * every function () -> ().  Control flow: a function's basic blocks in
 * nested blocks, block j's code after the end of block j: a branch
 * forward is a br out to it; one backward goes round a loop whose
 * br_table picks the block (local 1).
 */

enum
{
	I32	= 0x7f,
	I64	= 0x7e,
	F32	= 0x7d,
	F64	= 0x7c,

	MAXPAGES = 16384,	/* -k: a shared memory has a maximum: 1 GB */
};

static	int	nimport = 1;	/* plan9.syscall, then -k's platform functions */
static	Sym**	imports;

static	long	stacktop;
static	long	database;
static	long	dataend;
static	long	bssend;
static	long	npages;
static	Sym*	setjmpsym;
static	long	asbase;
static	Sym*	entrysym;
static	void	unwindgraph(void);
static	char*	synths[] = { "_trap", "_yield", "longjmp", "gotolabel", "_tas", "ainc", "adec", "cas", "casp", "casl", "coherence", nil };

void
bput1(Buf *b, int c)
{
	if(b->n >= b->max) {
		b->max = b->max ? 2*b->max : 256;
		b->p = realloc(b->p, b->max);
		if(b->p == nil) {
			fprint(2, "3l: out of memory\n");
			exits("memory");
		}
	}
	b->p[b->n++] = c;
}

void
bputn(Buf *b, void *v, long n)
{
	uchar *p;

	p = v;
	while(n-- > 0)
		bput1(b, *p++);
}

void
buleb(Buf *b, uvlong v)
{
	int c;

	do {
		c = v & 0x7f;
		v >>= 7;
		if(v)
			c |= 0x80;
		bput1(b, c);
	} while(v);
}

void
bsleb(Buf *b, vlong v)
{
	int c;

	for(;;) {
		c = v & 0x7f;
		v >>= 7;
		if(v == 0 && (c & 0x40) == 0 || v == -1 && (c & 0x40) != 0) {
			bput1(b, c);
			return;
		}
		bput1(b, c|0x80);
	}
}

static void
bstr(Buf *b, char *s)
{
	buleb(b, strlen(s));
	bputn(b, s, strlen(s));
}

void
bfree(Buf *b)
{
	free(b->p);
	b->p = nil;
	b->n = b->max = 0;
}

static long
rnd(long v, long r)
{
	return (v + r - 1) / r * r;
}

static Sym*
defsym(char *name, long v)
{
	Sym *s;

	s = lookup(name, 0);
	if(s->type == SNONE) {
		s->type = SBSS;
		s->size = 0;
		s->value = v;
		s->file = "3l";
	}
	return s;
}

/*
 * fork: a process's stack is unwound into memory and rewound in the
 * parent and in the child (docs/wasm32.md).  What can be on the stack
 * then: a function that calls _trap, calls through a pointer, or calls
 * one that can be
 */
static int
unwindcall(Prog *p)
{
	Sym *s;

	if(p->as != ACALL || debug['k'])
		return 0;
	if(p->to.type != D_EXTERN && p->to.type != D_STATIC)
		return 1;
	s = p->to.sym;
	if(s->type == STEXT)
		return s->unwind;
	return s->type == SSYNTH && (strcmp(s->name, "_trap") == 0 || strcmp(s->name, "_yield") == 0);
}

/*
 * preemption: rfork(RFMEM) procs take turns on one Worker, and one that
 * only computes would keep the others out.  Before each branch back
 * (a loop): GPREEMPT--, and when it reaches 0 a call of _yield, a block of
 * its own, so that the platform can unwind the proc there and let
 * another go (host3js.c).  A function with a loop becomes unwindable.
 */
static void
preemptpoints(void)
{
	Sym *s, *ys;
	Prog *p, *q, *prev, *q1, *q2;
	int n, i, nloop;

	ys = lookup("_yield", 0);
	nloop = 0;
	if(debug['k'])
		return;		/* a kernel's procs are Workers: the browser preempts them */
	for(s = allsym; s != nil; s = s->next) {
		if(s->type != STEXT)
			continue;
		/* program order: a branch to an earlier one goes back */
		n = 0;
		for(p = s->text->link; p != nil; p = p->link)
			p->blk = n++;
		prev = s->text;
		for(p = s->text->link; p != nil; prev = p, p = p->link) {
			if(p->targ == nil || p->targ->blk > p->blk || p->as == APREEMPT)
				continue;
			q1 = emalloc(sizeof *q1);
			q1->as = APREEMPT;
			q1->lineno = p->lineno;
			q1->targ = p;
			q1->to.type = D_BRANCH;
			q2 = emalloc(sizeof *q2);
			q2->as = ACALL;
			q2->lineno = p->lineno;
			q2->to.type = D_EXTERN;
			q2->to.sym = ys;
			q1->link = q2;
			q2->link = p;
			prev->link = q1;
			q1->blk = q2->blk = p->blk;
			nloop++;
		}
		USED(q);
		USED(i);
	}
	if(nloop > 0)
		ys->ref = 1;
	if(debug['v'])
		fprint(2, "3l: %d preemption points\n", nloop);
}

static void
unwindgraph(void)
{
	int i, change, n;
	Prog *p;
	Sym *s;

	do {
		change = 0;
		for(i = 0; i < nfuncs; i++) {
			s = funcs[i];
			if(s->type != STEXT || s->unwind)
				continue;
			for(p = s->text->link; p != nil; p = p->link)
				if(unwindcall(p)) {
					s->unwind = 1;
					change = 1;
					break;
				}
		}
	} while(change);
	if(debug['v']) {
		n = 0;
		for(i = 0; i < nfuncs; i++)
			n += funcs[i]->unwind;
		fprint(2, "3l: %d of %d functions can be unwound\n", n, nfuncs);
	}
}

/* -k: the platform's functions are plat*, eia*: anything else undefined is a missing function */
static int
platformname(char *n)
{
	return strncmp(n, "plat", 4) == 0 || strncmp(n, "eia", 3) == 0;
}

void
layout(void)
{
	Sym *s;
	long a, al;
	int n, i;

	applydata();
	preemptpoints();

	/* functions: defined ones, then _trap if wanted */
	/* made here when used and not defined: _trap, longjmp, the atomics */
	for(i = 0; synths[i] != nil; i++) {
		s = lookup(synths[i], 0);
		if(s->ref && s->type == SNONE)
			s->type = SSYNTH;
	}
	/* setjmp, and the kernel's setlabel: in place at each call (function) */
	setjmpsym = lookup("setjmp", 0);
	if(setjmpsym->type == SNONE)
		setjmpsym->type = SINLINE;
	s = lookup("setlabel", 0);
	if(s->type == SNONE)
		s->type = SINLINE;
	/* -k: what it calls and does not define, the platform's */
	if(debug['k']) {
		Sym *f;
		Prog *p;

		n = 0;
		for(f = allsym; f != nil; f = f->next) {
			if(f->type != STEXT)
				continue;
			for(p = f->text->link; p != nil; p = p->link)
				if(p->as == ACALL && (p->to.type == D_EXTERN || p->to.type == D_STATIC) &&
				   p->to.sym->type == SNONE && platformname(p->to.sym->name)) {
					p->to.sym->type = SIMPORT;
					n++;
				}
		}
		imports = emalloc((n+1)*sizeof(Sym*));
		for(s = allsym; s != nil; s = s->next)
			if(s->type == SIMPORT) {
				s->fn = nimport;
				imports[nimport-1] = s;
				nimport++;
			}
	}
	n = 0;
	for(s = allsym; s != nil; s = s->next)
		if(s->type == STEXT || s->type == SSYNTH)
			n++;
	funcs = emalloc((n+1)*sizeof(Sym*));
	for(s = allsym; s != nil; s = s->next)
		if(s->type == STEXT || s->type == SSYNTH) {
			s->fn = nimport + nfuncs;
			s->tab = nfuncs + 1;
			funcs[nfuncs++] = s;
		}
	entrysym = lookup(entry, 0);
	if(entrysym->type != STEXT)
		diag("entry %s not defined", entry);
	unwindgraph();

	/* data, then bss */
	stacktop = rnd(GUARD + stacksize, 16);
	a = stacktop;
	database = a;
	for(s = allsym; s != nil; s = s->next) {
		if(s->type != SDATA)
			continue;
		al = s->size >= 8 ? 8 : s->size >= 4 ? 4 : s->size >= 2 ? 2 : 1;
		a = rnd(a, al);
		s->value = a;
		a += s->size;
	}
	dataend = a;
	for(s = allsym; s != nil; s = s->next) {
		if(s->type != SBSS)
			continue;
		al = s->size >= 8 ? 8 : s->size >= 4 ? 4 : s->size >= 2 ? 2 : 1;
		a = rnd(a, al);
		s->value = a;
		a += s->size;
	}
	a = rnd(a, 8);
	asbase = a;	/* the saved frames, for fork */
	a += ASAREA;
	bssend = rnd(a, 8);
	defsym("edata", dataend);
	defsym("end", bssend);
	defsym("etext", 0);
	npages = (bssend + PAGE - 1) / PAGE + 1;

	for(s = allsym; s != nil; s = s->next)
		if(s->ref && s->type == SNONE)
			diag("undefined: %s", s->name);
	if(debug['v'])
		fprint(2, "3l: %d functions, stack %ld-%ld, data %ld-%ld, bss -%ld, %ld pages\n",
			nfuncs, (long)GUARD, stacktop, database, dataend, bssend, npages);
}

/*
 * a function
 */
static	Buf	code;
static	long	frame;
static	int	locbase[NK];
static	int	nloc[NK];
static	int	nblk;
static	int	hasloop;
static	int	sjlj;		/* calls setjmp: in a try, longjmp's catch */
static	int	unwind;		/* this function can be unwound (fork) */
static	long	recsize;	/* its saved frame */
static	long	recoff[4+1024];	/* where each local is in it */
static	int	nlocals;
static	Sym*	cursym;

static void
op(int c)
{
	bput1(&code, c);
}

static void
op2(int c, int d)
{
	bput1(&code, c);
	buleb(&code, d);
}

static void
local(int set, int i)
{
	op2(set ? 0x21 : 0x20, i);
}

static int
regidx(long r)
{
	int k, n;

	k = RCLASS(r);
	n = RNUM(r);
	if(n == NREGSP)
		return 0;
	if(n < NREGFIRST) {
		diag("%s: bad register %ld", cursym->name, r);
		return 0;
	}
	return locbase[k] + n - NREGFIRST;
}

static int
isext(long r)
{
	return RNUM(r) >= NREGEXT && RNUM(r) < NREGEXT+2;
}

/* a register's value: a local, or extern register's global */
static void
getreg(long r)
{
	if(isext(r))
		op2(0x23, GEXT + RNUM(r) - NREGEXT);
	else
		local(0, regidx(r));
}

static long
symaddr(Sym *s)
{
	switch(s->type) {
	case STEXT:
	case SSYNTH:
		return s->tab;
	case SDATA:
	case SBSS:
		return s->value;
	case SINLINE:
		diag("%s: the address of %s, which is made in place", cursym->name, s->name);
		return 0;
	case SIMPORT:
		diag("%s: the address of %s, the platform's", cursym->name, s->name);
		return 0;
	}
	diag("%s: undefined %s", cursym->name, s->name);
	return 0;
}

static void
iconst(vlong v)
{
	op(0x41);
	bsleb(&code, (long)v);
}

static void
fconst(int k, double d)
{
	union {
		float	f;
		uchar	b[4];
	} uf;
	union {
		double	d;
		uchar	b[8];
	} ud;

	if(k == Kf) {
		op(0x43);
		uf.f = d;
		bputn(&code, uf.b, 4);
	} else {
		op(0x44);
		ud.d = d;
		bputn(&code, ud.b, 8);
	}
}

/*
 * the address of a: as a value
 */
static void
pushaddr(Adr *a)
{
	switch(a->index) {
	case D_EXTERN:
	case D_STATIC:
		if(a->sym == nil) {
			diag("%s: address without a symbol", cursym->name);
			iconst(0);
			break;
		}
		iconst(symaddr(a->sym) + a->offset);
		break;
	case D_AUTO:
	case D_PARAM:
		local(0, 0);
		iconst(frame + a->offset);
		op(0x6a);
		break;
	case D_SP:
		local(0, 0);
		iconst(a->offset);
		op(0x6a);
		break;
	case D_OREG:
		getreg(a->reg);
		iconst(a->offset);
		op(0x6a);
		break;
	default:
		diag("%s: bad address type %d", cursym->name, a->index);
	}
}

/*
 * a value of class k onto the stack
 */
static void
push(Adr *a, int k)
{
	switch(a->type) {
	case D_REG:
		if(RCLASS(a->reg) != k && RNUM(a->reg) != NREGSP)
			diag("%s: register class %ld, not %d", cursym->name, RCLASS(a->reg), k);
		if(RNUM(a->reg) == NREGRET)
			op2(0x23, GRET + RCLASS(a->reg));
		else
			getreg(a->reg);
		break;
	case D_CONST:
		switch(k) {
		case Kw:
			iconst(a->offset);
			break;
		case Kv:
			op(0x42);
			bsleb(&code, a->offset);
			break;
		default:
			fconst(k, (double)a->offset);
		}
		break;
	case D_FCONST:
		switch(k) {
		case Kw:
			iconst((vlong)a->dval);
			break;
		case Kv:
			op(0x42);
			bsleb(&code, (vlong)a->dval);
			break;
		default:
			fconst(k, a->dval);
		}
		break;
	case D_ADDR:
		pushaddr(a);
		break;
	default:
		diag("%s: bad operand type %d", cursym->name, a->type);
	}
}

static void
setreg(Adr *a, int k)
{
	if(a->type != D_REG) {
		diag("%s: result to type %d", cursym->name, a->type);
		op(0x1a);	/* drop */
		return;
	}
	if(RCLASS(a->reg) != k && RNUM(a->reg) != NREGSP)
		diag("%s: register class %ld, not %d", cursym->name, RCLASS(a->reg), k);
	if(RNUM(a->reg) == NREGRET)
		op2(0x24, GRET + RCLASS(a->reg));
	else if(isext(a->reg))
		op2(0x24, GEXT + RNUM(a->reg) - NREGEXT);
	else
		local(1, regidx(a->reg));
}

static int
ismem(Adr *a)
{
	switch(a->type) {
	case D_EXTERN:
	case D_STATIC:
	case D_AUTO:
	case D_PARAM:
	case D_SP:
	case D_OREG:
		return 1;
	}
	return 0;
}

/*
 * memory at a: the base onto the stack, the offset returned
 */
static ulong
memaddr(Adr *a)
{
	long off;

	switch(a->type) {
	case D_EXTERN:
	case D_STATIC:
		iconst(0);
		return symaddr(a->sym) + a->offset;
	case D_AUTO:
	case D_PARAM:
		local(0, 0);
		off = frame + a->offset;
		if(off < 0)
			diag("%s: frame offset %ld", cursym->name, off);
		return off;
	case D_SP:
		local(0, 0);
		return a->offset;
	case D_OREG:
		getreg(a->reg);
		if(a->offset < 0 || a->offset > 0x7fffffff) {
			iconst(a->offset);
			op(0x6a);
			return 0;
		}
		return a->offset;
	}
	diag("%s: not memory", cursym->name);
	return 0;
}

static void
memarg(int al, ulong off)
{
	buleb(&code, al);
	buleb(&code, off);
}

static int
movclass(int as)
{
	switch(as) {
	case AMOVV:	return Kv;
	case AMOVF:	return Kf;
	case AMOVD:	return Kd;
	}
	return Kw;
}

static void
emov(Prog *p)
{
	int as, k;
	ulong off, toff;
	static uchar load[] = { 0x2c, 0x2d, 0x2e, 0x2f, 0x28, 0x29, 0x2a, 0x2b };
	static uchar store[] = { 0x3a, 0x3a, 0x3b, 0x3b, 0x36, 0x37, 0x38, 0x39 };
	static uchar al[] = { 0, 0, 1, 1, 2, 3, 2, 3 };
	int i;

	as = p->as;
	i = as - AMOVB;
	k = movclass(as);
	toff = 0;
	if(ismem(&p->to))
		toff = memaddr(&p->to);
	if(ismem(&p->from)) {
		off = memaddr(&p->from);
		op(load[i]);
		memarg(al[i], off);
	} else {
		push(&p->from, k);
		if(p->from.type == D_REG)
			switch(as) {
			case AMOVB:	op(0xc0); break;		/* i32.extend8_s */
			case AMOVBU:	iconst(0xff); op(0x71); break;
			case AMOVH:	op(0xc1); break;		/* i32.extend16_s */
			case AMOVHU:	iconst(0xffff); op(0x71); break;
			}
	}
	if(ismem(&p->to)) {
		op(store[i]);
		memarg(al[i], toff);
	} else
		setreg(&p->to, k);
}

static void
econv(Prog *p)
{
	int f, t, u;

	f = p->k2;
	t = p->k;
	u = p->as == ACVTU;
	push(&p->from, f);
	switch(f*NK + t) {
	case Kw*NK+Kv:	op(u ? 0xad : 0xac); break;
	case Kv*NK+Kw:	op(0xa7); break;
	case Kw*NK+Kf:	op(u ? 0xb3 : 0xb2); break;
	case Kv*NK+Kf:	op(u ? 0xb5 : 0xb4); break;
	case Kw*NK+Kd:	op(u ? 0xb8 : 0xb7); break;
	case Kv*NK+Kd:	op(u ? 0xba : 0xb9); break;
	case Kf*NK+Kw:	op2(0xfc, u ? 1 : 0); break;	/* trunc_sat */
	case Kd*NK+Kw:	op2(0xfc, u ? 3 : 2); break;
	case Kf*NK+Kv:	op2(0xfc, u ? 5 : 4); break;
	case Kd*NK+Kv:	op2(0xfc, u ? 7 : 6); break;
	case Kf*NK+Kd:	op(0xbb); break;
	case Kd*NK+Kf:	op(0xb6); break;
	default:
		diag("%s: bad conversion %d to %d", cursym->name, f, t);
	}
	setreg(&p->to, t);
}

static int
arith(int as, int k)
{
	static uchar w[] = { 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76 };
	static uchar v[] = { 0x7c, 0x7d, 0x7e, 0x7f, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88 };
	int i;

	i = as - AADD;
	switch(k) {
	case Kw:
		return w[i];
	case Kv:
		return v[i];
	}
	switch(as) {
	case AADD:	return k == Kf ? 0x92 : 0xa0;
	case ASUB:	return k == Kf ? 0x93 : 0xa1;
	case AMUL:	return k == Kf ? 0x94 : 0xa2;
	case ADIV:
	case ADIVU:	return k == Kf ? 0x95 : 0xa3;
	}
	diag("%s: no float %d", cursym->name, as);
	return 0x00;
}

static int
compare(int as, int k)
{
	/* EQ NE LT LE GT GE LO LS HI HS */
	static uchar w[] = { 0x46, 0x47, 0x48, 0x4c, 0x4a, 0x4e, 0x49, 0x4d, 0x4b, 0x4f };
	static uchar v[] = { 0x51, 0x52, 0x53, 0x57, 0x55, 0x59, 0x54, 0x58, 0x56, 0x5a };
	static uchar f[] = { 0x5b, 0x5c, 0x5d, 0x5f, 0x5e, 0x60, 0x5d, 0x5f, 0x5e, 0x60 };
	static uchar d[] = { 0x61, 0x62, 0x63, 0x65, 0x64, 0x66, 0x63, 0x65, 0x64, 0x66 };
	int i;

	i = as - ACMPEQ;
	switch(k) {
	case Kw:	return w[i];
	case Kv:	return v[i];
	case Kf:	return f[i];
	}
	return d[i];
}

static void
epilogue(void)
{
	if(frame) {
		local(0, 0);
		iconst(frame);
		op(0x6a);
		op2(0x24, GSP);
	}
	op(0x0f);	/* return */
}

static void
branch(Prog *p, int cond)
{
	int j, t;

	j = p->blk;
	t = p->targ->blk;
	if(t > j) {
		op2(cond ? 0x0d : 0x0c, t - j - 1);
		return;
	}
	if(cond)
		op2(0x04, 0x40);	/* if */
	iconst(t);
	local(1, 1);
	op2(0x0c, nblk - 1 - j + cond + sjlj);
	if(cond)
		op(0x0b);
}

/*
 * a frame saved: the block of the call (it is called again when
 * rewound), then every local; locals by class as declared
 */
static int
localclass(int i)
{
	if(i < locbase[Kv])
		return Kw;
	if(i < locbase[Kf])
		return Kv;
	if(i < locbase[Kd])
		return Kf;
	return Kd;
}

static void
framerec(void)
{
	long o;
	int i, k;
	static int w[NK] = { 4, 8, 4, 8 };

	nlocals = locbase[Kd] + nloc[Kd];
	if(nlocals > nelem(recoff)) {
		diag("%s: too many registers to save", cursym->name);
		nlocals = nelem(recoff);
	}
	o = 4;
	for(i = 0; i < nlocals; i++) {
		k = localclass(i);
		o = rnd(o, w[k]);
		recoff[i] = o;
		o += w[k];
	}
	/* then the number of i32 locals and the size: the kernel walks them (rfork RFMEM) */
	recsize = rnd(o + 8, 8);
}

static void
saveframe(int blk)
{
	int i;
	static uchar store[NK] = { 0x36, 0x37, 0x38, 0x39 };
	static uchar al[NK] = { 2, 3, 2, 3 };

	/* if unwinding: push this frame, return */
	op2(0x23, GSTATE);
	iconst(1);
	op(0x46);
	op2(0x04, 0x40);
	op2(0x23, GASPTR);
	iconst(blk);
	op(0x36);
	memarg(2, 0);
	for(i = 0; i < nlocals; i++) {
		op2(0x23, GASPTR);
		local(0, i);
		op(store[localclass(i)]);
		memarg(al[localclass(i)], recoff[i]);
	}
	op2(0x23, GASPTR);
	iconst(locbase[Kv]);
	op(0x36);
	memarg(2, recsize-8);
	op2(0x23, GASPTR);
	iconst(recsize);
	op(0x36);
	memarg(2, recsize-4);
	op2(0x23, GASPTR);
	iconst(recsize);
	op(0x6a);
	op2(0x24, GASPTR);
	op(0x0f);
	op(0x0b);
}

static void
restoreframe(void)
{
	int i;
	static uchar load[NK] = { 0x28, 0x29, 0x2a, 0x2b };
	static uchar al[NK] = { 2, 3, 2, 3 };

	/* pop this frame; on to its call's block */
	op2(0x23, GASPTR);
	iconst(recsize);
	op(0x6b);
	op2(0x24, GASPTR);
	for(i = 0; i < nlocals; i++) {
		op2(0x23, GASPTR);
		op(load[localclass(i)]);
		memarg(al[localclass(i)], recoff[i]);
		local(1, i);
	}
	op2(0x23, GASPTR);
	op(0x28);
	memarg(2, 0);
	local(1, 1);
	local(0, 0);
	op2(0x24, GSP);
}

static void
eprog(Prog *p)
{
	switch(p->as) {
	case ANOP:
		break;

	case AMOVB:
	case AMOVBU:
	case AMOVH:
	case AMOVHU:
	case AMOVW:
	case AMOVV:
	case AMOVF:
	case AMOVD:
		emov(p);
		break;

	case ACVT:
	case ACVTU:
		econv(p);
		break;

	case AADD:
	case ASUB:
	case AMUL:
	case ADIV:
	case ADIVU:
	case AMOD:
	case AMODU:
	case AAND:
	case AOR:
	case AXOR:
	case ASHL:
	case ASHR:
	case ASHRU:
		push(&p->from, p->k);
		push(&p->from2, p->k);
		op(arith(p->as, p->k));
		setreg(&p->to, p->k);
		break;

	case ACMPEQ:
	case ACMPNE:
	case ACMPLT:
	case ACMPLE:
	case ACMPGT:
	case ACMPGE:
	case ACMPLO:
	case ACMPLS:
	case ACMPHI:
	case ACMPHS:
		push(&p->from, p->k);
		push(&p->from2, p->k);
		op(compare(p->as, p->k));
		setreg(&p->to, Kw);
		break;

	case AJMP:
		branch(p, 0);
		break;

	case APREEMPT:
		/* GPREEMPT--; on (past the _yield) while it is above 0 */
		op2(0x23, GPREEMPT);
		iconst(1);
		op(0x6b);
		op2(0x22, 2);		/* local.tee: a scratch i32 */
		op2(0x24, GPREEMPT);
		local(0, 2);
		iconst(0);
		op(0x4a);		/* i32.gt_s */
		branch(p, 1);
		break;

	case ABNZ:
	case ABZ:
		push(&p->from, Kw);
		if(p->as == ABZ)
			op(0x45);	/* i32.eqz */
		branch(p, 1);
		break;

	case ACALL:
		if(p->to.type == D_EXTERN || p->to.type == D_STATIC) {
			Sym *s;

			s = p->to.sym;
			if(s->type == SINLINE) {
				/*
				 * setjmp(buf): buf[0] this frame's SP, buf[1] the block
				 * after the call (longjmp's catch goes there); 0
				 */
				local(0, 0);
				op(0x28);
				memarg(2, 0);
				op2(0x22, 2);		/* local.tee: the buf */
				local(0, 0);
				op(0x36);
				memarg(2, 0);
				local(0, 2);
				iconst(p->link != nil ? p->link->blk : 0);
				op(0x36);
				memarg(2, 4);
				iconst(0);
				op2(0x24, GRET+Kw);
				break;
			}
			if(s->type != STEXT && s->type != SSYNTH && s->type != SIMPORT) {
				diag("%s: call of %s, not a function", cursym->name, s->name);
				break;
			}
			op2(0x10, s->fn);
			if(unwind && unwindcall(p))
				saveframe(p->blk);
			break;
		}
		push(&p->to, Kw);
		op2(0x11, 0);	/* call_indirect type 0 */
		buleb(&code, 0);	/* table 0 */
		if(unwind)
			saveframe(p->blk);
		break;

	case ARET:
		epilogue();
		break;

	case ACOPY:
		push(&p->to, Kw);
		push(&p->from, Kw);
		push(&p->from2, Kw);
		op2(0xfc, 10);		/* memory.copy */
		buleb(&code, 0);
		buleb(&code, 0);
		break;

	default:
		diag("%s: unknown instruction %d", cursym->name, p->as);
	}
}

static void
scanreg(Adr *a)
{
	int k, n;

	if(a->type != D_REG && a->type != D_OREG && !(a->type == D_ADDR && a->index == D_OREG))
		return;
	k = RCLASS(a->reg);
	n = RNUM(a->reg);
	if(n >= NREGFIRST && n - NREGFIRST + 1 > nloc[k])
		nloc[k] = n - NREGFIRST + 1;
}

static int
isbranch(int as)
{
	return as == AJMP || as == ABNZ || as == ABZ || as == ARET || as == APREEMPT;
}

/*
 * a function made here: its arguments at SP, as any function's
 */
static void
argw(int i)
{
	op2(0x23, GSP);
	op(0x28);
	memarg(2, 4*i);
}

static void
atomic(int c)
{
	op2(0xfe, c);
	memarg(2, 0);
}

static void
synth(Sym *s, Buf *b)
{
	char *n;

	n = s->name;
	if(strcmp(n, "_trap") == 0) {
		/*
		 * _trap(n, args): plan9.syscall(n, args), the result in RET;
		 * rewound (fork), the result is asret and the rewinding ends
		 */
		buleb(b, 1);
		buleb(b, 1);
		bput1(b, I64);
		op2(0x23, GSTATE);
		iconst(2);
		op(0x46);
		op2(0x04, 0x40);
		iconst(0);
		op2(0x24, GSTATE);
		op2(0x23, GASRET);
		op2(0x24, GRET+Kv);
		op2(0x23, GASRET);
		op(0xa7);
		op2(0x24, GRET+Kw);
		op(0x0f);
		op(0x0b);
		argw(0);
		argw(1);
		op2(0x10, 0);
		op2(0x22, 0);		/* local.tee */
		op2(0x24, GRET+Kv);
		local(0, 0);
		op(0xa7);
		op2(0x24, GRET+Kw);
	} else if(strcmp(n, "_yield") == 0) {
		/* a preemption point: the platform may unwind here; rewound, it ends the rewinding */
		buleb(b, 0);
		op2(0x23, GSTATE);
		iconst(2);
		op(0x46);
		op2(0x04, 0x40);
		iconst(0);
		op2(0x24, GSTATE);
		op(0x0f);
		op(0x0b);
		iconst(105);
		iconst(0);
		op2(0x10, 0);
		op(0x1a);		/* drop */
	} else {
		buleb(b, 0);
		if(strcmp(n, "longjmp") == 0) {
			/* longjmp(buf, v): thrown to the frame of buf's setjmp */
			argw(0);
			argw(1);
			op2(0x08, 0);
		} else if(strcmp(n, "gotolabel") == 0) {
			/* the kernel's: setlabel's place gets 1 */
			argw(0);
			iconst(1);
			op2(0x08, 0);
		} else if(strcmp(n, "_tas") == 0) {
			argw(0);
			iconst(0xdeadead);
			atomic(0x41);		/* i32.atomic.rmw.xchg */
			op2(0x24, GRET+Kw);
		} else if(strcmp(n, "ainc") == 0 || strcmp(n, "adec") == 0) {
			argw(0);
			iconst(1);
			atomic(n[0] == 'a' && n[1] == 'i' ? 0x1e : 0x25);	/* rmw.add, rmw.sub */
			iconst(1);
			op(n[1] == 'i' ? 0x6a : 0x6b);
			op2(0x24, GRET+Kw);
		} else if(strcmp(n, "coherence") == 0) {
			op2(0xfe, 0x03);	/* atomic.fence */
			bput1(&code, 0);
		} else {
			/* cas, casp, casl(p, ov, nv): 1 if *p was ov and is now nv */
			argw(0);
			argw(1);
			argw(2);
			atomic(0x48);		/* i32.atomic.rmw.cmpxchg */
			argw(1);
			op(0x46);
			op2(0x24, GRET+Kw);
		}
	}
	op(0x0b);
	bputn(b, code.p, code.n);
}

/*
 * a function's body (locals and code) into b
 */
static void
function(Sym *s, Buf *b)
{
	Prog *p;
	int k, i, nl;
	static int types[NK] = { I32, I64, F32, F64 };

	cursym = s;
	code.n = 0;
	if(s->type == SSYNTH) {
		synth(s, b);
		return;
	}
	frame = rnd(s->text->to.offset, 8);
	memset(nloc, 0, sizeof nloc);
	nblk = 0;
	hasloop = 0;
	sjlj = 0;
	unwind = s->unwind;
	for(p = s->text->link; p != nil; p = p->link) {
		scanreg(&p->from);
		scanreg(&p->from2);
		scanreg(&p->to);
		if(p->targ != nil)
			p->targ->leader = 1;
		if(isbranch(p->as) && p->link != nil)
			p->link->leader = 1;
		if(p->as == ACALL && p->to.sym != nil && p->to.sym->type == SINLINE) {
			sjlj = 1;
			if(p->link != nil)
				p->link->leader = 1;
		}
		if(unwind && unwindcall(p)) {
			/* called again when rewound: a block of its own */
			p->leader = 1;
			if(p->link != nil)
				p->link->leader = 1;
		}
	}
	if(s->text->link != nil)
		s->text->link->leader = 1;
	for(p = s->text->link; p != nil; p = p->link) {
		if(p->leader)
			nblk++;
		p->blk = nblk-1;
	}
	for(p = s->text->link; p != nil; p = p->link)
		if(p->targ != nil && p->targ->blk <= p->blk)
			hasloop = 1;
	if(sjlj || unwind)
		hasloop = 1;

	/* locals: SP, the block, longjmp's buf and value, then the registers by class */
	nl = 0;
	for(k = 0; k < NK; k++)
		if(nloc[k] || k == Kw)
			nl++;
	buleb(b, nl);
	for(k = 0; k < NK; k++) {
		if(k == Kw) {
			buleb(b, 4 + nloc[Kw]);
			bput1(b, I32);
			continue;
		}
		if(nloc[k]) {
			buleb(b, nloc[k]);
			bput1(b, types[k]);
		}
	}
	locbase[Kw] = 4;
	locbase[Kv] = 4 + nloc[Kw];
	locbase[Kf] = locbase[Kv] + nloc[Kv];
	locbase[Kd] = locbase[Kf] + nloc[Kf];
	if(unwind)
		framerec();

	/* SP; or, rewinding, the saved frame */
	if(unwind) {
		op2(0x23, GSTATE);
		iconst(2);
		op(0x46);
		op2(0x04, 0x40);
		restoreframe();
		op(0x05);	/* else */
	}
	op2(0x23, GSP);
	if(frame) {
		iconst(frame);
		op(0x6b);
		op2(0x22, 0);
		op2(0x24, GSP);
	} else
		local(1, 0);
	if(unwind)
		op(0x0b);

	if(nblk > 1 || sjlj || unwind) {
		if(hasloop)
			op2(0x03, 0x40);	/* loop */
		if(sjlj)
			op2(0x06, 0x40);	/* try */
		for(i = nblk-1; i >= 1; i--)
			op2(0x02, 0x40);	/* block */
		if(hasloop) {
			op2(0x02, 0x40);
			local(0, 1);
			op(0x0e);		/* br_table */
			buleb(&code, nblk);
			for(i = 0; i < nblk; i++)
				buleb(&code, i);
			buleb(&code, 0);
			op(0x0b);
		}
	}
	for(p = s->text->link; p != nil; p = p->link) {
		if(p->leader && p->blk > 0)
			op(0x0b);	/* end of block blk: its code */
		eprog(p);
	}
	if(sjlj) {
		/*
		 * longjmp(buf, v) threw (buf, v): this frame's if buf[0] is its
		 * SP - the global SP back, v (or 1) the result, round the loop
		 * to buf[1]'s block; else on up
		 */
		op2(0x07, 0);		/* catch tag 0 */
		local(1, 3);
		local(1, 2);
		local(0, 2);
		op(0x28);
		memarg(2, 0);
		local(0, 0);
		op(0x46);
		op2(0x04, 0x40);	/* if */
		local(0, 0);
		op2(0x24, GSP);
		local(0, 3);
		iconst(1);
		local(0, 3);
		op(0x1b);		/* select: v ? v : 1 */
		op2(0x24, GRET+Kw);
		local(0, 2);
		op(0x28);
		memarg(2, 4);
		local(1, 1);
		op2(0x0c, 2);		/* br: the loop */
		op(0x0b);
		op2(0x09, 0);		/* rethrow */
		op(0x0b);		/* end try */
	}
	if((nblk > 1 || sjlj || unwind) && hasloop)
		op(0x0b);
	op(0x0b);
	bputn(b, code.p, code.n);
}

static void
section(Buf *out, int id, Buf *b)
{
	bput1(out, id);
	buleb(out, b->n);
	bputn(out, b->p, b->n);
	bfree(b);
}

void
asmb(void)
{
	Buf out, b, f;
	Sym *s;
	Rel *r;
	uchar *mem;
	long v;
	int i, fd;

	memset(&out, 0, sizeof out);
	memset(&b, 0, sizeof b);
	memset(&f, 0, sizeof f);
	bputn(&out, "\0asm\1\0\0\0", 8);

	/* types: 0 () -> (), 1 (i32 i32) -> i64, 2 (i32 i32) -> () longjmp's tag */
	buleb(&b, 3);
	bput1(&b, 0x60); buleb(&b, 0); buleb(&b, 0);
	bput1(&b, 0x60); buleb(&b, 2); bput1(&b, I32); bput1(&b, I32); buleb(&b, 1); bput1(&b, I64);
	bput1(&b, 0x60); buleb(&b, 2); bput1(&b, I32); bput1(&b, I32); buleb(&b, 0);
	section(&out, 1, &b);

	/* imports: plan9.syscall; -k the platform's functions and the memory */
	buleb(&b, nimport + (debug['k'] ? 1 : 0));
	bstr(&b, "plan9");
	bstr(&b, "syscall");
	bput1(&b, 0x00);
	buleb(&b, 1);
	for(i = 0; i < nimport-1; i++) {
		bstr(&b, "platform");
		bstr(&b, imports[i]->name);
		bput1(&b, 0x00);
		buleb(&b, 0);
	}
	if(debug['k']) {
		bstr(&b, "platform");
		bstr(&b, "memory");
		bput1(&b, 0x02);
		bput1(&b, 0x03);	/* shared, with a maximum */
		buleb(&b, npages);
		buleb(&b, MAXPAGES);
	}
	section(&out, 2, &b);

	/* functions, and _start (and -k's _init) */
	buleb(&b, nfuncs+1+(debug['k']?1:0));
	for(i = 0; i <= nfuncs+(debug['k']?1:0); i++)
		buleb(&b, 0);
	section(&out, 3, &b);

	/* table */
	buleb(&b, 1);
	bput1(&b, 0x70);
	bput1(&b, 0x00);
	buleb(&b, nfuncs+1);
	section(&out, 4, &b);

	/* memory: -k imports it */
	if(!debug['k']) {
		buleb(&b, 1);
		bput1(&b, 0x00);
		buleb(&b, npages);
		section(&out, 5, &b);
	}

	/* tags: 0 longjmp's (buf, v) */
	buleb(&b, 1);
	bput1(&b, 0x00);
	buleb(&b, 2);
	section(&out, 13, &b);

	/* globals: SP, RET.w RET.v RET.f RET.d, asstate asptr asret; constants: stacktop asbase perproc perprocsize; preempt; ext0 ext1 */
	buleb(&b, 15);
	bput1(&b, I32); bput1(&b, 1); bput1(&b, 0x41); bsleb(&b, stacktop); bput1(&b, 0x0b);
	bput1(&b, I32); bput1(&b, 1); bput1(&b, 0x41); bsleb(&b, 0); bput1(&b, 0x0b);
	bput1(&b, I64); bput1(&b, 1); bput1(&b, 0x42); bsleb(&b, 0); bput1(&b, 0x0b);
	bput1(&b, F32); bput1(&b, 1); bput1(&b, 0x43); bputn(&b, "\0\0\0\0", 4); bput1(&b, 0x0b);
	bput1(&b, F64); bput1(&b, 1); bput1(&b, 0x44); bputn(&b, "\0\0\0\0\0\0\0\0", 8); bput1(&b, 0x0b);
	bput1(&b, I32); bput1(&b, 1); bput1(&b, 0x41); bsleb(&b, 0); bput1(&b, 0x0b);
	bput1(&b, I32); bput1(&b, 1); bput1(&b, 0x41); bsleb(&b, asbase); bput1(&b, 0x0b);
	bput1(&b, I64); bput1(&b, 1); bput1(&b, 0x42); bsleb(&b, 0); bput1(&b, 0x0b);
	bput1(&b, I32); bput1(&b, 0); bput1(&b, 0x41); bsleb(&b, stacktop); bput1(&b, 0x0b);
	bput1(&b, I32); bput1(&b, 0); bput1(&b, 0x41); bsleb(&b, asbase); bput1(&b, 0x0b);
	{
		Sym *pp;

		pp = lookup("_perproc", 0);
		bput1(&b, I32); bput1(&b, 0); bput1(&b, 0x41); bsleb(&b, pp->type == SDATA || pp->type == SBSS ? pp->value : 0); bput1(&b, 0x0b);
		bput1(&b, I32); bput1(&b, 0); bput1(&b, 0x41); bsleb(&b, pp->type == SDATA || pp->type == SBSS ? pp->size : 0); bput1(&b, 0x0b);
	}
	bput1(&b, I32); bput1(&b, 1); bput1(&b, 0x41); bsleb(&b, 0x3fffffff); bput1(&b, 0x0b);	/* preempt: the platform sets it */
	bput1(&b, I32); bput1(&b, 1); bput1(&b, 0x41); bsleb(&b, 0); bput1(&b, 0x0b);	/* extern register 2 */
	bput1(&b, I32); bput1(&b, 1); bput1(&b, 0x41); bsleb(&b, 0); bput1(&b, 0x0b);	/* extern register 3 */
	section(&out, 6, &b);

	/*
	 * exports: the kernel puts argc and argv below sp, then calls _start;
	 * fork: asstate, asptr, asret
	 */
	buleb(&b, 12 + (debug['k'] ? 3 : 0));
	if(debug['k']) {
		/* the platform's functions return through RET; _init puts the data in memory */
		bstr(&b, "retw");
		bput1(&b, 0x03);
		buleb(&b, GRET+Kw);
		bstr(&b, "retv");
		bput1(&b, 0x03);
		buleb(&b, GRET+Kv);
		bstr(&b, "_init");
		bput1(&b, 0x00);
		buleb(&b, nimport + nfuncs + 1);
	}
	bstr(&b, "preempt");	/* back edges until _yield */
	bput1(&b, 0x03);
	buleb(&b, GPREEMPT);
	bstr(&b, "perproc");	/* libc's per-proc region (_perproc): the kernel swaps it (rfork RFMEM) */
	bput1(&b, 0x03);
	buleb(&b, GASRET+3);
	bstr(&b, "perprocsize");
	bput1(&b, 0x03);
	buleb(&b, GASRET+4);
	bstr(&b, "table");	/* function pointers, for the kernel's contexts (_ctxnew) */
	bput1(&b, 0x01);
	buleb(&b, 0);
	{
		static char *cn[] = { "stacktop", "asbase" };

		for(i = 0; i < 2; i++) {
			bstr(&b, cn[i]);
			bput1(&b, 0x03);
			buleb(&b, GASRET+1+i);
		}
	}
	bstr(&b, "asstate");
	bput1(&b, 0x03);
	buleb(&b, GSTATE);
	bstr(&b, "asptr");
	bput1(&b, 0x03);
	buleb(&b, GASPTR);
	bstr(&b, "asret");
	bput1(&b, 0x03);
	buleb(&b, GASRET);
	bstr(&b, "memory");
	bput1(&b, 0x02);
	buleb(&b, 0);
	bstr(&b, "sp");
	bput1(&b, 0x03);
	buleb(&b, GSP);
	bstr(&b, "_start");
	bput1(&b, 0x00);
	buleb(&b, nimport + nfuncs);
	section(&out, 7, &b);

	/* elements: the table from 1 */
	buleb(&b, 1);
	buleb(&b, 0);
	bput1(&b, 0x41); bsleb(&b, 1); bput1(&b, 0x0b);
	buleb(&b, nfuncs);
	for(i = 0; i < nfuncs; i++)
		buleb(&b, funcs[i]->fn);
	section(&out, 9, &b);

	/* -k: one passive data segment (its count before the code) */
	if(debug['k']) {
		buleb(&b, 1);
		section(&out, 12, &b);
	}

	/* code */
	buleb(&b, nfuncs+1+(debug['k']?1:0));
	for(i = 0; i < nfuncs; i++) {
		function(funcs[i], &f);
		buleb(&b, f.n);
		bputn(&b, f.p, f.n);
		f.n = 0;
	}
	/* _start */
	f.n = 0;
	buleb(&f, 0);
	if(entrysym->type == STEXT) {
		bput1(&f, 0x10);
		buleb(&f, entrysym->fn);
	}
	bput1(&f, 0x0b);
	buleb(&b, f.n);
	bputn(&b, f.p, f.n);
	if(debug['k']) {
		/* _init: the data where it goes; once, by the first Worker */
		f.n = 0;
		buleb(&f, 0);
		bput1(&f, 0x41); bsleb(&f, database);
		bput1(&f, 0x41); bsleb(&f, 0);
		bput1(&f, 0x41); bsleb(&f, dataend - database);
		bput1(&f, 0xfc); buleb(&f, 8); buleb(&f, 0); bput1(&f, 0x00);	/* memory.init 0 */
		bput1(&f, 0xfc); buleb(&f, 9); buleb(&f, 0);			/* data.drop 0 */
		bput1(&f, 0x0b);
		buleb(&b, f.n);
		bputn(&b, f.p, f.n);
	}
	bfree(&f);
	section(&out, 10, &b);

	/* data: one segment, database to dataend; -k passive (_init) */
	if(dataend > database || debug['k']) {
		mem = emalloc(dataend - database + 1);
		for(s = allsym; s != nil; s = s->next) {
			if(s->type != SDATA || s->data == nil)
				continue;
			memmove(mem + s->value - database, s->data, s->size);
			for(r = s->rel; r != nil; r = r->link) {
				cursym = s;
				v = symaddr(r->sym) + r->add;
				for(i = 0; i < r->width; i++)
					mem[s->value - database + r->off + i] = v >> 8*i;
			}
		}
		buleb(&b, 1);
		if(debug['k'])
			buleb(&b, 1);	/* passive */
		else {
			buleb(&b, 0);
			bput1(&b, 0x41); bsleb(&b, database); bput1(&b, 0x0b);
		}
		buleb(&b, dataend - database);
		bputn(&b, mem, dataend - database);
		free(mem);
		section(&out, 11, &b);
	}

	/* names, for the debuggers */
	{
		Buf n;

		memset(&n, 0, sizeof n);
		buleb(&n, nfuncs + 1 + nimport + (debug['k'] ? 1 : 0));
		buleb(&n, 0);
		bstr(&n, "plan9.syscall");
		for(i = 0; i < nimport-1; i++) {
			buleb(&n, imports[i]->fn);
			bstr(&n, imports[i]->name);
		}
		for(i = 0; i < nfuncs; i++) {
			buleb(&n, funcs[i]->fn);
			bstr(&n, funcs[i]->name);
		}
		buleb(&n, nimport + nfuncs);
		bstr(&n, "_start");
		if(debug['k']) {
			buleb(&n, nimport + nfuncs + 1);
			bstr(&n, "_init");
		}
		bstr(&b, "name");
		bput1(&b, 1);
		buleb(&b, n.n);
		bputn(&b, n.p, n.n);
		bfree(&n);
		section(&out, 0, &b);
	}

	if(nerrors)
		return;
	fd = create(outfile, OWRITE, 0775);
	if(fd < 0) {
		diag("cannot create %s: %r", outfile);
		return;
	}
	if(write(fd, out.p, out.n) != out.n)
		diag("%s: write: %r", outfile);
	close(fd);
	if(debug['v'])
		fprint(2, "3l: %s: %ld bytes\n", outfile, out.n);
}
