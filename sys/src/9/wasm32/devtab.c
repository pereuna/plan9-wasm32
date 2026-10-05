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
extern	Dev	srvdevtab, mntdevtab, uartdevtab, rootfsdevtab, procdevtab, drawdevtab, mousedevtab, kbddevtab, wsnetdevtab, tlsdevtab, sdwdevtab, webauthndevtab;

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
	&wsnetdevtab,
	&tlsdevtab,
	&sdwdevtab,
	&webauthndevtab,
	nil,
};

extern	PhysUart	wasm32physuart;
PhysUart*	physuart[] = { &wasm32physuart, nil };

char*	conffile = "wasm32";
ulong	kerndate = 1790000000;

void
links(void)
{
}

void
bootlinks(void)
{
}
