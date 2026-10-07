#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

/*
 * #b/kbd: the page's keyboard, for kbdfs - its messages as kbdfs reads
 * them: r and a rune a key down, R up, c a rune typed (pasted), each
 * ended by a 0.  The runes are the browser's (e.key): the system's
 * layout is applied already, no scancodes, no kbmap.  The page writes
 * them into a ring (platkbdring); a kproc gives each message to the
 * queue whole, as a read wants it.  Nothing is dropped: a full queue
 * stops the kproc (qwrite), a full ring keeps them on the page.
 */
enum
{
	Qdir,
	Qkbd,

	Nring	= 4096,
};

typedef struct Kring Kring;
struct Kring
{
	ulong	r;		/* modulo 2^32; Nring a power of 2, the index masked */
	ulong	w;
	uchar	b[Nring];
};

static	Kring	kring;
static	Queue	*kbdq;

static Dirtab kbddir[] = {
	".",	{Qdir, 0, QTDIR},	0,	DMDIR|0555,
	"kbd",	{Qkbd},		0,	0444,
};

static void
kbdproc(void*)
{
	char msg[16];
	int n;

	n = 0;
	for(;;){
		while(kring.r == kring.w)
			platwait((long*)&kring.w, kring.r, -1);
		msg[n] = kring.b[kring.r & (Nring-1)];
		kring.r++;
		if(msg[n] == 0){
			qwrite(kbdq, msg, n+1);
			n = 0;
		}else if(++n == sizeof msg)
			n = 0;	/* not a message: dropped */
	}
}

static void
kbdreset(void)
{
	kbdq = qopen(4*1024, Qmsg, 0, 0);
	if(kbdq == nil)
		panic("kbdreset");
	platkbdring(&kring);
	kproc("kbdin", kbdproc, nil);
}

static Chan*
kbdattach(char *spec)
{
	return devattach('b', spec);
}

static Walkqid*
kbdwalk(Chan *c, Chan *nc, char **name, int nname)
{
	return devwalk(c, nc, name, nname, kbddir, nelem(kbddir), devgen);
}

static int
kbdstat(Chan *c, uchar *dp, int n)
{
	return devstat(c, dp, n, kbddir, nelem(kbddir), devgen);
}

static Chan*
kbdopen(Chan *c, int omode)
{
	return devopen(c, omode, kbddir, nelem(kbddir), devgen);
}

static void
kbdclose(Chan*)
{
}

static long
kbdread(Chan *c, void *a, long n, vlong)
{
	if(c->qid.type & QTDIR)
		return devdirread(c, a, n, kbddir, nelem(kbddir), devgen);
	return qread(kbdq, a, n);
}

static long
kbdwrite(Chan*, void*, long, vlong)
{
	error(Eperm);
	return 0;
}

Dev kbddevtab = {
	'b',
	"kbd",

	kbdreset,
	devinit,
	devshutdown,
	kbdattach,
	kbdwalk,
	kbdstat,
	kbdopen,
	devcreate,
	kbdclose,
	kbdread,
	devbread,
	kbdwrite,
	devbwrite,
	devremove,
	devwstat,
};
