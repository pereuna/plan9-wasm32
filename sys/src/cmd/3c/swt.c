#include "gc.h"

/*
 * switch: binary search down to 4 cases, then compares
 */
void
swit1(C1 *q, int nc, long def, Node *n)
{
	C1 *r;
	int i;
	Prog *sp;

	if(nc < 5) {
		for(i=0; i<nc; i++) {
			if(debug['W'])
				print("case = %.8llux\n", q->val);
			gcmp(OEQ, n, q->val);
			patch(p, q->label);
			q++;
		}
		gbranch(OGOTO);
		patch(p, def);
		return;
	}
	i = nc / 2;
	r = q+i;
	if(debug['W'])
		print("case > %.8llux\n", r->val);
	gcmp(OGT, n, r->val);
	sp = p;
	gcmp(OEQ, n, r->val);
	patch(p, r->label);
	swit1(q, i, def, n);

	if(debug['W'])
		print("case < %.8llux\n", r->val);
	patch(sp, pc);
	swit1(r+1, nc-i-1, def, n);
}

/*
 * bit fields: b is the field (OBIT), n1 gets its word, n2 its value,
 * n3 the address if the word is not addressable
 */
void
bitload(Node *b, Node *n1, Node *n2, Node *n3, Node *nn)
{
	int sh;
	long v;
	Node *l;

	/*
	 * n1 gets adjusted/masked value
	 * n2 gets address of cell
	 * n3 gets contents of cell
	 */
	l = b->left;
	if(n2 != Z) {
		regalloc(n1, l, nn);
		reglcgen(n2, l, Z);
		regalloc(n3, l, Z);
		gmove(n2, n3);
		gmove(n3, n1);
	} else {
		regalloc(n1, l, nn);
		cgen(l, n1);
	}
	if(b->type->shift == 0 && typeu[b->type->etype]) {
		v = ~0 + (1L << b->type->nbits);
		gins3(AAND, Kw, n1, nodconst(v), n1);
	} else {
		sh = 32 - b->type->shift - b->type->nbits;
		if(sh > 0)
			gins3(ASHL, Kw, n1, nodconst(sh), n1);
		sh += b->type->shift;
		if(sh > 0)
			gins3(typeu[b->type->etype]? ASHRU: ASHR, Kw, n1, nodconst(sh), n1);
	}
}

void
bitstore(Node *b, Node *n1, Node *n2, Node *n3, Node *nn)
{
	long v;
	Node nod;
	int sh;

	regalloc(&nod, b->left, Z);
	v = ~0 + (1L << b->type->nbits);
	gins3(AAND, Kw, n1, nodconst(v), n1);
	gmove(n1, &nod);
	if(nn != Z)
		gmove(n1, nn);
	sh = b->type->shift;
	if(sh > 0)
		gins3(ASHL, Kw, &nod, nodconst(sh), &nod);
	v <<= sh;
	gins3(AAND, Kw, n3, nodconst(~v), n3);
	gins3(AOR, Kw, n3, &nod, n3);
	gmove(n3, n2);

	regfree(&nod);
	regfree(n1);
	regfree(n2);
	regfree(n3);
}

long
outstring(char *s, long n)
{
	long r;

	if(suppress)
		return nstring;
	r = nstring;
	while(n) {
		string[mnstring] = *s++;
		mnstring++;
		nstring++;
		if(mnstring >= NSNAME) {
			gpseudo(ADATA, symstring, nodconst(0L));
			p->from.offset += nstring - NSNAME;
			p->from2.type = D_CONST;
			p->from2.offset = NSNAME;
			p->to.type = D_SCONST;
			memmove(p->to.sval, string, NSNAME);
			mnstring = 0;
		}
		n--;
	}
	return r;
}

void
sextern(Sym *s, Node *a, long o, long w)
{
	long e, lw;

	for(e=0; e<w; e+=NSNAME) {
		lw = NSNAME;
		if(w-e < lw)
			lw = w-e;
		gpseudo(ADATA, s, nodconst(0L));
		p->from.offset += o+e;
		p->from2.type = D_CONST;
		p->from2.offset = lw;
		p->to.type = D_SCONST;
		memmove(p->to.sval, a->cstring+e, lw);
	}
}

void
gextern(Sym *s, Node *a, long o, long w)
{
	gpseudo(ADATA, s, a);
	p->from.offset += o;
	p->from2.type = D_CONST;
	p->from2.offset = w;
	switch(p->to.type) {
	case D_EXTERN:
	case D_STATIC:
		/* a name: its address */
		p->to.index = p->to.type;
		p->to.type = D_ADDR;
		break;
	default:
		diag(a, "bad initializer: %D", &p->to);
	case D_CONST:
	case D_FCONST:
	case D_ADDR:
		break;
	}
}

/*
 * the object: as Ken's, symbols by number (ANAME), then the Progs
 */
static	Biobuf	obuf;

static void
put1(int c)
{
	Bputc(&obuf, c);
}

static void
put2(int v)
{
	put1(v);
	put1(v>>8);
}

static void
put4(long v)
{
	put2(v);
	put2(v>>16);
}

static void
put8(vlong v)
{
	put4(v);
	put4(v>>32);
}

