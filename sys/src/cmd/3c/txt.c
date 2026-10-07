#include "gc.h"

/*
 * registers: any number per class, numbered per function (ATEXT
 * starts again); rcount[k][n] is how many hold register n of class k
 */
enum { NRMAX = 4096 };
static	uchar	rcount[NK][NRMAX];
static	int	rmax[NK];

void
ginit(void)
{
	thechar = '3';
	thestring = "wasm32";
	listinit();
	nstring = 0;
	mnstring = 0;
	pc = 0;
	breakpc = -1;
	continpc = -1;
	cases = C;
	firstp = P;
	lastp = P;
	tfield = types[TLONG];

	typeword = typechlp;
	typeswitch = typechlv;
	typecmplx = typesu;

	zprog.link = P;
	zprog.as = AGOK;
	zprog.from.type = D_NONE;
	zprog.from2 = zprog.from;
	zprog.to = zprog.from;

	regnode.op = OREGISTER;
	regnode.class = CEXREG;
	regnode.complex = 0;
	regnode.addable = 11;
	regnode.type = types[TLONG];

	constnode.op = OCONST;
	constnode.class = CXXX;
	constnode.complex = 0;
	constnode.addable = 20;
	constnode.type = types[TLONG];

	vconstnode = constnode;
	vconstnode.type = types[TVLONG];

	fconstnode.op = OCONST;
	fconstnode.class = CXXX;
	fconstnode.complex = 0;
	fconstnode.addable = 20;
	fconstnode.type = types[TDOUBLE];

	nodsafe = new(ONAME, Z, Z);
	nodsafe->sym = slookup(".safe");
	nodsafe->type = types[TINT];
	nodsafe->etype = types[TINT]->etype;
	nodsafe->class = CAUTO;
	complex(nodsafe);

	nodret = new(ONAME, Z, Z);
	nodret->sym = slookup(".ret");
	nodret->type = types[TIND];
	nodret->etype = TIND;
	nodret->class = CPARAM;
	nodret = new(OIND, nodret, Z);
	complex(nodret);
}

void
gclean(void)
{
	int k, i;
	Sym *s;

	for(k=0; k<NK; k++)
		for(i=NREGFIRST; i<rmax[k]; i++)
			if(rcount[k][i])
				diag(Z, "reg %R left allocated", MKREG(i, k));
	while(mnstring)
		outstring("", 1L);
	symstring->type->width = nstring;
	for(i=0; i<NHASH; i++)
	for(s = hash[i]; s != S; s = s->link) {
		if(s->type == T)
			continue;
		if(s->type->width == 0)
			continue;
		if(s->class != CGLOBL && s->class != CSTATIC)
			continue;
		if(s->type == types[TENUM])
			continue;
		gpseudo(AGLOBL, s, nodconst(s->type->width));
	}
	nextpc();
	p->as = AEND;
	outcode();
}

void
nextpc(void)
{

	p = alloc(sizeof(*p));
	*p = zprog;
	p->lineno = nearln;
	pc++;
	if(firstp == P) {
		firstp = p;
		lastp = p;
		return;
	}
	lastp->link = p;
	lastp = p;
}

/*
 * the class of a type: its WebAssembly value type
 */
int
kind(Type *t)
{
	if(t == T)
		return Kw;
	switch(t->etype) {
	case TVLONG:
	case TUVLONG:
		return Kv;
	case TFLOAT:
		return Kf;
	case TDOUBLE:
		return Kd;
	}
	return Kw;
}

int
regkind(Node *n)
{
	return RCLASS(n->reg);
}

static int
movop(Type *t)
{
	switch(t->etype) {
	case TCHAR:
		return AMOVB;
	case TUCHAR:
		return AMOVBU;
	case TSHORT:
		return AMOVH;
	case TUSHORT:
		return AMOVHU;
	case TVLONG:
	case TUVLONG:
		return AMOVV;
	case TFLOAT:
		return AMOVF;
	case TDOUBLE:
		return AMOVD;
	}
	return AMOVW;
}

/*
 * arguments: those with calls in them first, to temporaries (a call
 * would overwrite the outgoing arguments), then all to base(SP) ...
 */
static	void	garg1(Node*, Node*, Node*, int, Node**);

