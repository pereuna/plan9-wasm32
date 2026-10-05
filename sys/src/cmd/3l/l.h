#include	<u.h>
#include	<libc.h>
#include	<bio.h>
#include	"../3c/3.out.h"

#ifndef	EXTERN
#define	EXTERN	extern
#endif

/*
 * 3l: .3 objects and libraries to a WebAssembly module (Plan2001,
 * objtype wasm32).  The module is a process: its own memory, every
 * function of type () -> () (arguments in memory at SP, results in the
 * RET globals), and one import, plan9.syscall(number, args) -> i64.
 */

typedef	struct	Adr	Adr;
typedef	struct	Prog	Prog;
typedef	struct	Sym	Sym;
typedef	struct	Rel	Rel;
typedef	struct	Buf	Buf;

struct	Adr
{
	vlong	offset;
	double	dval;
	char	sval[NSNAME];
	Sym*	sym;
	long	reg;
	uchar	type;
	uchar	index;
};

struct	Prog
{
	Adr	from;
	Adr	from2;
	Adr	to;
	Prog*	link;	/* the function's next */
	Prog*	targ;	/* a branch's */
	long	lineno;
	short	as;
	uchar	k;
	uchar	k2;
	int	blk;	/* basic block number */
	char	leader;
};

enum
{
	SNONE,
	STEXT,
	SDATA,
	SBSS,
	SSYNTH,		/* made here: _trap, longjmp, the atomics */
	SINLINE,	/* made in place at each call: setjmp */
	SIMPORT,	/* -k: the platform's, a function the module imports */
};

struct	Rel	/* in data: the address of sym+add, or its function pointer */
{
	long	off;
	int	width;
	Sym*	sym;
	vlong	add;
	Rel*	link;
};

struct	Sym
{
	char*	name;
	int	version;	/* 0 extern, else the object's number (statics) */
	int	type;
	int	ref;		/* referred to */
	long	size;
	long	value;		/* data: its address */
	int	fn;		/* text: function index */
	int	tab;		/* text: its pointer (table index) */
	int	unwind;		/* text: can be unwound and rewound (fork) */
	Prog*	text;		/* the TEXT */
	uchar*	data;
	Rel*	rel;
	char*	file;
	Sym*	hash;
	Sym*	next;		/* all symbols */
};

struct	Buf
{
	uchar*	p;
	long	n;
	long	max;
};

enum
{
	NHASH	= 10007,
	STACK	= 1024*1024,
	GUARD	= 4096,
	PAGE	= 65536,

	/* globals */
	GSP	= 0,
	GRET,		/* GRET+k: RET of class k */
	GSTATE	= GRET+NK,	/* unwinding 1, rewinding 2 (fork) */
	GASPTR,		/* the top of the saved frames */
	GASRET,		/* what the rewound _trap returns */

	ASAREA	= 256*1024,	/* the saved frames */
	GPREEMPT = GASRET+5,	/* back edges until _yield (after stacktop asbase perproc perprocsize) */
	GEXT	= GPREEMPT+1,	/* 2: extern register (3c), each instance's own: the kernel's m, up */

	APREEMPT = ALAST+1,	/* 3l's own: GPREEMPT--, and on to targ while it is > 0 */
};

EXTERN	Sym*	hash[NHASH];
EXTERN	Sym*	allsym;
EXTERN	Sym*	lastsym;
EXTERN	int	nerrors;
EXTERN	char*	outfile;
EXTERN	char*	entry;
EXTERN	long	stacksize;
EXTERN	int	version;
EXTERN	char	debug[128];
EXTERN	Sym**	funcs;
EXTERN	int	nfuncs;

/* obj.c */
Sym*	lookup(char*, int);
void	diag(char*, ...);
void	ldfile(char*);
void	ldlibs(void);
void	applydata(void);
void*	emalloc(long);

/* wasm.c */
void	layout(void);
void	asmb(void);

/* buf */
void	bput1(Buf*, int);
void	bputn(Buf*, void*, long);
void	buleb(Buf*, uvlong);
void	bsleb(Buf*, vlong);
void	bfree(Buf*);

#pragma	varargck	argpos	diag	1
