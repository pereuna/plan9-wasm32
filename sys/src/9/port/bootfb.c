#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

/*
 * Boot progress markers on the UEFI framebuffer.
 *
 * No text, no font, no /dev/cons: a row of small squares in the top left
 * corner of the screen, one per boot stage, and a red bar when the kernel
 * panics.  Unreached stages show as dark squares, so the first dark square
 * is the stage the kernel never got past.  Each stage has its own colour
 * (not just "reached/not reached"), so the last colour seen identifies
 * exactly which stage was reached without needing to count squares.
 *
 * This row continues the loader's own marker row (sys/src/boot/efi/efi.c,
 * x64.s): the loader draws LoaderMarks squares (its own boot stages, before
 * and around ExitBootServices) at slots 0..LoaderMarks-1 in the same
 * top-left grid (same Margin/Size/Gap), and the kernel's squares below
 * continue at slot LoaderMarks - one shared strip spanning the whole boot,
 * loader and kernel alike, instead of two separate marker areas.
 *
 * The loader passes the framebuffer in the BootInfo (fbbase, fbstride, ...;
 * see sys/include/bootinfo.h).  fbstride is the row pitch in pixels, not the
 * visible width.  Only 32 bits per pixel (x8r8g8b8 or x8b8g8r8) are handled;
 * anything else leaves the markers off.  Marks made before the framebuffer is mapped
 * (bootfbinit needs the memory allocator) are remembered and drawn then.
 */

enum {
	LoaderMarks = 3,	/* slots the loader already drew into, see above */
	Nmark	= 5,		/* stages drawn as squares: BMMain..BMExec */
	Size	= 8,		/* square size in pixels - kept small deliberately:
				 * this row is a corner diagnostic, not meant to
				 * spend any real screen space during boot */
	Gap	= 2,
	Margin	= 2,
	Barh	= Margin+Size+Margin,	/* height of the panic bar; also the
					 * height reserved for the whole marker
					 * row - see bootmarkheight() */

	Dark	= 0x303030,
	Red	= 0xFF0000,
};

/* one colour per stage: BMMain, BMMem, BMDevs, BMUser, BMExec */
static u32int stagecolor[Nmark] = {
	0x8000FF,	/* BMMain: purple */
	0xFF0080,	/* BMMem: pink */
	0xFFFFFF,	/* BMDevs: white */
	0x0080FF,	/* BMUser: sky blue */
	0x00FF00,	/* BMExec: green - the "made it to /boot/boot" colour */
};

static u32int *fb;		/* mapped framebuffer, 32 bits per pixel */
static ulong stride;		/* pixels per scan line */
static ulong height;
static int bgr;			/* x8b8g8r8 instead of x8r8g8b8 */
static ulong reached;		/* one bit per stage, set even while fb == nil */

static void
rect(int x, int y, int w, int h, u32int rgb)
{
	u32int *p, c;
	int i, j;

	c = rgb;
	if(bgr)
		c = (rgb & 0x00FF00) | (rgb>>16 & 0xFF) | (rgb & 0xFF)<<16;
	if(y+h > height)
		h = height-y;
	for(j = 0; j < h; j++){
		p = fb + (y+j)*stride + x;
		for(i = 0; i < w; i++)
			p[i] = c;
	}
}

static void
draw(int n)
{
	if(n == BMPanic){
		rect(0, 0, stride, Barh, Red);
		return;
	}
	rect(Margin + (LoaderMarks+n)*(Size+Gap), Margin, Size, Size, stagecolor[n]);
}

/*
 * Height to reserve at the top of the screen for the marker row (loader's
 * and this file's squares together), so a console drawn below never
 * overlaps them. 0 if there is no framebuffer to reserve space on.
 */
int
bootmarkheight(void)
{
	return fb == nil? 0: Barh;
}

void
bootfbinit(void)
{
	BootInfo *b;
	ulong sz;
	void *v;
	int i;

	if(fb != nil || (b = bootinfo) == nil || b->fbbase == 0)
		return;
	if(b->fbdepth != 32 || b->fbstride == 0 || b->fbheight == 0)
		return;
	if(strcmp(b->fbchan, "x8r8g8b8") == 0)
		bgr = 0;
	else if(strcmp(b->fbchan, "x8b8g8r8") == 0)
		bgr = 1;
	else
		return;
	sz = b->fbsize;
	if(sz == 0)
		sz = b->fbstride * b->fbheight * 4;
	if((v = fbmap(b->fbbase, sz)) == nil)	/* the ISA's, eg pc64/bootarch.c */
		return;
	stride = b->fbstride;
	height = b->fbheight;
	fb = v;

	for(i = 0; i < Nmark; i++)
		rect(Margin + (LoaderMarks+i)*(Size+Gap), Margin, Size, Size, Dark);
	for(i = 0; i <= BMPanic; i++)
		if(reached & 1UL<<i)
			draw(i);
}

void
bootmark(int n)
{
	reached |= 1UL<<n;
	if(fb != nil)
		draw(n);
}

/*
 * The loader's own captured print() output (see sys/src/boot/efi/sub.c and
 * the BootInfo blob's log section, bootinfo.h), so a console can replay it before its own
 * history - otherwise those lines are simply gone once the console draws
 * over them. *np is set to the byte count; 0/nil if the loader had nothing
 * captured (eg its own capture-buffer allocation failed - non-fatal there
 * too).
 */
char*
bootlogtext(int *np)
{
	BootInfo *b;

	*np = 0;
	if((b = bootinfo) == nil || b->loglen == 0)
		return nil;
	*np = b->loglen;
	return (char*)bootblob + b->logoff;
}
