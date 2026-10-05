/*
 * wasm32: the platform's functions (platform.js) - the kernel calls them
 * as any C function, its arguments in memory; 3l -k imports them
 */
void	eiaout(void*, int);		/* #t/eia0: bytes to the page */
void	eiaring(void*);			/* and from it: the page writes into this ring (uartwasm32.c) */
enum { Wproc, Wfork, Whelper, Wrfmem };	/* what a Worker is for: a proc, a fork's child, a helper, an rfork(RFMEM) child's (the page's tests) */
void	platnewproc(void (*)(void*), void*, void*, long*, int);	/* fn(arg) on a Worker of its own, its stack's top; -1 in the word if the page can not make it; what for */
int	platwait(long*, long, long);	/* Atomics.wait(addr, val, ms; -1 for ever): 0 woken, 1 timed out, 2 not val */
int	platwake(long*, int);		/* Atomics.notify(addr, n) */
vlong	platnsec(void);			/* nanoseconds since 1970 */
void	platrandom(void*, ulong);	/* crypto.getRandomValues */
void	platlog(char*);			/* early, before #t/eia0: on the page's console (KLOG) */
uvlong	platmemsize(void);		/* the kernel's memory: its bytes now (WebAssembly.Memory's buffer) */
void	plathalt(char*);			/* the machine stops: the page says so */

/* the program on this proc's Worker: its own module and memory (trap.c, sysproc.c) */
int	platexec(void*, long, void*, long, int);	/* the next program: its module's bytes, argv's strings, argc; -1 not a module */
typedef struct Ufns Ufns;
struct Ufns		/* the kernel's functions for the program's Worker (trap.c) */
{
	vlong	(*syscall)(int, ulong);	/* a call, all of it */
	int	(*sysprep)(Proc*, int, ulong);	/* rfork(RFMEM): a proc's call in, done (1) or to its helper (0) */
	int	(*sysdone)(Proc*);		/* 0 not yet, 1 done, 2 the proc ended */
	int	(*sysfin)(Proc*);		/* its results out; 1 a note ends it on its helper */
	vlong	(*sysret)(Proc*);		/* its result */
	void	(*coend)(Proc*);		/* the memory's Worker is done with the proc */
	void	(*memend)(Umem*);		/* and with the memory's group */
};
_Noreturn void	platuser(Ufns*, Proc*, ulong);	/* run the program (up, its pid), and each one exec makes next */
int	platcopyin(void*, ulong, long);	/* from the program's memory: -1 not there */
int	platcopyout(ulong, void*, long);	/* to it */
long	platustrlen(ulong, long);		/* a string's length there, at most the second; -1 none */
int	platbrk(ulong);			/* its memory to the address at least: -1 can not */
void	platfork(Proc*, int (*)(Proc*), ulong);	/* it unwinds, its memory the child's, the function readies the child (-1: none), both rewind (pid or -1, 0) */
void	platrfmem(Proc*, int (*)(Proc*), ulong, Umem*, long*);	/* rfork(RFMEM): it unwinds; the function makes the child's helper and the forking proc's if it has none (-1: undone); both rewind; the memory's group and word */
void	platnote(void*, char*, void (*)(Proc*), Proc*);	/* the proc's notify handler gets the note when the call returns; the function when it is done */
void	platnoted(void);		/* noted(NCONT): back out of the handler */

/* the screen and the mouse (screen.c) */
/* the framebuffer is the page's, in BootInfo (fbbase ...): the kernel draws there and says what changed */
void	platflush(int, int, int, int);	/* this rectangle of it changed */
void	platcursor(int, int, uchar*, uchar*);	/* the cursor: its offset, clr and set (16x16, 2 bytes a line) */
void	platkbdring(void*);			/* the page's keyboard messages into the ring: r, w, b[4096] (devkbd.c) */
void	platmousering(void*);
/* the network (devwsnet.c): WebSockets to the machine's webterm, the page's */
void	platnetopen(int, ulong, char*, void*, long*);	/* conversation n, its gen: the path (/17019), its ring (r, w, closed, b[64K]; the page empties it), its word (1 open, -1 not) */
void	platnetsend(int, ulong, void*, long);	/* bytes out, if (n, gen) is still the page's */
void	platnetclose(int, ulong);		/* hang up (n, gen) */		/* the page's mouse events into the ring: w, r, ev[64][4] (x, y, buttons, msec) */
/* WebAuthn (devwebauthn.c): request gen to the page, which shows a button; its answer into buf (at most n), then its length in the word (-1 none); a nil request takes the page's button away */
void	platwebauthn(ulong, char*, char*, long, long*);
