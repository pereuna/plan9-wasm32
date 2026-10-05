#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

/*
 * The loader passes what it learned in a BootInfo blob (Plan2001 Boot ABI
 * v1, sys/include/bootinfo.h, docs/boot-abi.md) and tells us where: each
 * ISA's entry code saves the address it was handed (AMD64: RDI, see
 * pc64/l.s) in bootinfopa.  There is no fixed address to look at.  This
 * file is the same for every ISA; what differs - mapping the blob before
 * the kernel's memory allocator exists, and the framebuffer's caching - is
 * bootearlymap() and fbmap() of the ISA port (pc64/bootarch.c).  The blob
 * is the only source of the memory map on this UEFI-only kernel, so
 * anything short of a complete, self-consistent blob is fatal:
 * bootinfoinit() below rejects an empty memory map or a section that does
 * not fit, not just a garbled header, precisely so meminit0() (pc/memory.c)
 * never falls through to its old BIOS-era ramscan() fallback - keeping that
 * path unreachable is the point, not an accident.  Nothing can be printed
 * this early; halt with interrupts off.
 */

/*
 * bootinfo is the kernel's copy of the blob's header, as long as this
 * kernel's sizeof(BootInfo): fields past the loader's headersize - added
 * to v1 after that loader was made - read as 0, which is what each such
 * field means when absent (bootinfo.h).  bootblob is the blob itself, where
 * the sections are.
 */
BootInfo *bootinfo;
uchar *bootblob;
static BootInfo bootinfohdr;

/* the header every v1 loader that this kernel takes writes: up to arch, which it must check */
#define	BootInfoMinHeader	((uintptr)&((BootInfo*)0)->fdtoff)

static void
bootinfohalt(void)
{
	for(;;)
		halt();
}

/*
 * Entropy from the loader's BootInfo, mixed in on top of whatever hwrandbuf
 * cpuidentify() (pc/devarch.c) already set (RDRAND, or nil), rather than
 * replacing it: more sources of entropy are never worse, and this way an
 * absent EFI_RNG_PROTOCOL or a CPU without RDRAND still leaves the other one
 * working.  Used once by randominit() (port/random.c) to seed the entropy
 * pool alongside the kernel's own timing-based collection.  Self-clearing:
 * the seed is copied at most once, then wiped from BootInfo (it would
 * otherwise sit in the blob, readable, for the life of the machine) and
 * hwrandbuf is put back to what it chains to, so a second call is cheap and
 * does not re-touch already-zeroed memory.
 */
static void (*bootinforandnext)(void*, ulong);

static void
bootinforand(void *p, ulong n)
{
	uchar *q;
	ulong i, m;

	if(bootinforandnext != nil)
		(*bootinforandnext)(p, n);

	q = p;
	m = n;
	if(m > bootinfo->rngseedlen)
		m = bootinfo->rngseedlen;
	for(i = 0; i < m; i++)
		q[i] ^= bootinfo->rngseed[i];

	memset(bootinfo->rngseed, 0, sizeof(bootinfo->rngseed));
	bootinfo->rngseedlen = 0;
	memset(((BootInfo*)bootblob)->rngseed, 0, sizeof(bootinfo->rngseed));	/* and the loader's */
	((BootInfo*)bootblob)->rngseedlen = 0;
	hwrandbuf = bootinforandnext;
}

/* does the section [off, off+len) lie within the blob, after the header? */
static int
bootinfoin(BootInfo *b, uvlong off, uvlong len)
{
	return off >= b->headersize && off <= b->totalsize && len <= b->totalsize - off;
}

/* do [a, a+al) and [b, b+bl), both within the blob, share a byte? */
static int
bootinfooverlap(uvlong a, uvlong al, uvlong b, uvlong bl)
{
	return al != 0 && bl != 0 && a < b + bl && b < a + al;
}

/* the memory map's entries, before bootinfo is set */
static BootMem*
bootmemof(BootInfo *b, uchar *blob, int i)
{
	return (BootMem*)(blob + b->mmapoff + (uvlong)i*b->mmapentsize);
}

/* does [base, base+len) lie within one entry of the map of this type? */
static int
bootinmap(BootInfo *b, uchar *blob, uvlong base, uvlong len, u32int type)
{
	BootMem *m;
	int i;

	for(i = 0; i < b->mmapcount; i++){
		m = bootmemof(b, blob, i);
		if(m->type == type && base >= m->base && base - m->base <= m->len
		&& len <= m->len - (base - m->base))
			return 1;
	}
	return 0;
}

