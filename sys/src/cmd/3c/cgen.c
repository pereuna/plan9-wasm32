#include "gc.h"

/*
 * 3c's code: every value is computed into a register (a WebAssembly
 * local) and only the MOVs touch memory.  Registers are never short,
 * so there is no Sethi-Ullman ordering: left, then right.
 */

static	void	gcall(Node*, Node*, Node*);
static	void	lval(Node*, Node*, Node*);
static	void	gas(Node*, Node*);
static	void	gasop(Node*, Node*);
static	void	gincdec(Node*, Node*);
void	reglcgen(Node*, Node*, Node*);

static int
arithop(int o)
{
	switch(o) {
	case OADD:	case OASADD:	return AADD;
	case OSUB:	case OASSUB:	return ASUB;
	case OMUL:	case OASMUL:
	case OLMUL:	case OASLMUL:	return AMUL;
	case ODIV:	case OASDIV:	return ADIV;
	case OLDIV:	case OASLDIV:	return ADIVU;
	case OMOD:	case OASMOD:	return AMOD;
	case OLMOD:	case OASLMOD:	return AMODU;
	case OAND:	case OASAND:	return AAND;
	case OOR:	case OASOR:	return AOR;
	case OXOR:	case OASXOR:	return AXOR;
	case OASHL:	case OASASHL:	return ASHL;
	case OASHR:	case OASASHR:	return ASHR;
	case OLSHR:	case OASLSHR:	return ASHRU;
	}
	return AGOK;
}

static int
relop(int o)
{
	switch(o) {
	case OEQ:	return ACMPEQ;
	case ONE:	return ACMPNE;
	case OLT:	return ACMPLT;
	case OLE:	return ACMPLE;
	case OGT:	return ACMPGT;
	case OGE:	return ACMPGE;
	case OLO:	return ACMPLO;
	case OLS:	return ACMPLS;
	case OHI:	return ACMPHI;
	case OHS:	return ACMPHS;
	}
	return AGOK;
}

static int
isshift(int a)
{
	return a == ASHL || a == ASHR || a == ASHRU;
}

static int
isfixed(Node *n)
{
	return n != Z && n->op == OREGISTER && RNUM(n->reg) < NREGFIRST;
}

/*
 * operand: n as an operand of class k: a constant as it is, else in a
 * register (r, allocated here; freed by the caller if r->op is
 * OREGISTER)
 */
static Node*
operand(Node *n, int k, Node *r)
{
	Node nod;

	r->op = OXXX;
	if(n->op == OCONST)
		return n;
	if(kind(n->type) == k) {
		regalloc(r, n, Z);
		cgen(n, r);
		return r;
	}
	regalloc(&nod, n, Z);
	cgen(n, &nod);
	*r = nod;
	r->type = k == Kv ? types[TVLONG] : k == Kf ? types[TFLOAT] : k == Kd ? types[TDOUBLE] : types[TLONG];
	regalloc(r, r, Z);
	gmove(&nod, r);
	regfree(&nod);
	return r;
}

static void
normalize(Node *nn)
{
	switch(nn->type->etype) {
	case TCHAR:	gins(AMOVB, nn, nn); break;
	case TUCHAR:	gins(AMOVBU, nn, nn); break;
	case TSHORT:	gins(AMOVH, nn, nn); break;
	case TUSHORT:	gins(AMOVHU, nn, nn); break;
	}
}

