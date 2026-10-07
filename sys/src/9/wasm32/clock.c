#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"ureg.h"

/*
 * wasm32's clock: no interrupts; the boot Worker, CPU 0 (mach0), is
 * the clock's interrupt.  It waits until the next timer is due, as
 * timerset says, and runs the timers (portclock.c's timerintr) - the HZ
 * timer's hzclock among them, which counts MACHP(0)->ticks.  A clock
 * late (a background tab's Workers are slowed) runs what is due; ticks
 * missed are lost, as on hardware.
 *
 * timerset is called on any Worker: nextwhen and clockword change
 * under clocklock, then the clock is woken.
 */
static	Lock	clocklock;
static	long	clockword;		/* the clock waits on it */
static	uvlong	nextwhen;		/* 0: no timer set */

void
timerset(Tval when)
{
	ilock(&clocklock);
	nextwhen = when;
	clockword++;
	iunlock(&clocklock);
	platwake(&clockword, 1);
}

/* main's last: CPU 0 is the clock from here on */
_Noreturn void
clockinit(void)
{
	Ureg u;
	uvlong now, when, ms;
	long seen;

	memset(&u, 0, sizeof u);
	for(;;){
		ilock(&clocklock);
		seen = clockword;
		when = nextwhen;
		iunlock(&clocklock);
		now = platnsec();
		if(when == 0){
			platwait(&clockword, seen, -1);
			continue;
		}
		if(when > now){
			/* rounded up: woken early, the timer would not be due */
			ms = (when - now + 999999) / 1000000;
			if(ms > 1000)
				ms = 1000;
			platwait(&clockword, seen, ms);
			continue;
		}
		/* due: timerintr sets the next, if there is one */
		ilock(&clocklock);
		if(nextwhen == when)
			nextwhen = 0;
		iunlock(&clocklock);
		timerintr(&u, now);
	}
}