/* does [base, base+len) share a byte with memory the kernel may hand out (BootClassRAM)? */
static int
bootoverram(BootInfo *b, uchar *blob, uvlong base, uvlong len)
{
	BootMem *m;
	int i;

	for(i = 0; i < b->mmapcount; i++){
		m = bootmemof(b, blob, i);
		if(bootmemclass(m->type) == BootClassRAM && m->len != 0
		&& base < m->base + m->len && m->base < base + len)
			return 1;
	}
	return 0;
}

static ulong
bootbe32(uchar *p)
{
	return (ulong)p[0]<<24 | p[1]<<16 | p[2]<<8 | p[3];
}

void
bootinfoinit(void)
{
	BootInfo *b;
	uchar *blob;
	uvlong off[4], len[4], need;
	uchar *fdt;
	int i, j;

	if(bootinfopa == 0 || (bootinfopa & (BY2PG-1)) != 0)
		bootinfohalt();
	if((blob = bootearlymap(bootinfopa, BY2PG)) == nil)
		bootinfohalt();
	/*
	 * v1's compatibility (bootinfo.h): the loader's header may be shorter
	 * than this kernel's, down to what every v1 loader writes; what it
	 * lacks is 0 in the copy.  Longer: fields this kernel does not know.
	 */
	b = &bootinfohdr;
	memset(b, 0, sizeof *b);
	memmove(b, blob, BootInfoMinHeader);
	if(b->magic != BootInfoMagic || b->version != BootInfoVersion
	|| b->headersize < BootInfoMinHeader || b->headersize > BY2PG
	|| b->totalsize < b->headersize)
		bootinfohalt();
	memmove(b, blob, b->headersize < sizeof *b ? b->headersize : sizeof *b);
	if(b->arch != BootInfoArch)		/* a blob made for another ISA */
		bootinfohalt();
	if(bootearlymap(bootinfopa, b->totalsize) != blob)
		bootinfohalt();

	/* every section after the header, within the blob, none overlapping */
	off[0] = b->configoff, len[0] = b->configlen;
	off[1] = b->logoff, len[1] = b->loglen;
	off[2] = b->fdtoff, len[2] = b->fdtlen;
	off[3] = b->mmapoff, len[3] = (uvlong)b->mmapcount * b->mmapentsize;
	for(i = 0; i < nelem(off); i++){
		if(!bootinfoin(b, off[i], len[i]))
			bootinfohalt();
		for(j = 0; j < i; j++)
			if(bootinfooverlap(off[i], len[i], off[j], len[j]))
				bootinfohalt();
	}
	if(b->configlen == 0 || ((char*)blob)[b->configoff + b->configlen - 1] != '\0'
	|| b->mmapcount == 0 || b->mmapentsize < sizeof(BootMem))
		bootinfohalt();

	/* a device tree, if there is one, is a whole one */
	if(b->fdtlen != 0){
		fdt = blob + b->fdtoff;
		if(b->fdtlen < 40 || bootbe32(fdt) != 0xd00dfeed || bootbe32(fdt+4) > b->fdtlen)
			bootinfohalt();
	}

	/*
	 * what lies outside the blob, by the map: the root's image is memory
	 * the loader took for it (LoaderData), not the blob; the framebuffer
	 * is nothing the kernel would hand out.  Whether the ISA can reach
	 * them is its own check (wasm32: bootearlymap, screen.c, devrootfs.c).
	 */
	if(b->rdbase != 0){
		if(b->rdlen == 0 || b->rdbase + b->rdlen < b->rdbase
		|| !bootinmap(b, blob, b->rdbase, b->rdlen, BootMemLoaderData)
		|| (b->rdbase < bootinfopa + b->totalsize && bootinfopa < b->rdbase + b->rdlen))
			bootinfohalt();
	}
	if(b->fbbase != 0){
		need = (uvlong)b->fbstride * b->fbheight * (b->fbdepth / 8);
		if(b->fbstride < b->fbwidth || b->fbheight == 0 || b->fbdepth == 0
		|| (b->fbsize != 0 && b->fbsize < need)
		|| b->fbbase + (b->fbsize ? b->fbsize : need) < b->fbbase
		|| bootoverram(b, blob, b->fbbase, b->fbsize ? b->fbsize : need))
			bootinfohalt();
	}
	bootblob = blob;
	bootinfo = b;
}

