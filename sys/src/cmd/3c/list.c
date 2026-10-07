#define EXTERN
#include "gc.h"

static	char	*kname[NK] = { "w", "v", "f", "d" };

void
listinit(void)
{

	fmtinstall('A', Aconv);
	fmtinstall('P', Pconv);
	fmtinstall('S', Sconv);
	fmtinstall('D', Dconv);
	fmtinstall('R', Rconv);
}

int
Pconv(Fmt *fp)
{
	char str[STRINGSZ];
	Prog *p;
	char *k;

	p = va_arg(fp->args, Prog*);
	k = "";
	switch(p->as) {
	case ACVT:
	case ACVTU:
		snprint(str, sizeof str, "(%ld)	%A.%s%s	%D,%D", p->lineno, p->as,
			kname[p->k2], kname[p->k], &p->from, &p->to);
		return fmtstrcpy(fp, str);
	case AADD: case ASUB: case AMUL: case ADIV: case ADIVU: case AMOD: case AMODU:
	case AAND: case AOR: case AXOR: case ASHL: case ASHR: case ASHRU:
	case ACMPEQ: case ACMPNE: case ACMPLT: case ACMPLE: case ACMPGT: case ACMPGE:
	case ACMPLO: case ACMPLS: case ACMPHI: case ACMPHS:
		k = kname[p->k];
		break;
	}
	if(p->from2.type != D_NONE)
		snprint(str, sizeof str, "(%ld)	%A%s%s	%D,%D,%D", p->lineno, p->as,
			*k ? "." : "", k, &p->from, &p->from2, &p->to);
	else
		snprint(str, sizeof str, "(%ld)	%A%s%s	%D,%D", p->lineno, p->as,
			*k ? "." : "", k, &p->from, &p->to);
	return fmtstrcpy(fp, str);
}

int
Aconv(Fmt *fp)
{
	int i;

	i = va_arg(fp->args, int);
	if(i < 0 || i >= ALAST)
		return fmtprint(fp, "A?%d", i);
	return fmtstrcpy(fp, anames[i]);
}

int
Dconv(Fmt *fp)
{
	char str[STRINGSZ], s[STRINGSZ];
	Adr *a;
	int i;

	a = va_arg(fp->args, Adr*);
	i = a->type;
	switch(i) {
	default:
		snprint(str, sizeof str, "type=%d", i);
		break;

	case D_NONE:
		str[0] = 0;
		break;

	case D_REG:
		snprint(str, sizeof str, "%R", (int)a->reg);
		break;

	case D_OREG:
		snprint(str, sizeof str, "%lld(%R)", a->offset, (int)a->reg);
		break;

	case D_EXTERN:
		snprint(str, sizeof str, "%s+%lld(SB)", a->sym->name, a->offset);
		break;

	case D_STATIC:
		snprint(str, sizeof str, "%s<>+%lld(SB)", a->sym->name, a->offset);
		break;

	case D_AUTO:
		if(a->sym != S)
			snprint(str, sizeof str, "%s+%lld(SP)", a->sym->name, a->offset);
		else
			snprint(str, sizeof str, "%lld(SP)", a->offset);
		break;

	case D_PARAM:
		if(a->sym != S)
			snprint(str, sizeof str, "%s+%lld(FP)", a->sym->name, a->offset);
		else
			snprint(str, sizeof str, "%lld(FP)", a->offset);
		break;

	case D_SP:
		snprint(str, sizeof str, "%lld(ARG)", a->offset);
		break;

	case D_CONST:
		snprint(str, sizeof str, "$%lld", a->offset);
		break;

	case D_FCONST:
		snprint(str, sizeof str, "$(%.17g)", a->dval);
		break;

	case D_SCONST:
		snprint(str, sizeof str, "$\"%S\"", a->sval);
		break;

	case D_BRANCH:
		snprint(str, sizeof str, "%lld(PC)", a->offset);
		break;

	case D_ADDR:
		a->type = a->index;
		a->index = D_NONE;
		snprint(s, sizeof s, "%D", a);
		a->index = a->type;
		a->type = D_ADDR;
		snprint(str, sizeof str, "$%s", s);
		break;
	}
	return fmtstrcpy(fp, str);
}

int
Rconv(Fmt *fp)
{
	char str[STRINGSZ];
	int r;

	r = va_arg(fp->args, int);
	switch(RNUM(r)) {
	case NREGRET:
		snprint(str, sizeof str, "RET.%s", kname[RCLASS(r)]);
		break;
	case NREGSP:
		snprint(str, sizeof str, "SP");
		break;
	default:
		snprint(str, sizeof str, "R%d.%s", RNUM(r), kname[RCLASS(r)]);
		break;
	}
	return fmtstrcpy(fp, str);
}

int
Sconv(Fmt *fp)
{
	int i, c;
	char str[STRINGSZ], *p, *a;

	a = va_arg(fp->args, char*);
	p = str;
	for(i=0; i<NSNAME; i++) {
		c = a[i] & 0xff;
		if(c >= 'a' && c <= 'z' ||
		   c >= 'A' && c <= 'Z' ||
		   c >= '0' && c <= '9' ||
		   c == ' ' || c == '%') {
			*p++ = c;
			continue;
		}
		*p++ = '\\';
		switch(c) {
		case 0:
			*p++ = 'z';
			continue;
		case '\\':
		case '"':
			*p++ = c;
			continue;
		case '\n':
			*p++ = 'n';
			continue;
		case '\t':
			*p++ = 't';
			continue;
		}
		*p++ = (c>>6) + '0';
		*p++ = ((c>>3) & 7) + '0';
		*p++ = (c & 7) + '0';
	}
	*p = 0;
	return fmtstrcpy(fp, str);
}
