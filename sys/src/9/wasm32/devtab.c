#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"

/*
 * wasm32's configuration: what mkdevc makes from a kernel's config file
 * on the other machines
 */
extern	Dev	rootdevtab, consdevtab, envdevtab, pipedevtab, dupdevtab;
extern	Dev	srvdevtab, mntdevtab, uartdevtab, rootfsdevtab, procdevtab, drawdevtab, mousedevtab, kbddevtab, etherdevtab, ipdevtab, tlsdevtab, sdwdevtab, webauthndevtab, swapdevtab;

Dev*	devtab[] = {
	&rootdevtab,
	&consdevtab,
	&envdevtab,
	&pipedevtab,
	&dupdevtab,
	&srvdevtab,
	&mntdevtab,
	&uartdevtab,
	&rootfsdevtab,
	&procdevtab,
	&drawdevtab,
	&mousedevtab,
	&kbddevtab,
	&etherdevtab,
	&ipdevtab,
	&tlsdevtab,
	&sdwdevtab,
	&webauthndevtab,
	&swapdevtab,
	nil,
};

extern	PhysUart	wasm32physuart;
PhysUart*	physuart[] = { &wasm32physuart, nil };

char*	conffile = "wasm32";
ulong	kerndate = 1790000000;

void
bootlinks(void)
{
}

extern void ethermediumlink(void);
extern void loopbackmediumlink(void);
extern void netdevmediumlink(void);
extern void pktmediumlink(void);
extern void etherwasm32link(void);

void
links(void)
{
	bootlinks();
	ethermediumlink();
	loopbackmediumlink();
	netdevmediumlink();
	pktmediumlink();
	etherwasm32link();
}

#include "../ip/ip.h"
extern void tcpinit(Fs*);
extern void udpinit(Fs*);
extern void rudpinit(Fs*);
extern void ipifcinit(Fs*);
extern void icmpinit(Fs*);
extern void icmp6init(Fs*);
extern void igmpinit(Fs*);
extern void greinit(Fs*);
extern void ipmuxinit(Fs*);
extern void ilinit(Fs*);
void (*ipprotoinit[])(Fs*) = {
	tcpinit,
	udpinit,
	rudpinit,
	ipifcinit,
	icmpinit,
	icmp6init,
	igmpinit,
	greinit,
	ipmuxinit,
	ilinit,
	nil,
};
