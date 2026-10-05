#include	"../cc/cc.h"
#include	"../3c/3.out.h"

/*
 * 3c/wasm32
 * WebAssembly, 32-bit addresses (Plan2001)
 */
#define	SZ_CHAR		1
#define	SZ_SHORT	2
#define	SZ_INT		4
#define	SZ_LONG		4
#define	SZ_IND		4
#define	SZ_FLOAT	4
#define	SZ_VLONG	8
#define	SZ_DOUBLE	8
#define	FNX		100

#define	REGARG		(-1)	/* every argument in memory: 8c's way */

typedef	struct	Adr	Adr;
typedef	struct	Prog	Prog;
typedef	struct	Case	Case;
typedef	struct	C1	C1;

struct	Adr
{
	vlong	offset;
	double	dval;
	char	sval[NSNAME];

	Sym*	sym;
	long	reg;
	uchar	type;
	uchar	index;
	uchar	etype;
};
#define	A	((Adr*)0)

#define	INDEXED	9
struct	Prog
{
	Adr	from;
	Adr	from2;
	Adr	to;
	Prog*	link;
	long	lineno;
	short	as;
	uchar	k;	/* class of the operation */
	uchar	k2;	/* ACVT: class of from */
};
#define	P	((Prog*)0)

struct	Case
{
	Case*	link;
	vlong	val;
	long	label;
	char	def;
	char	isv;
};
#define	C	((Case*)0)

struct	C1
{
	vlong	val;
	long	label;
};

EXTERN	long	breakpc;
EXTERN	long	nbreak;
EXTERN	Case*	cases;
EXTERN	Node	constnode;
EXTERN	Node	fconstnode;
EXTERN	Node	vconstnode;
EXTERN	long	continpc;
EXTERN	long	curarg;
EXTERN	long	cursafe;
EXTERN	Prog*	firstp;
EXTERN	Prog*	lastp;
EXTERN	long	maxargsafe;
EXTERN	int	mnstring;
EXTERN	Node*	nodret;
EXTERN	Node*	nodsafe;
EXTERN	long	nstring;
EXTERN	Prog*	p;
EXTERN	long	pc;
EXTERN	Node	regnode;
EXTERN	char	string[NSNAME];
EXTERN	Node	znode;
EXTERN	Prog	zprog;
EXTERN	int	suppress;

extern	char*	anames[];

/*
 * sgen.c
 */
void	codgen(Node*, Node*);
void	gen(Node*);
void	noretval(int);
void	usedset(Node*, int);
void	xcom(Node*);
int	bcomplex(Node*, Node*);

/*
 * cgen.c
 */
void	cgen(Node*, Node*);
void	lcgen(Node*, Node*);
void	reglcgen(Node*, Node*, Node*);
void	bcgen(Node*, int);
void	boolgen(Node*, int, Node*);
void	sugen(Node*, Node*, long);

/*
 * txt.c
 */
void	ginit(void);
void	gclean(void);
void	nextpc(void);
void	gargs(Node*, Node*, Node*, long);
Node*	nodconst(long);
Node*	nodfconst(double);
Node*	nodgconst(vlong, Type*);
int	nodreg(Node*, Node*, int);
void	regret(Node*, Node*);
void	regalloc(Node*, Node*, Node*);
void	regfree(Node*);
void	regialloc(Node*, Node*, Node*);
void	regsalloc(Node*, Node*);
void	regaalloc(Node*, Node*);
void	regind(Node*, Node*);
void	naddr(Node*, Adr*);
void	gmove(Node*, Node*);
void	gins(int a, Node*, Node*);
void	gins3(int, int, Node*, Node*, Node*);
void	gcmp(int, Node*, vlong);
void	gbranch(int);
void	gbranchk(int, Node*);
void	patch(Prog*, long);
void	gpseudo(int, Sym*, Node*);
int	kind(Type*);
int	regkind(Node*);
void	regopt(Prog*);

/*
 * swt.c
 */
int	swcmp(const void*, const void*);
void	doswit(Node*);
void	swit1(C1*, int, long, Node*);
void	casf(void);
void	bitload(Node*, Node*, Node*, Node*, Node*);
void	bitstore(Node*, Node*, Node*, Node*, Node*);
long	outstring(char*, long);
void	nullwarn(Node*, Node*);
void	sextern(Sym*, Node*, long, long);
void	gextern(Sym*, Node*, long, long);
void	outcode(void);
void	ieeedtod(Ieee*, double);

/*
 * list
 */
void	listinit(void);
int	Pconv(Fmt*);
int	Aconv(Fmt*);
int	Dconv(Fmt*);
int	Sconv(Fmt*);
int	Rconv(Fmt*);

/*
 * com64 (not used: i64 is the machine's)
 */
int	cond(int);
int	com64(Node*);
void	com64init(void);
void	bool64(Node*);

#pragma	varargck	type	"A"	int
#pragma	varargck	type	"D"	Adr*
#pragma	varargck	type	"P"	Prog*
#pragma	varargck	type	"R"	int
#pragma	varargck	type	"S"	char*