/* entry i of the memory map, i < bootinfo->mmapcount */
BootMem*
bootmem(int i)
{
	return bootmemof(bootinfo, bootblob, i);
}

/*
 * What a memory map entry is to the kernel, from its UEFI type (BootMem*,
 * sys/include/bootinfo.h): the ISA port turns these into its own memory
 * kinds (pc/memory.c: MemRAM, MemACPI, MemReserved); -1 for a type this
 * kernel does not know, which it should leave alone.
 */
int
bootmemclass(u32int type)
{
	switch(type){
	case BootMemLoaderCode:
	case BootMemLoaderData:
	case BootMemBootCode:
	case BootMemBootData:
	case BootMemConventional:
		return BootClassRAM;
	case BootMemACPIReclaim:
		return BootClassACPI;
	case BootMemReserved:
	case BootMemRuntimeCode:
	case BootMemRuntimeData:
	case BootMemUnusable:
	case BootMemACPINVS:
	case BootMemMMIO:
	case BootMemMMIOPort:
	case BootMemPalCode:
		return BootClassReserved;
	}
	return -1;
}

/*
 * The firmware's flattened device tree, copied whole into the blob by the
 * loader (checked by bootinfoinit()), and its size; nil if there is none,
 * as on PCs, which describe themselves by ACPI.
 */
uchar*
bootfdt(ulong *len)
{
	if(bootinfo == nil || bootinfo->fdtlen == 0){
		*len = 0;
		return nil;
	}
	*len = bootinfo->fdtlen;
	return bootblob + bootinfo->fdtoff;
}

/* the plan9.ini text, NUL-terminated (checked by bootinfoinit()) */
char*
bootconfig(void)
{
	return (char*)bootblob + bootinfo->configoff;
}

/*
 * Wire the BootInfo entropy in.  cpuidentify() (pc/devarch.c) sets hwrandbuf
 * unconditionally, and every CPU calls it once: the boot processor early in
 * main(), and each AP from squidboy() during its own bring-up.  This must
 * therefore run after all of them have, or a later one would clobber our
 * wiring; arch->intrinit() (pc/mp.c) starts the APs one at a time and each
 * mpstartap() call waits for the AP's apic->online flag, which squidboy()
 * only sets after its own cpuidentify(), so intrinit() returning is the
 * first point where no cpuidentify() call is still pending.
 */
void
bootinforandinit(void)
{
	if(bootinfo == nil || bootinfo->rngseedlen == 0)
		return;
	bootinforandnext = hwrandbuf;
	hwrandbuf = bootinforand;
}

/*
 * Seed the wall clock from the loader's reading of the UEFI real-time clock,
 * the same way a write to #c/time does (port/devcons.c), so the clock is
 * right from the first line of boot output instead of only once /boot/boot
 * or some other user program next sets it.  todset() calls todinit() on
 * first use, which starts a clock0 link (port/tod.c todfix) and arms it
 * right away through arch->timerset(); call this after arch->clockenable(),
 * not from bootinfoinit() (too early: no malloc yet, and lapictimerset()
 * divides by an LAPIC timer divisor that clockenable() has not set up yet).
 *
 * tscfreq, measured by the loader against its own timer before any kernel
 * code ran, is not used to skip the kernel's own TSC calibration (i8253.c,
 * hpet.c): that calibration also derives delaylcycles, which delayloop()
 * needs and which tscfreq alone does not give us.  It is only printed here,
 * as a cross-check against the value the kernel measures a little later.
 */
void
bootinfoclock(void)
{
	if(bootinfo == nil)
		return;
	if(bootinfo->epoch != 0)
		todset(bootinfo->epoch * 1000000000LL, 0, 0);
	if(bootinfo->epoch != 0 || bootinfo->tscfreq != 0)
		print("bootinfo: epoch %llud tscfreq %llud\n", bootinfo->epoch, bootinfo->tscfreq);
	if(bootinfo->fdtlen != 0)
		print("bootinfo: device tree %ud bytes\n", bootinfo->fdtlen);
}
