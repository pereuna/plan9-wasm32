#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

/*
 * wasm32: #t/eia0, the platform's serial port, a PhysUart for devuart.c.
 * Out: what it has staged goes to eiaout.  In: the page writes into a
 * ring here (eiaring tells the platform where) and wakes eia0's kproc,
 * which gives the bytes to uartrecv.
 */
enum { Nring = 8192 };	/* a power of 2: the counters run on modulo 2^32, the index is masked */

typedef struct Ring Ring;
struct Ring
{
	ulong	r;
	ulong	w;
	uchar	b[Nring];
};

static	Ring	ring;
static	Uart	eia0;
extern	PhysUart	wasm32physuart;

static Uart*
pnp(void)
{
	eia0.regs = &ring;
	eia0.name = "eia0";
	eia0.freq = 1843200;
	eia0.phys = &wasm32physuart;
	eia0.console = 1;
	eia0.special = 0;
	return &eia0;
}

static void
inproc(void *v)
{
	Uart *u;
	ulong w;

	u = v;
	for(;;){
		w = ring.w;
		while(ring.r == w){
			platwait((long*)&ring.w, w, -1);
			w = ring.w;
		}
		while(ring.r != w){
			uartrecv(u, ring.b[ring.r & (Nring-1)]);
			ring.r++;
		}
	}
}

static void
enable(Uart *u, int)
{
	static int started;

	u->cts = 1;
	if(!started){
		started = 1;
		eiaring(&ring);
		kproc("eia0", inproc, u);
	}
}

static void
disable(Uart*)
{
}

static void
kick(Uart *u)
{
	long n;

	while(u->op < u->oe || uartstageoutput(u)){
		n = u->oe - u->op;
		eiaout(u->op, n);
		u->op += n;
	}
}

static void	dobreak(Uart*, int) { }
static int	baud(Uart *u, int b) { u->baud = b; return 0; }
static int	bits(Uart *u, int b) { u->bits = b; return 0; }
static int	stop(Uart *u, int s) { u->stop = s; return 0; }
static int	parity(Uart *u, int p) { u->parity = p; return 0; }
static void	modemctl(Uart*, int) { }
static void	rts(Uart*, int) { }
static void	dtr(Uart*, int) { }
static void	fifo(Uart*, int) { }

static char*
status(Uart *u, char *p, char *e)
{
	return seprint(p, e, "b%d c0 d0 e0 l%d m0 p%c r0 s%d i0\n",
		u->baud, u->bits, u->parity, u->stop);
}

static int
getc(Uart*)
{
	int c;
	ulong w;

	while((w = ring.w) == ring.r)
		platwait((long*)&ring.w, w, -1);
	c = ring.b[ring.r & (Nring-1)];
	ring.r++;
	return c;
}

static void
putc(Uart*, int c)
{
	uchar b;

	b = c;
	eiaout(&b, 1);
}

PhysUart wasm32physuart = {
	.name		= "wasm32",
	.pnp		= pnp,
	.enable		= enable,
	.disable	= disable,
	.kick		= kick,
	.dobreak	= dobreak,
	.baud		= baud,
	.bits		= bits,
	.stop		= stop,
	.parity		= parity,
	.modemctl	= modemctl,
	.rts		= rts,
	.dtr		= dtr,
	.status		= status,
	.fifo		= fifo,
	.getc		= getc,
	.putc		= putc,
};
