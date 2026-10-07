#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"
#include	"../port/netif.h"
#include	"../port/etherif.h"

/*
 * #l/ether0: wasm32's Ethernet card (docs/kernel.md), as a virtual
 * machine's: its frames go whole over a WebSocket of the page's to the
 * machine's gateway, which puts them on a network (a TAP, a switch: the
 * gateway's), and that network's frames come back the same way.  9front's
 * devether and IP stack over it as over any card: ipconfig, DHCP, ARP,
 * TCP and the rest are 9front's own.
 *
 * The page writes what comes in into a ring here (platether): each frame
 * its length, two bytes little-endian, then its bytes; a frame the ring
 * has no room for is dropped, as a full card's would be.  A kproc gives
 * them to devether.  What goes out goes to the page a frame at a time
 * (platethersend), which drops it if the WebSocket is not open or is
 * full.  The link is up while the gateway's WebSocket is open.  The
 * wire is not trusted: a frame shorter than its header or longer than
 * ETHERMAXTU is dropped either way (framing errors in, output errors
 * out), never cut to size.
 *
 * plan9.ini: ether0=type=wasm32 ea=... (the page's, the same each time
 * in one browser: the firmware's)
 */
enum
{
	Nring	= 256*1024,	/* a power of 2: the counters run modulo 2^32, the index masked */
	Mtu	= ETHERMAXTU,
};

typedef struct Ering Ering;
struct Ering
{
	ulong	r;		/* ours: bytes read */
	ulong	w;		/* the page's: bytes written, whole frames */
	long	link;		/* the page's: 1 the gateway's WebSocket is open */
	long	drops;		/* the page's: frames the ring had no room for */
	uchar	b[Nring];
};

typedef struct Ctlr Ctlr;
struct Ctlr
{
	Ering	*in;
	Lock	tlock;		/* transmit: one at a time, in order */
	uchar	buf[Mtu];
};

static uchar
ringbyte(Ering *r, ulong i)
{
	return r->b[i & (Nring-1)];
}

static void
etherin(void *arg)
{
	Ether *ether;
	Ctlr *ctlr;
	Ering *r;
	Block *bp;
	ulong w, n, i;

	ether = arg;
	ctlr = ether->ctlr;
	r = ctlr->in;
	while(ether->f == nil)	/* devether's netifinit, after reset */
		tsleep(&up->sleep, return0, nil, 10);
	for(;;){
		ethersetlink(ether, r->link > 0);
		w = r->w;
		if(r->r == w){
			platwait((long*)&r->w, w, 1000);
			continue;
		}
		while(r->r != w){
			n = ringbyte(r, r->r) | ringbyte(r, r->r+1)<<8;
			if(n > w - r->r - 2){	/* not the page's: never */
				print("#l%d: wasm32: bad frame in the ring\n", ether->ctlrno);
				r->r = w;
				break;
			}
			if(n < ETHERHDRSIZE || n > Mtu){
				ether->frames++;
				r->r += 2+n;
				continue;
			}
			bp = allocb(n);
			for(i = 0; i < n; i++)
				bp->wp[i] = ringbyte(r, r->r+2+i);
			bp->wp += n;
			coherence();
			r->r += 2+n;
			etheriq(ether, bp);
		}
	}
}

static void
transmit(Ether *ether)
{
	Ctlr *ctlr;
	Block *bp;
	long n;

	ctlr = ether->ctlr;
	lock(&ctlr->tlock);
	while((bp = qget(ether->oq)) != nil){
		n = BLEN(bp);
		if(n < ETHERHDRSIZE || n > Mtu)
			ether->oerrs++;
		else if(ctlr->in->link > 0)
			platethersend(bp->rp, n);
		freeb(bp);
	}
	unlock(&ctlr->tlock);
}

static char*
ifstat(void *arg, char *p, char *e)
{
	Ether *ether;
	Ering *r;

	ether = arg;
	r = ((Ctlr*)ether->ctlr)->in;
	return seprint(p, e, "gateway: %s\ndrops: %ld\n", r->link > 0 ? "open" : "closed", r->drops);
}

static void
promiscuous(void*, int)
{
}

static void
multicast(void*, uchar*, int)
{
}

static int
reset(Ether *ether)
{
	static int once;
	Ctlr *ctlr;
	uchar zea[Eaddrlen];

	/* one card: the page's one WebSocket */
	if(once)
		return -1;
	once = 1;
	ctlr = mallocz(sizeof(Ctlr), 1);
	if(ctlr == nil || (ctlr->in = mallocz(sizeof(Ering), 1)) == nil){
		free(ctlr);
		return -1;
	}
	memset(zea, 0, Eaddrlen);
	if(memcmp(ether->ea, zea, Eaddrlen) == 0){
		/* no ea= from the firmware: one of our own, locally administered */
		platrandom(ether->ea, Eaddrlen);
		ether->ea[0] = (ether->ea[0] & ~1) | 2;
	}
	ether->ctlr = ctlr;
	ether->port = 0;
	ether->irq = -1;
	ether->mbps = 1000;
	ether->maxmtu = Mtu;
	ether->transmit = transmit;
	ether->ifstat = ifstat;
	ether->promiscuous = promiscuous;
	ether->multicast = multicast;
	ether->arg = ether;
	platether(ctlr->in);
	/* from the start, not at attach: the link is the gateway's, known before /net/ether0 is opened */
	kproc("etherwasm32", etherin, ether);
	return 0;
}

void
etherwasm32link(void)
{
	addethercard("wasm32", reset);
}
