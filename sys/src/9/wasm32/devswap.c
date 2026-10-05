#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"
#include	<pool.h>

/*
 * #¶: the machine's memory, as port/devswap.c says it in swap - wasm32
 * has no paging, so no swap and no swapfile.  The kernel's memory is
 * conf.npage pages; each program is a module with a memory of its own,
 * outside the kernel's, which the browser grows as the program asks (brk):
 * user is what a program may have, Usermem (the installer sizes a file
 * server's cache from it, inst/mounthjfs).
 */
enum
{
	Qdir,
	Qswap,

	Usermem	= 512*MiB,
};

static Dirtab swapdir[] = {
	".",		{Qdir, 0, QTDIR},	0,	DMDIR|0555,
	"swap",		{Qswap},		0,	0444,
};

static Chan*
swapattach(char *spec)
{
	return devattach(L'¶', spec);
}

static Walkqid*
swapwalk(Chan *c, Chan *nc, char **name, int nname)
{
	return devwalk(c, nc, name, nname, swapdir, nelem(swapdir), devgen);
}

static int
swapstat(Chan *c, uchar *dp, int n)
{
	return devstat(c, dp, n, swapdir, nelem(swapdir), devgen);
}

static Chan*
swapopen(Chan *c, int omode)
{
	return devopen(c, omode, swapdir, nelem(swapdir), devgen);
}

static void
swapclose(Chan*)
{
}

static long
swapread(Chan *c, void *va, long n, vlong off)
{
	char tmp[512];

	switch((ulong)c->qid.path){
	case Qdir:
		return devdirread(c, va, n, swapdir, nelem(swapdir), devgen);
	case Qswap:
		snprint(tmp, sizeof tmp,
			"%llud memory\n"
			"%llud pagesize\n"
			"%lud kernel\n"
			"%lud/%lud user\n"
			"0/0 swap\n"
			"0/%lud reclaim\n"
			"%llud/%llud/%llud kernel malloc\n"
			"%llud/%llud/%llud kernel draw\n"
			"%llud/%llud/%llud kernel secret\n",
			(uvlong)conf.npage*BY2PG,
			(uvlong)BY2PG,
			conf.npage,
			0UL, Usermem/BY2PG,
			Usermem/BY2PG,
			(uvlong)mainmem->curalloc, (uvlong)mainmem->cursize, (uvlong)mainmem->maxsize,
			(uvlong)imagmem->curalloc, (uvlong)imagmem->cursize, (uvlong)imagmem->maxsize,
			(uvlong)secrmem->curalloc, (uvlong)secrmem->cursize, (uvlong)secrmem->maxsize);
		return readstr((ulong)off, va, n, tmp);
	}
	error(Egreg);
	return 0;
}

static long
swapwrite(Chan*, void*, long, vlong)
{
	error(Eperm);
	return 0;
}

Dev swapdevtab = {
	L'¶',
	"swap",

	devreset,
	devinit,
	devshutdown,
	swapattach,
	swapwalk,
	swapstat,
	swapopen,
	devcreate,
	swapclose,
	swapread,
	devbread,
	swapwrite,
	devbwrite,
	devremove,
	devwstat,
};