typedef struct Onum Onum;
struct Onum
{
	Sym*	sym;
	int	type;
	int	num;
	Onum*	link;
};
static	Onum*	onumhash[NHASH];
static	int	nosym;

static int
symnum(Sym *s, int t)
{
	Onum *o;
	ulong h;
	char *n;

	if(s == S)
		return 0;
	h = ((uintptr)s >> 4) % NHASH;
	for(o = onumhash[h]; o != nil; o = o->link)
		if(o->sym == s && o->type == t)
			return o->num;
	o = alloc(sizeof(*o));
	o->sym = s;
	o->type = t;
	o->num = ++nosym;
	if(nosym > 0xffff)
		fatal(Z, "too many symbols in one object");
	o->link = onumhash[h];
	onumhash[h] = o;
	put2(ANAME);
	put1(t);
	put2(o->num);
	for(n = s->name; *n; n++)
		put1(*n);
	put1(0);
	return o->num;
}

/*
 * #pragma lib "libc.a": the libraries a program needs, as Plan 9's
 * compilers pass them to the loader (the front end keeps them as history
 * lines with offset -1): a name record of type D_FILE and number 0, which
 * 3l loads from /$objtype/lib - so 9front's mkone links a program with no
 * libraries named
 */
static void
outlibs(void)
{
	Hist *h;
	char *n;

	for(h = hist; h != H; h = h->link) {
		if(h->offset != -1 || h->name == nil)
			continue;
		put2(ANAME);
		put1(D_FILE);
		put2(0);
		for(n = h->name; *n; n++)
			put1(*n);
		put1(0);
	}
}

static int
symtype(Adr *a)
{
	switch(a->type) {
	case D_ADDR:
		return a->index == D_STATIC ? D_STATIC : D_EXTERN;
	case D_STATIC:
		return D_STATIC;
	}
	return D_EXTERN;
}

static void
outadr(Adr *a)
{
	union {
		double	d;
		uvlong	v;
	} u;

	put1(a->type);
	if(a->type == D_NONE)
		return;
	put1(a->index);
	put4(a->reg);
	put2(a->sym != S ? symnum(a->sym, symtype(a)) : 0);
	switch(a->type) {
	case D_FCONST:
		u.d = a->dval;
		put8(u.v);
		break;
	case D_SCONST:
		Bwrite(&obuf, a->sval, NSNAME);
		break;
	default:
		put8(a->offset);
		break;
	}
}

static void outlibs(void);
void
outcode(void)
{
	Prog *p;
	int f;

	if(debug['S'])
		for(p = firstp; p != P; p = p->link)
			print("%P\n", p);
	f = open(outfile, OWRITE);
	if(f < 0) {
		diag(Z, "cannot open %s", outfile);
		return;
	}
	Binit(&obuf, f, OWRITE);
	Bseek(&obuf, 0L, 2);
	Bwrite(&obuf, OBJMAGIC, strlen(OBJMAGIC));
	outlibs();
	for(p = firstp; p != P; p = p->link) {
		/* the names first: a record refers only to names before it */
		if(p->from.sym != S)
			symnum(p->from.sym, symtype(&p->from));
		if(p->from2.sym != S)
			symnum(p->from2.sym, symtype(&p->from2));
		if(p->to.sym != S)
			symnum(p->to.sym, symtype(&p->to));
		put2(p->as);
		put1(p->k);
		put1(p->k2);
		put4(p->lineno);
		outadr(&p->from);
		outadr(&p->from2);
		outadr(&p->to);
	}
	Bterm(&obuf);
	close(f);
	firstp = P;
	lastp = P;
}

long
align(long i, Type *t, int op)
{
	long o;
	Type *v;
	int w;

	o = i;
	w = 1;
	switch(op) {
	default:
		diag(Z, "unknown align opcode %d", op);
		break;

	case Asu2:	/* padding at end of a struct */
		w = SZ_VLONG;
		if(packflg)
			w = packflg;
		break;

	case Ael1:	/* initial align of struct element */
		for(v=t; v->etype==TARRAY; v=v->link)
			;
		w = ewidth[v->etype];
		if(w <= 0 || w >= SZ_VLONG)
			w = SZ_VLONG;
		if(packflg)
			w = packflg;
		break;

	case Ael2:	/* width of a struct element */
		o += t->width;
		break;

	case Aarg0:	/* initial passbyptr argument in arg list */
		if(typesu[t->etype]) {
			o = align(o, types[TIND], Aarg1);
			o = align(o, types[TIND], Aarg2);
		}
		break;

	case Aarg1:	/* initial align of parameter: 4-byte slots, as 8c */
		w = ewidth[t->etype];
		if(w <= 0 || w >= SZ_LONG) {
			w = SZ_LONG;
			break;
		}
		w = 1;		/* little endian no adjustment */
		break;

	case Aarg2:	/* width of a parameter */
		o += t->width;
		w = SZ_LONG;
		break;

	case Aaut3:	/* total align of automatic */
		o = align(o, t, Ael1);
		o = align(o, t, Ael2);
		break;
	}
	o = round(o, w);
	if(debug['A'])
		print("align %s %ld %T = %ld\n", bnames[op], i, t, o);
	return o;
}

long
maxround(long max, long v)
{
	v += SZ_VLONG-1;
	if(v > max)
		max = round(v, SZ_VLONG);
	return max;
}