void
cgen(Node *n, Node *nn)
{
	Node *l, *r, nod, nod1, nod2, nod3, *o2;
	Prog *p1, *p2;
	int o, k, a;
	long curs;

	if(debug['g']) {
		prtree(nn, "cgen lhs");
		prtree(n, "cgen");
	}
	if(n == Z || n->type == T)
		return;
	if(typesu[n->type->etype]) {
		sugen(n, nn, n->type->width);
		return;
	}
	l = n->left;
	r = n->right;
	o = n->op;

	/*
	 * into memory, or into a fixed register that a call inside could
	 * overwrite: through a register of our own
	 */
	if(nn != Z && (nn->op != OREGISTER || isfixed(nn) && n->complex >= FNX) &&
	   !(n->op == OCONST || n->op == OREGISTER) && n->type->etype != TVOID) {
		regalloc(&nod, n, Z);
		cgen(n, &nod);
		gmove(&nod, nn);
		regfree(&nod);
		return;
	}
	if(nn != Z && nn->op != OREGISTER && n->type->etype != TVOID) {
		gmove(n, nn);
		return;
	}

	curs = cursafe;
	switch(o) {
	default:
		diag(n, "unknown op in cgen: %O", o);
		break;

	case OCONST:
	case ONAME:
	case OINDREG:
	case OREGISTER:
		if(nn != Z)
			gmove(n, nn);
		break;

	case OADDR:
		if(nn == Z) {
			nullwarn(l, Z);
			break;
		}
		lcgen(l, nn);
		break;

	case OIND:
		if(nn == Z) {
			cgen(l, Z);
			break;
		}
		lval(n, &nod, &nod1);
		gmove(&nod, nn);
		if(nod1.op == OREGISTER)
			regfree(&nod1);
		break;

	case ODOT:
		regsalloc(&nod, l);
		sugen(l, &nod, l->type->width);
		if(nn == Z)
			break;
		if(!r || r->op != OCONST) {
			diag(n, "DOT and no offset");
			break;
		}
		nod.xoffset += (long)r->vconst;
		nod.type = n->type;
		cgen(&nod, nn);
		break;

	case OBIT:
		if(nn == Z) {
			nullwarn(l, Z);
			break;
		}
		bitload(n, &nod, Z, Z, nn);
		gmove(&nod, nn);
		regfree(&nod);
		break;

	case OFUNC:
		gcall(n, nn, Z);
		break;

	case OCOMMA:
		cgen(l, Z);
		cgen(r, nn);
		break;

	case OCOND:
		bcgen(l, 1);
		p1 = p;
		cgen(r->left, nn);
		gbranch(OGOTO);
		patch(p1, pc);
		p1 = p;
		cgen(r->right, nn);
		patch(p1, pc);
		break;

	case OCAST:
		if(nn == Z || n->type->etype == TVOID) {
			cgen(l, Z);
			break;
		}
		if(typesu[l->type->etype]) {
			diag(n, "cast of a struct");
			break;
		}
		regalloc(&nod, l, kind(l->type) == kind(n->type) ? nn : Z);
		cgen(l, &nod);
		gmove(&nod, nn);
		regfree(&nod);
		break;

	case OAS:
		gas(n, nn);
		break;

	case OASADD:
	case OASSUB:
	case OASMUL:
	case OASLMUL:
	case OASDIV:
	case OASLDIV:
	case OASMOD:
	case OASLMOD:
	case OASAND:
	case OASOR:
	case OASXOR:
	case OASASHL:
	case OASASHR:
	case OASLSHR:
		gasop(n, nn);
		break;

	case OPOSTINC:
	case OPOSTDEC:
	case OPREINC:
	case OPREDEC:
		gincdec(n, nn);
		break;

	case OPOS:
		cgen(l, nn);
		break;

	case ONEG:
	case OCOM:
		if(nn == Z) {
			nullwarn(l, Z);
			break;
		}
		k = kind(n->type);
		regalloc(&nod, n, nn);
		cgen(l, &nod);
		if(o == OCOM)
			gins3(AXOR, k, &nod, nodgconst(-1, n->type), nn);
		else if(k == Kf || k == Kd)
			gins3(AMUL, k, &nod, nodfconst(-1.0), nn);
		else
			gins3(ASUB, k, nodgconst(0, n->type), &nod, nn);
		normalize(nn);
		regfree(&nod);
		break;

	case OADD:
	case OSUB:
	case OMUL:
	case OLMUL:
	case ODIV:
	case OLDIV:
	case OMOD:
	case OLMOD:
	case OAND:
	case OOR:
	case OXOR:
	case OASHL:
	case OASHR:
	case OLSHR:
		if(nn == Z) {
			nullwarn(l, r);
			break;
		}
		a = arithop(o);
		k = kind(n->type);
		if(kind(l->type) == k)
			regalloc(&nod, l, nn);
		else {
			nod = *nn;
			regalloc(&nod, n, nn);
		}
		if(l->op == OCONST && kind(l->type) == k) {
			o2 = operand(r, isshift(a) ? k : k, &nod1);
			gins3(a, k, l, o2, nn);
			if(nod1.op == OREGISTER)
				regfree(&nod1);
			regfree(&nod);
			normalize(nn);
			break;
		}
		if(kind(l->type) == k)
			cgen(l, &nod);
		else {
			regalloc(&nod2, l, Z);
			cgen(l, &nod2);
			gmove(&nod2, &nod);
			regfree(&nod2);
		}
		o2 = operand(r, k, &nod1);
		gins3(a, k, &nod, o2, nn);
		if(nod1.op == OREGISTER)
			regfree(&nod1);
		regfree(&nod);
		normalize(nn);
		break;

	case OEQ:
	case ONE:
	case OLT:
	case OLE:
	case OGT:
	case OGE:
	case OLO:
	case OLS:
	case OHI:
	case OHS:
		if(nn == Z) {
			nullwarn(l, r);
			break;
		}
		k = kind(l->type);
		if(kind(r->type) > k && r->op != OCONST)
			k = kind(r->type);
		if(l->op == OCONST)
			o2 = l;
		else
			o2 = operand(l, k, &nod);
		if(l->op == OCONST)
			nod.op = OXXX;
		{
			Node *o3;

			o3 = operand(r, k, &nod1);
			gins3(relop(o), k, o2, o3, nn);
		}
		if(nod.op == OREGISTER)
			regfree(&nod);
		if(nod1.op == OREGISTER)
			regfree(&nod1);
		break;

	case ONOT:
	case OANDAND:
	case OOROR:
		if(nn == Z) {
			bcgen(n, 1);
			patch(p, pc);
			break;
		}
		bcgen(n, 1);
		p1 = p;
		gmove(nodconst(1), nn);
		gbranch(OGOTO);
		p2 = p;
		patch(p1, pc);
		gmove(nodconst(0), nn);
		patch(p2, pc);
		break;
	}
	USED(nod3);
	cursafe = curs;
}

