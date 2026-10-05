/*
 * wasm32 (docs/architecture.md, phase C): no MMU, no paging; the kernel's
 * memory is the platform's shared memory, its addresses physical, from 0
 */
#define KiB		1024u
#define MiB		1048576u
#define GiB		1073741824u

#define MIN(a, b)	((a) < (b)? (a): (b))
#define MAX(a, b)	((a) > (b)? (a): (b))
#define ALIGNED(p, a)	(!(((uintptr)(p)) & ((a)-1)))

#define	BI2BY		8
#define	BI2WD		32
#define	BY2WD		4
#define	BY2V		8
#define	BY2PG		4096
#define	WD2PG		(BY2PG/BY2WD)
#define	PGSHIFT		12
#define	ROUND(s, sz)	(((s)+((sz)-1))&~((sz)-1))
#define	PGROUND(s)	ROUND(s, BY2PG)
#define	BLOCKALIGN	8
#define	FPalign		8

#define	MAXMACH		64		/* Workers that are CPUs */
#define KSTACK		(64*KiB)	/* a Proc's kernel stack: its Worker's */

#define HZ		(100)
#define MS2HZ		(1000/HZ)
#define TK2SEC(t)	((t)/HZ)

/* no user address space in the kernel's: a process's memory is its own (platform.js) */
#define UTZERO		0
#define USTKTOP		0
#define USTKSIZE	(1*MiB)
#define KZERO		0
#define KTZERO		0

/* port/'s segments and pages: unused here, but in its types */
#define PTEMAPMEM	(1*MiB)
#define PTEPERTAB	(PTEMAPMEM/BY2PG)
#define SEGMAPSIZE	1984
#define SSEGMAPSIZE	16
#define PPN(x)		((x)&~(BY2PG-1))
