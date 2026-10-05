/*
 * Plan 9's libc.h for 9front's cc (6c) on POSIX and Emscripten:
 * drawterm's portable lib.h (types, fmt, utf, print) and the few system
 * calls cc uses, on POSIX.  open, read, write, close and remove take the
 * same arguments as POSIX's; the others are renamed.
 */
#include "lib.h"
#include "args.h"

#define	IOUNIT	(32*1024)

#define	DMTMP	0x04000000

#define	AEXIST	0
#define	AEXEC	1
#define	AWRITE	2
#define	AREAD	4

#define	create	p9create
#define	seek	p9seek
#define	exits	p9exits
#define	errstr	p9errstr
#define	dup	p9dup
#define	getwd	p9getwd
#define	qsort	p9qsort

extern	int	p9create(char*, int, ulong);
extern	vlong	p9seek(int, vlong, int);
extern	void	p9exits(char*);
extern	int	p9errstr(char*, uint);
extern	int	p9dup(int, int);
extern	char*	p9getwd(char*, int);
extern	void	p9qsort(void*, unsigned long, unsigned long, int (*)(const void*, const void*));
extern	void	werrstr(char*, ...);
#define	charstod	fmtcharstod
extern	double	charstod(int(*)(void*), void*);

#define	setmalloctag(p, pc)
#define	setrealloctag(p, pc)

/* cc's own getc and round (cc.h), not stdio's and math.h's */
#undef	getc
#define	getc	cc_getc
#define	round	cc_round

extern	int	libatexit(void(*)(void));
extern	int	fmtrune(Fmt*, int);

#define	isInf	__isInf
#define	isNaN	__isNaN
#define	NaN	__NaN
#define	Inf	__Inf