/*
 * the address of n into nn (a Kw register)
 */
void
lcgen(Node *n, Node *nn)
{
	Node nod, nod1;

	if(debug['g']) {
		prtree(nn, "lcgen lhs");
		prtree(n, "lcgen");
	}
	if(n == Z || n->type == T)
		return;
	if(nn == Z) {
		nn = &nod;
		regialloc(&nod, n, Z);
	}
	switch(n->op) {
	default:
		if(n->addable < INDEXED && n->op != OFUNC && n->op != ODOT && n->op != OAS && n->op != OCOND) {
			diag(n, "unknown op in lcgen: %O", n->op);
			break;
		}
		/* a struct value: in a temporary */
		regsalloc(&nod1, n);
		sugen(n, &nod1, n->type->width);
		lcgen(&nod1, nn);
		break;

	case ONAME:
	case OINDREG:
		nod1 = znode;
		nod1.op = OADDR;
		nod1.left = n;
		nod1.type = types[TIND];
		gins(AMOVW, &nod1, nn);
		break;

	case OIND:
		cgen(n->left, nn);
		break;

	case OCOMMA:
		cgen(n->left, Z);
		lcgen(n->right, nn);
		break;

	case OREGISTER:
		diag(n, "address of a register");
		break;
	}
	if(nn == &nod)
		regfree(&nod);
}

void
reglcgen(Node *t, Node *n, Node *nn)
{
	regialloc(t, n, nn);
	lcgen(n, t);
	regind(t, n);
}

/*
 * n, an lvalue, as a node the MOVs can use: a name, or memory at a
 * register (r, then OREGISTER: the caller frees it)
 */
static void
lval(Node *n, Node *a, Node *r)
{
	Node *l;
	vlong v;

	r->op = OXXX;
	switch(n->op) {
	case ONAME:
	case OINDREG:
	case OREGISTER:
		*a = *n;
		return;

	case OIND:
		l = n->left;
		v = 0;
		/* *(x + c): the offset in the instruction */
		if(l->op == OADD && l->right->op == OCONST && !typefd[l->right->type->etype] &&
		   l->right->vconst >= 0 && l->right->vconst < (1<<30)) {
			v = l->right->vconst;
			l = l->left;
		}
		regialloc(r, l, Z);
		cgen(l, r);
		*a = *r;
		regind(a, n);
		a->xoffset = v;
		return;
	}
	/* a struct value or the like: in a temporary */
	regialloc(r, n, Z);
	lcgen(n, r);
	*a = *r;
	regind(a, n);
	a->xoffset = 0;
}

