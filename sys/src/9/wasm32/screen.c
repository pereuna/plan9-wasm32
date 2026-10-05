#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

#define	Image	IMAGE
#include	<draw.h>
#include	<memdraw.h>
#include	<cursor.h>
#include	"screen.h"

/*
 * wasm32's screen (docs/architecture.md, phase C): XRGB32 in the
 * kernel's memory; flushmemscreen tells the page which part changed and
 * the page draws it from the shared memory on its canvas (platform.js).
 * The cursor is the page's own (platcursor: the bitmaps), the mouse the
 * page's pointer: its events come through a ring (mouseproc).
 */
Memimage	*gscreen;

typedef struct Mring Mring;
struct Mring
{
	ulong	w;		/* the page's: events written (modulo 2^32) */
	ulong	r;		/* ours: read */
	long	ev[64][4];	/* x, y, buttons, msec */
};
static Mring	mring;

static void
mouseproc(void*)
{
	long *e;

	for(;;){
		while(mring.r == mring.w)
			platwait((long*)&mring.w, mring.r, -1);
		e = mring.ev[mring.r & (nelem(mring.ev)-1)];
		absmousetrack(e[0], e[1], e[2], e[3]);
		mring.r++;
	}
}

/*
 * the page's framebuffer, from BootInfo: the page, as the firmware,
 * put it in the kernel's memory (the memory map calls it Reserved) and
 * draws it on its canvas; XRGB32, a line its width.  None: no screen.
 */
void
screeninit(void)
{
	static Memdata md;
	BootInfo *bi;
	Rectangle r;

	bi = bootinfo;
	if(bi->fbbase == 0)
		return;
	r = Rect(0, 0, bi->fbwidth, bi->fbheight);
	if(bi->fbdepth != 32 || bi->fbstride != bi->fbwidth
	|| strcmp(bi->fbchan, "x8r8g8b8") != 0
	|| bi->fbsize < (uvlong)Dx(r)*Dy(r)*4){
		print("screen: framebuffer %dx%d %d %s: not XRGB32, no screen\n",
			bi->fbwidth, bi->fbheight, bi->fbdepth, bi->fbchan);
		return;
	}
	/* bootinfoinit checked it against the map, this against the memory */
	if(bootearlymap(bi->fbbase, (uvlong)Dx(r)*Dy(r)*4) == nil){
		print("screen: framebuffer at %#llux: not in the memory, no screen\n", bi->fbbase);
		return;
	}
	memimageinit();
	md.base = nil;
	md.bdata = (uchar*)(uintptr)bi->fbbase;
	md.ref = 1;
	md.allocd = 0;
	gscreen = allocmemimaged(r, XRGB32, &md);
	if(gscreen == nil)
		panic("screeninit: no %dx%d screen", Dx(r), Dy(r));
	memfillcolor(gscreen, 0x777777FF);
	flushmemscreen(gscreen->r);
}

void
mouseinput(void)
{
	platmousering(&mring);
	kproc("mousein", mouseproc, nil);
}

Memdata*
attachscreen(Rectangle *r, ulong *chan, int *d, int *width, int *softscreen)
{
	if(gscreen == nil)
		return nil;
	*r = gscreen->clipr;
	*chan = gscreen->chan;
	*d = gscreen->depth;
	*width = gscreen->width;
	*softscreen = 1;
	gscreen->data->ref++;
	return gscreen->data;
}

void
flushmemscreen(Rectangle r)
{
	if(gscreen == nil || !rectclip(&r, gscreen->r))
		return;
	platflush(r.min.x, r.min.y, r.max.x, r.max.y);
}

/* no colour map: XRGB32 */
void
getcolor(ulong, ulong *r, ulong *g, ulong *b)
{
	*r = *g = *b = 0;
}

int
setcolor(ulong, ulong, ulong, ulong)
{
	return 0;
}

void
blankscreen(int)
{
}

/* the page's cursor: shown and moved there */
void
cursoron(void)
{
}

void
cursoroff(void)
{
}

void
setcursor(Cursor *c)
{
	platcursor(c->offset.x, c->offset.y, c->clr, c->set);
}

/*
 * 9front's mouse settings (accelerated, linear, res, swap, ...): the
 * page's pointer is the browser's, as the system has it - nothing to set
 */
void
mousectl(Cmdbuf *cb)
{
	static char *ok[] = { "accelerated", "linear", "res", "swap", "reset", "hwaccel", "intellimouse", "ps2", "ps2intellimouse", "serial", "wheel" };
	int i;

	for(i = 0; i < nelem(ok); i++)
		if(strcmp(cb->f[0], ok[i]) == 0)
			return;
	error(Ebadctl);
}
