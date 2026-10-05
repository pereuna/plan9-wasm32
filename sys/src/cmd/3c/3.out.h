/*
 * 3c, 3l: WebAssembly (objtype wasm32, objects .3) - Plan2001.
 *
 * The machine is an abstract one with any number of typed registers -
 * 3l makes them WebAssembly locals, and the engine's compiler allocates
 * real registers - and memory: ILP32, little-endian, the stack in
 * memory, growing down from SP.  Instructions are three-address:
 *	op from, from2, to	to = from op from2
 * Only the MOVs touch memory, with their width; the rest work on
 * registers and constants of the instruction's class (Prog.k).  Control
 * flow is jumps, as on any machine: 3l makes WebAssembly's structured
 * blocks of them.
 */
#define	NSNAME	8
#define	NSYM	50

/* register classes: the WebAssembly value types */
enum
{
	Kw	= 0,	/* i32: char, short, int, long, pointers */
	Kv,		/* i64: vlong */
	Kf,		/* f32: float */
	Kd,		/* f64: double */
	NK,
};

/*
 * registers: (n<<2)|class; n<4 are fixed
 */
#define	RCLASS(r)	((r)&3)
#define	RNUM(r)		((r)>>2)
#define	MKREG(n, k)	(((n)<<2)|(k))
enum
{
	NREGRET	= 0,	/* the return value: 3l's globals, one per class */
	NREGSP	= 1,	/* the stack pointer, a global; in a function its local */
	NREGEXT	= 2,	/* 2, 3: extern register (the kernel's m, up): globals, each Worker's own */
	NREGFIRST	= 4,	/* the first allocatable n */

	REGSP	= (NREGSP<<2)|Kw,
};

enum	as
{
	AXXX,
	AGOK,
	ANOP,
	ATEXT,		/* from sym, to $framesize */
	AGLOBL,		/* from sym, to $size */
	ADATA,		/* from sym+off, from2 $width, to value */
	ANAME,
	AHISTORY,
	AEND,

	AMOVB,		/* memory widths; registers B, BU, H, HU, W are Kw */
	AMOVBU,
	AMOVH,
	AMOVHU,
	AMOVW,
	AMOVV,
	AMOVF,
	AMOVD,

	ACVT,		/* to (class k) = from (class Prog.k2); signed */
	ACVTU,		/* unsigned: the integer side */

	AADD,
	ASUB,
	AMUL,
	ADIV,
	ADIVU,
	AMOD,
	AMODU,
	AAND,
	AOR,
	AXOR,
	ASHL,
	ASHR,
	ASHRU,

	ACMPEQ,		/* to (Kw) = from rel from2, class k */
	ACMPNE,
	ACMPLT,
	ACMPLE,
	ACMPGT,
	ACMPGE,
	ACMPLO,		/* unsigned */
	ACMPLS,
	ACMPHI,
	ACMPHS,

	AJMP,		/* to branch */
	ABNZ,		/* from (Kw), to branch */
	ABZ,
	ACALL,		/* to: sym (direct) or Kw register (function pointer) */
	ARET,
	ACOPY,		/* memory: to address (Kw) = from address (Kw), from2 $size */

	ALAST
};

enum
{
	D_NONE	= 0,
	D_REG,		/* reg: (n<<2)|class */
	D_CONST,	/* offset */
	D_FCONST,	/* dval */
	D_SCONST,	/* sval */
	D_EXTERN,	/* memory at sym+offset */
	D_STATIC,
	D_AUTO,		/* memory at the frame's top + offset (offset < 0) */
	D_PARAM,	/* memory at the frame's top + offset (offset >= 0) */
	D_SP,		/* memory at SP + offset: the outgoing arguments */
	D_OREG,		/* memory at reg + offset */
	D_ADDR,		/* the address of what index (one of the above) says */
	D_BRANCH,	/* offset: the pc */
	D_FILE,
	D_FILE1,

	D_LAST
};

/*
 * .3 objects: records, each starting with its as (2 bytes, little-endian)
 *	ANAME	type (1) symno (2) name\0
 *	other	k (1) k2 (1) lineno (4) from from2 to
 * an address:
 *	type (1); D_NONE: nothing more; else index (1) reg (4) sym (2)
 *	offset (8), D_FCONST: the IEEE double (8), D_SCONST: sval (8)
 * all little-endian.  A file starts with the line "wasm32 object\n".
 */
#define	OBJMAGIC	"wasm32 object\n"

typedef	struct	ieee	Ieee;
struct	ieee
{
	long	l;	/* contains ls-man	0xffffffff */
	long	h;	/* contains sign	0x80000000
				    exp		0x7ff00000
				    ms-man	0x000fffff */
};