void
gargs(Node *n, Node *tn1, Node *tn2, long base)
{
	long regs;
	Node fnxargs[20], *fnxp;

	regs = cursafe;

	fnxp = fnxargs;
	garg1(n, tn1, tn2, 0, &fnxp);	/* compile fns to temps */

	curarg = base;
	maxargsafe = maxround(maxargsafe, cursafe+curarg);
	fnxp = fnxargs;
	garg1(n, tn1, tn2, 1, &fnxp);	/* compile normal args and temps */

	cursafe = regs;
}

static void
garg1(Node *n, Node *tn1, Node *tn2, int f, Node **fnxp)
{
	Node nod;

	if(n == Z)
		return;
	if(n->op == OLIST) {
		garg1(n->left, tn1, tn2, f, fnxp);
		garg1(n->right, tn1, tn2, f, fnxp);
		return;
	}
	if(f == 0) {
		if(n->complex >= FNX) {
			regsalloc(*fnxp, n);
			nod = znode;
			nod.op = OAS;
			nod.left = *fnxp;
			nod.right = n;
			nod.type = n->type;
			cgen(&nod, Z);
			(*fnxp)++;
		}
		return;
	}
	if(typesu[n->type->etype]) {
		regaalloc(tn2, n);
		if(n->complex >= FNX) {
			sugen(*fnxp, tn2, n->type->width);
			(*fnxp)++;
		} else
			sugen(n, tn2, n->type->width);
		return;
	}
	if(n->op == OCONST) {
		regaalloc(tn2, n);
		gmove(n, tn2);
		return;
	}
	regalloc(tn1, n, Z);
	if(n->complex >= FNX) {
		cgen(*fnxp, tn1);
		(*fnxp)++;
	} else
		cgen(n, tn1);
	regaalloc(tn2, n);
	gmove(tn1, tn2);
	regfree(tn1);
}

Node*
nodgconst(vlong v, Type *t)
{
	if(!typev[t->etype])
		return nodconst((long)v);
	vconstnode.vconst = v;
	return &vconstnode;
}

Node*
nodconst(long v)
{
	constnode.vconst = v;
	return &constnode;
}

Node*
nodfconst(double d)
{
	fconstnode.fconst = d;
	return &fconstnode;
}

int
nodreg(Node *n, Node *nn, int r)
{
	Type *t;
	long l;

	t = T;
	l = 0;
	if(nn != Z){	/* before *n is written: n may be nn */
		t = nn->type;
		l = nn->lineno;
	}
	*n = regnode;
	n->reg = r;
	if(nn != Z){
		n->type = t;
		n->lineno = l;
	}
	return 0;
}

void
regret(Node *n, Node *nn)
{
	nodreg(n, nn, MKREG(NREGRET, kind(nn->type)));
}

void
regalloc(Node *n, Node *tn, Node *o)
{
	int k, i;

	k = kind(tn->type);
	if(typesu[tn->type->etype] || tn->type->etype == TVOID)
		diag(tn, "unknown type in regalloc: %T", tn->type);
	if(o != Z && o->op == OREGISTER && RCLASS(o->reg) == k && RNUM(o->reg) >= NREGFIRST) {
		i = RNUM(o->reg);
		goto out;
	}
	for(i=NREGFIRST; i<NRMAX; i++)
		if(rcount[k][i] == 0)
			goto out;
	diag(tn, "out of registers");
	i = NREGFIRST;
out:
	rcount[k][i]++;
	if(i >= rmax[k])
		rmax[k] = i+1;
	nodreg(n, tn, MKREG(i, k));
}

void
regialloc(Node *n, Node *tn, Node *o)
{
	Node nod;

	nod = *tn;
	nod.type = types[TIND];
	regalloc(n, &nod, o);
}

void
regfree(Node *n)
{
	int k, i;

	if(n->op != OREGISTER && n->op != OINDREG)
		goto err;
	k = RCLASS(n->reg);
	i = RNUM(n->reg);
	if(i < NREGFIRST)
		return;		/* fixed: REGRET, REGSP */
	if(i >= NRMAX || rcount[k][i] == 0)
		goto err;
	rcount[k][i]--;
	return;
err:
	diag(n, "error in regfree: %R", n->reg);
}