static void
gas(Node *n, Node *nn)
{
	Node *l, *r, a, ra, nod, nod1, nod2, nod3;

	l = n->left;
	r = n->right;
	if(l->op == OBIT) {
		/* bit field: read, change, write */
		regalloc(&nod, r, Z);
		cgen(r, &nod);
		if(kind(r->type) != Kw) {
			regalloc(&nod1, l, Z);
			gmove(&nod, &nod1);
			regfree(&nod);
			nod = nod1;
		}
		bitload(l, &nod1, &nod2, &nod3, Z);
		regfree(&nod1);
		bitstore(l, &nod, &nod2, &nod3, nn);
		return;
	}
	lval(l, &a, &ra);
	if(r->op == OCONST) {
		gmove(r, &a);
		if(nn != Z)
			gmove(&a, nn);
	} else {
		regalloc(&nod, r, nn != Z && kind(nn->type) == kind(r->type) && !isfixed(nn) ? nn : Z);
		cgen(r, &nod);
		gmove(&nod, &a);
		if(nn != Z)
			gmove(&a, nn);
		regfree(&nod);
	}
	if(ra.op == OREGISTER)
		regfree(&ra);
}

/*
 * l op= r: computed in l's type, or r's when that is wider
 * (int *= double)
 */
static void
gasop(Node *n, Node *nn)
{
	Node *l, *r, *t, a, ra, nod, nod1, nod2, nod3, *o2;
	int k, op;

	l = n->left;
	r = n->right;
	op = arithop(n->op);
	if(l->op == OBIT) {
		bitload(l, &nod, &nod1, &nod2, Z);
		o2 = operand(r, Kw, &nod3);
		gins3(op, Kw, &nod, o2, &nod);
		if(nod3.op == OREGISTER)
			regfree(&nod3);
		bitstore(l, &nod, &nod1, &nod2, nn);
		return;
	}
	t = l;
	if(!isshift(op) && (typefd[r->type->etype] && !typefd[l->type->etype] ||
	   typev[r->type->etype] && !typev[l->type->etype] && !typefd[l->type->etype] ||
	   r->type->etype == TDOUBLE && l->type->etype == TFLOAT))
		t = r;
	k = kind(t->type);
	lval(l, &a, &ra);
	nod2 = *l;
	nod2.type = t->type;
	regalloc(&nod, &nod2, Z);
	gmove(&a, &nod);
	o2 = operand(r, k, &nod1);
	gins3(op, k, &nod, o2, &nod);
	if(nod1.op == OREGISTER)
		regfree(&nod1);
	gmove(&nod, &a);
	if(nn != Z)
		gmove(&a, nn);
	regfree(&nod);
	if(ra.op == OREGISTER)
		regfree(&ra);
}

static void
gincdec(Node *n, Node *nn)
{
	Node *l, *r, a, ra, nod, nod1, nod2, nod3, step;
	int k, op, post;

	l = n->left;
	op = (n->op == OPOSTINC || n->op == OPREINC) ? AADD : ASUB;
	/* the step: 1, or what the pointer points to */
	if(typefd[l->type->etype])
		step = *nodfconst(1.0);
	else {
		long v;

		v = 1;
		if(l->type->etype == TIND) {
			v = l->type->link->width;
			if(v < 1)
				v = 1;
		}
		step = *nodgconst(v, l->type);
	}
	r = &step;	/* a copy: nodconst's node is everyone's */
	post = n->op == OPOSTINC || n->op == OPOSTDEC;
	if(l->op == OBIT) {
		bitload(l, &nod, &nod1, &nod2, Z);
		if(post && nn != Z)
			gmove(&nod, nn);
		gins3(op, Kw, &nod, r, &nod);
		bitstore(l, &nod, &nod1, &nod2, post ? Z : nn);
		return;
	}
	k = kind(l->type);
	lval(l, &a, &ra);
	regalloc(&nod, l, Z);
	gmove(&a, &nod);
	if(post && nn != Z)
		gmove(&nod, nn);
	gins3(op, k, &nod, r, &nod);
	gmove(&nod, &a);
	if(!post && nn != Z)
		gmove(&a, nn);
	regfree(&nod);
	if(ra.op == OREGISTER)
		regfree(&ra);
	USED(nod3);
}