void
regsalloc(Node *n, Node *nn)
{
	cursafe = align(cursafe, nn->type, Aaut3);
	maxargsafe = maxround(maxargsafe, cursafe+curarg);
	*n = *nodsafe;
	n->xoffset = -(stkoff + cursafe);
	n->type = nn->type;
	n->etype = nn->type->etype;
	n->lineno = nn->lineno;
}

void
regaalloc(Node *n, Node *nn)
{
	curarg = align(curarg, nn->type, Aarg1);
	*n = *nn;
	n->op = OINDREG;
	n->reg = REGSP;
	n->xoffset = curarg;
	n->complex = 0;
	n->addable = 20;
	curarg = align(curarg, nn->type, Aarg2);
	maxargsafe = maxround(maxargsafe, cursafe+curarg);
}

void
regind(Node *n, Node *nn)
{

	if(n->op != OREGISTER) {
		diag(n, "regind not OREGISTER");
		return;
	}
	n->op = OINDREG;
	n->type = nn->type;
}

void
naddr(Node *n, Adr *a)
{
	long v;

	a->type = D_NONE;
	if(n == Z)
		return;
	switch(n->op) {
	default:
	bad:
		diag(n, "bad in naddr: %O %D", n->op, a);
		break;

	case OREGISTER:
		a->type = D_REG;
		a->reg = n->reg;
		a->sym = S;
		break;

	case OINDREG:
		a->sym = S;
		a->offset = n->xoffset;
		if(n->reg == REGSP) {
			a->type = D_SP;
			break;
		}
		a->type = D_OREG;
		a->reg = n->reg;
		break;

	case ONAME:
		a->etype = n->etype;
		a->offset = n->xoffset;
		a->sym = n->sym;
		switch(n->class) {
		default:
			goto bad;
		case CEXTERN:
		case CGLOBL:
			a->type = D_EXTERN;
			break;
		case CSTATIC:
			a->type = D_STATIC;
			break;
		case CAUTO:
			a->type = D_AUTO;
			break;
		case CPARAM:
			a->type = D_PARAM;
			break;
		}
		break;

	case OCONST:
		a->sym = S;
		if(typefd[n->type->etype]) {
			a->type = D_FCONST;
			a->dval = n->fconst;
			break;
		}
		a->type = D_CONST;
		a->offset = n->vconst;
		break;

	case OADDR:
		naddr(n->left, a);
		switch(a->type) {
		case D_EXTERN:
		case D_STATIC:
		case D_AUTO:
		case D_PARAM:
		case D_SP:
		case D_OREG:
			a->index = a->type;
			a->type = D_ADDR;
			break;
		default:
			goto bad;
		}
		break;

	case OADD:
		if(n->right->op == OCONST) {
			v = n->right->vconst;
			naddr(n->left, a);
		} else
		if(n->left->op == OCONST) {
			v = n->left->vconst;
			naddr(n->right, a);
		} else
			goto bad;
		a->offset += v;
		break;
	}
}

/*
 * is n a value the machine has: a register or a constant
 */
static int
isval(Node *n)
{
	return n->op == OREGISTER || n->op == OCONST;
}

/*
 * a constant of type t from c (of its own type), as C converts
 */
static Node*
convconst(Node *c, Type *t)
{
	Node *n;
	vlong v;
	double d;
	int ft, tt;

	ft = c->type->etype;
	tt = t->etype;
	n = new(OCONST, Z, Z);
	*n = constnode;
	n->type = t;
	if(typefd[tt]) {
		if(typefd[ft])
			d = c->fconst;
		else if(typeu[ft] && typev[ft])
			d = (double)(uvlong)c->vconst;
		else if(typeu[ft])
			d = (double)(ulong)c->vconst;
		else
			d = (double)c->vconst;
		if(tt == TFLOAT)
			d = (float)d;
		n->fconst = d;
		return n;
	}
	if(typefd[ft]) {
		if(typeu[tt])
			v = (uvlong)c->fconst;
		else
			v = (vlong)c->fconst;
	} else
		v = c->vconst;
	switch(tt) {
	case TCHAR:	v = (schar)v; break;
	case TUCHAR:	v = (uchar)v; break;
	case TSHORT:	v = (short)v; break;
	case TUSHORT:	v = (ushort)v; break;
	case TINT:
	case TLONG:	v = (long)v; break;
	case TUINT:
	case TULONG:
	case TIND:
	case TFUNC:	v = (ulong)v; break;
	}
	n->vconst = v;
	return n;
}

/*
 * a sub-word register keeps its value as the type says: sign or zero
 * extended
 */
static int
subword(Type *t)
{
	switch(t->etype) {
	case TCHAR:
	case TUCHAR:
	case TSHORT:
	case TUSHORT:
		return 1;
	}
	return 0;
}

void
gmove(Node *f, Node *t)
{
	int ft, tt, kf, kt, a;
	Node nod, nod1;

	ft = f->type->etype;
	tt = t->type->etype;
	if(debug['M'])
		print("gop: %O %O[%s],%O[%s]\n", OAS,
			f->op, tnames[ft], t->op, tnames[tt]);
	if(typesu[ft] || typesu[tt]) {
		diag(Z, "gmove of a struct or union");
		return;
	}

	/* constants: converted here */
	if(f->op == OCONST) {
		if(ft != tt && !(typechlp[ft] && typechlp[tt] && ewidth[ft] == ewidth[tt] && !subword(t->type)))
			f = convconst(f, t->type);
		gins(movop(t->type), f, t);
		return;
	}

	kf = kind(f->type);
	kt = kind(t->type);

	/* memory to a register of the same class: the load extends */
	if(f->op != OREGISTER && t->op == OREGISTER && kf == kt) {
		gins(movop(f->type), f, t);
		if(subword(t->type) && ft != tt)
			gins(movop(t->type), t, t);
		return;
	}

	/* a value in a register of f's class */
	if(f->op != OREGISTER) {
		regalloc(&nod, f, Z);
		gins(movop(f->type), f, &nod);
		gmove(&nod, t);
		regfree(&nod);
		return;
	}
	if(kf != kt) {
		a = ACVT;
		if(typefd[tt] ? typeu[ft] : typeu[tt] && typefd[ft] || kf == Kw && kt == Kv && typeu[ft])
			a = ACVTU;
		if(t->op == OREGISTER) {
			gins(a, f, t);
			p->k = kt;
			p->k2 = kf;
			if(subword(t->type))
				gins(movop(t->type), t, t);
			return;
		}
		regalloc(&nod1, t, Z);
		gins(a, f, &nod1);
		p->k = kt;
		p->k2 = kf;
		gins(movop(t->type), &nod1, t);
		regfree(&nod1);
		return;
	}

	/* the same class: a register to a register or to memory */
	if(t->op == OREGISTER) {
		if(f->reg == t->reg && !(subword(t->type) && ft != tt))
			return;
		if(subword(t->type) && ft != tt)
			gins(movop(t->type), f, t);
		else
			gins(movop(t->type) == AMOVB || movop(t->type) == AMOVBU ||
				movop(t->type) == AMOVH || movop(t->type) == AMOVHU ? AMOVW : movop(t->type), f, t);
		return;
	}
	gins(movop(t->type), f, t);
}

void
gins(int a, Node *f, Node *t)
{

	if(f != Z && f->op == OINDEX)
		diag(f, "gins OINDEX");
	nextpc();
	p->as = a;
	if(f != Z)
		naddr(f, &p->from);
	if(t != Z)
		naddr(t, &p->to);
	switch(a) {
	case AMOVV:
		p->k = Kv;
		break;
	case AMOVF:
		p->k = Kf;
		break;
	case AMOVD:
		p->k = Kd;
		break;
	}
	if(debug['g'])
		print("%P\n", p);
}

/*
 * to = f op f2, class k
 */
void
gins3(int a, int k, Node *f, Node *f2, Node *t)
{
	nextpc();
	p->as = a;
	p->k = k;
	if(f != Z)
		naddr(f, &p->from);
	if(f2 != Z)
		naddr(f2, &p->from2);
	if(t != Z)
		naddr(t, &p->to);
	if(debug['g'])
		print("%P\n", p);
}

/*
 * branch if n (a register) rel v: for the switches
 */
void
gcmp(int o, Node *n, vlong v)
{
	Node nod, *c;
	int a;

	switch(o) {
	case OEQ: a = ACMPEQ; break;
	case OGT: a = ACMPGT; break;
	default:
		diag(n, "gcmp %O", o);
		a = ACMPEQ;
	}
	c = nodgconst(v, n->type);
	nod = regnode;
	nod.type = types[TLONG];
	regalloc(&nod, &nod, Z);
	gins3(a, kind(n->type), n, c, &nod);
	gbranchk(ABNZ, &nod);
	regfree(&nod);
}