/*
 * a call: arguments at 0(SP)...; a struct's address (st) at 0(SP) and
 * the arguments after it
 */
static void
gcall(Node *n, Node *nn, Node *st)
{
	Node *l, fr, tn1, tn2, nod, sp;
	int ind;

	l = n->left;
	ind = 0;
	if(l->op != ONAME || l->class == CAUTO || l->class == CPARAM || l->type->etype != TFUNC) {
		/* through a pointer: the pointer first, a register survives calls */
		ind = 1;
		regialloc(&fr, l, Z);
		if(l->op == OIND)
			cgen(l->left, &fr);
		else
			cgen(l, &fr);
	}
	gargs(n->right, &tn1, &tn2, st != Z ? SZ_IND : 0);
	if(st != Z) {
		/* the struct's address at 0(SP); gargs put the rest after it (Aarg0) */
		sp = znode;
		sp.op = OINDREG;
		sp.reg = REGSP;
		sp.xoffset = 0;
		sp.type = types[TIND];
		gmove(st, &sp);
	}
	if(ind) {
		gins(ACALL, Z, &fr);
		regfree(&fr);
	} else
		gins(ACALL, Z, l);
	if(nn != Z && n->type->etype != TVOID) {
		regret(&nod, n);
		gmove(&nod, nn);
	}
}

/*
 * conditional branches: boolgen(n, true, nn) branches when n is not
 * true (true=1) or when it is (true=0), p the branch to patch
 */
void
bcgen(Node *n, int true)
{

	if(n->type == T)
		gbranch(OGOTO);
	else
		boolgen(n, true, Z);
}

void
boolgen(Node *n, int true, Node *nn)
{
	int o;
	Prog *p1, *p2;
	Node *l, *r, nod;

	if(debug['g']) {
		prtree(nn, "boolgen lhs");
		prtree(n, "boolgen");
	}
	l = n->left;
	r = n->right;
	switch(n->op) {

	default:
		/* a value: compared with 0 */
		regalloc(&nod, n, Z);
		cgen(n, &nod);
		if(kind(n->type) != Kw) {
			Node c;

			c = nod;
			c.type = types[TLONG];
			regalloc(&c, &c, Z);
			gins3(ACMPNE, kind(n->type), &nod, typefd[n->type->etype] ? nodfconst(0.0) : nodgconst(0, n->type), &c);
			gbranchk(true ? ABZ : ABNZ, &c);
			regfree(&c);
		} else
			gbranchk(true ? ABZ : ABNZ, &nod);
		regfree(&nod);
		goto com;

	case OCONST:
		o = vconst(n);
		if(typefd[n->type->etype])
			o = n->fconst != 0;
		if(!true)
			o = !o;
		gbranch(OGOTO);
		if(o) {
			p1 = p;
			gbranch(OGOTO);
			patch(p1, pc);
		}
		goto com;

	case OCOMMA:
		cgen(l, Z);
		boolgen(r, true, nn);
		break;

	case ONOT:
		boolgen(l, !true, nn);
		break;

	case OCOND:
		bcgen(l, 1);
		p1 = p;
		bcgen(r->left, true);
		p2 = p;
		gbranch(OGOTO);
		patch(p1, pc);
		p1 = p;
		bcgen(r->right, !true);
		patch(p2, pc);
		p2 = p;
		gbranch(OGOTO);
		patch(p1, pc);
		patch(p2, pc);
		goto com;

	case OANDAND:
		if(!true)
			goto caseor;

	caseand:
		bcgen(l, true);
		p1 = p;
		bcgen(r, !true);
		p2 = p;
		patch(p1, pc);
		gbranch(OGOTO);
		patch(p2, pc);
		goto com;

	case OOROR:
		if(!true)
			goto caseand;

	caseor:
		bcgen(l, !true);
		p1 = p;
		bcgen(r, !true);
		p2 = p;
		gbranch(OGOTO);
		patch(p1, pc);
		patch(p2, pc);
		goto com;

	case OEQ:
	case ONE:
	case OLE:
	case OLT:
	case OGE:
	case OGT:
	case OHI:
	case OHS:
	case OLO:
	case OLS:
		nod = znode;
		nod.op = OREGISTER;
		nod.type = types[TLONG];
		regalloc(&nod, &nod, Z);
		cgen(n, &nod);
		gbranchk(true ? ABZ : ABNZ, &nod);
		regfree(&nod);
		goto com;
	}
	return;

com:
	return;
}