void
gbranch(int o)
{
	int a;

	a = AGOK;
	switch(o) {
	case ORETURN:
		a = ARET;
		break;
	case OGOTO:
		a = AJMP;
		break;
	}
	nextpc();
	if(a == AGOK) {
		diag(Z, "bad in gbranch %O",  o);
		nextpc();
	}
	p->as = a;
}

/*
 * branch on a Kw register: ABNZ, ABZ
 */
void
gbranchk(int a, Node *n)
{
	nextpc();
	p->as = a;
	naddr(n, &p->from);
}

void
patch(Prog *op, long pc)
{

	op->to.offset = pc;
	op->to.type = D_BRANCH;
}

void
gpseudo(int a, Sym *s, Node *n)
{
	int k;

	nextpc();
	p->as = a;
	p->from.type = D_EXTERN;
	p->from.sym = s;
	if(s->class == CSTATIC)
		p->from.type = D_STATIC;
	naddr(n, &p->to);
	if(a == ATEXT) {
		for(k=0; k<NK; k++) {
			memset(rcount[k], 0, rmax[k]);
			rmax[k] = 0;
		}
	}
}

int
sconst(Node *n)
{
	USED(n);
	return 0;
}

void
regopt(Prog *p)
{
	USED(p);
}

/*
 * extern register: the first two words or pointers (the kernel's m and
 * up, dat.h) are registers 2 and 3 - 3l's per-instance globals, so each
 * Worker (a CPU) has its own; in the same order in every file, as 6c's
 */
long
exreg(Type *t)
{
	static int n;

	if(t == T || !typechlp[t->etype] || n >= 2)
		return 0;
	return MKREG(NREGEXT + n++, Kw);
}

schar	ewidth[NTYPE] =
{
	-1,		/*[TXXX]*/
	SZ_CHAR,	/*[TCHAR]*/
	SZ_CHAR,	/*[TUCHAR]*/
	SZ_SHORT,	/*[TSHORT]*/
	SZ_SHORT,	/*[TUSHORT]*/
	SZ_INT,		/*[TINT]*/
	SZ_INT,		/*[TUINT]*/
	SZ_LONG,	/*[TLONG]*/
	SZ_LONG,	/*[TULONG]*/
	SZ_VLONG,	/*[TVLONG]*/
	SZ_VLONG,	/*[TUVLONG]*/
	SZ_FLOAT,	/*[TFLOAT]*/
	SZ_DOUBLE,	/*[TDOUBLE]*/
	SZ_IND,		/*[TIND]*/
	0,		/*[TFUNC]*/
	-1,		/*[TARRAY]*/
	0,		/*[TVOID]*/
	-1,		/*[TSTRUCT]*/
	-1,		/*[TUNION]*/
	SZ_INT,		/*[TENUM]*/
};
long	ncast[NTYPE] =
{
	0,				/*[TXXX]*/
	BCHAR|BUCHAR,			/*[TCHAR]*/
	BCHAR|BUCHAR,			/*[TUCHAR]*/
	BSHORT|BUSHORT,			/*[TSHORT]*/
	BSHORT|BUSHORT,			/*[TUSHORT]*/
	BINT|BUINT|BLONG|BULONG|BIND,	/*[TINT]*/
	BINT|BUINT|BLONG|BULONG|BIND,	/*[TUINT]*/
	BINT|BUINT|BLONG|BULONG|BIND,	/*[TLONG]*/
	BINT|BUINT|BLONG|BULONG|BIND,	/*[TULONG]*/
	BVLONG|BUVLONG,			/*[TVLONG]*/
	BVLONG|BUVLONG,			/*[TUVLONG]*/
	BFLOAT,				/*[TFLOAT]*/
	BDOUBLE,			/*[TDOUBLE]*/
	BLONG|BULONG|BIND,		/*[TIND]*/
	0,				/*[TFUNC]*/
	0,				/*[TARRAY]*/
	0,				/*[TVOID]*/
	BSTRUCT,			/*[TSTRUCT]*/
	BUNION,				/*[TUNION]*/
	0,				/*[TENUM]*/
};