/*
 * structs: copied in memory (ACOPY), w bytes
 */
void
sugen(Node *n, Node *nn, long w)
{
	Node nod0, nod1, nod2, nod3, nod4, *l, *r;
	Type *t;
	long curs;

	if(n == Z || n->type == T)
		return;
	if(debug['g']) {
		prtree(nn, "sugen lhs");
		prtree(n, "sugen");
	}
	curs = cursafe;
	switch(n->op) {
	case OIND:
		if(nn == Z) {
			nullwarn(n->left, Z);
			break;
		}

	default:
	copy:
		if(nn == Z) {
			cgen(n, Z);
			break;
		}
		regialloc(&nod1, n, Z);
		lcgen(n, &nod1);
		regialloc(&nod2, nn, Z);
		lcgen(nn, &nod2);
		gins3(ACOPY, Kw, &nod1, nodconst(w), &nod2);
		regfree(&nod1);
		regfree(&nod2);
		break;

	case OCONST:
		goto copy;

	case ODOT:
		l = n->left;
		regsalloc(&nod1, l);
		sugen(l, &nod1, l->type->width);
		if(nn == Z)
			break;
		r = n->right;
		if(!r || r->op != OCONST) {
			diag(n, "DOT and no offset");
			break;
		}
		nod1.xoffset += (long)r->vconst;
		nod1.type = n->type;
		sugen(&nod1, nn, w);
		break;

	case OSTRUCT:
		/*
		 * rewrite so lhs has no fn call
		 */
		if(nn != Z && side(nn)) {
			nod1 = *n;
			nod1.type = typ(TIND, n->type);
			regialloc(&nod2, &nod1, Z);
			lcgen(nn, &nod2);
			regsalloc(&nod0, &nod1);
			gmove(&nod2, &nod0);
			regfree(&nod2);

			nod1 = *n;
			nod1.op = OIND;
			nod1.left = &nod0;
			nod1.right = Z;
			nod1.complex = 1;

			sugen(n, &nod1, w);
			return;
		}

		r = n->left;
		for(t = n->type->link; t != T; t = t->down) {
			l = r;
			if(r->op == OLIST) {
				l = r->left;
				r = r->right;
			}
			if(nn == Z) {
				cgen(l, nn);
				continue;
			}
			/*
			 * hand craft *(&nn + o) = l
			 */
			nod0 = znode;
			nod0.op = OAS;
			nod0.type = t;
			nod0.left = &nod1;
			nod0.right = l;

			nod1 = znode;
			nod1.op = OIND;
			nod1.type = t;
			nod1.left = &nod2;

			nod2 = znode;
			nod2.op = OADD;
			nod2.type = typ(TIND, t);
			nod2.left = &nod3;
			nod2.right = &nod4;

			nod3 = znode;
			nod3.op = OADDR;
			nod3.type = nod2.type;
			nod3.left = nn;

			nod4 = znode;
			nod4.op = OCONST;
			nod4.type = nod2.type;
			nod4.vconst = t->offset;

			ccom(&nod0);
			acom(&nod0);
			xcom(&nod0);
			nod0.addable = 0;

			cgen(&nod0, Z);
		}
		break;

	case OAS:
		if(nn == Z) {
			if(n->addable < INDEXED)
				sugen(n->right, n->left, w);
			break;
		}
		sugen(n->right, n->left, w);
		sugen(n->left, nn, w);
		break;

	case OFUNC:
		if(nn == Z) {
			regsalloc(&nod1, n);
			nn = &nod1;
		}
		regialloc(&nod2, nn, Z);
		lcgen(nn, &nod2);
		gcall(n, Z, &nod2);
		regfree(&nod2);
		break;

	case OCOND:
		bcgen(n->left, 1);
		{
			Prog *p1;

			p1 = p;
			sugen(n->right->left, nn, w);
			gbranch(OGOTO);
			patch(p1, pc);
			p1 = p;
			sugen(n->right->right, nn, w);
			patch(p1, pc);
		}
		break;

	case OCOMMA:
		cgen(n->left, Z);
		sugen(n->right, nn, w);
		break;
	}
	cursafe = curs;
}
