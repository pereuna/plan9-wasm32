/*
 * Plan2001 Boot ABI v1: the BootInfo blob a loader hands the kernel,
 * everything it learned from the firmware.  This is the data half of the
 * contract and the same on every ISA; docs/boot-abi.md is its prose.  How
 * the blob's address is passed and what the CPU looks like at the jump is
 * each ISA's entry ABI: docs/boot-abi-amd64.md (RDI = the blob's physical
 * address); ARM64 (X0), wasm32 (_start's argument: docs/boot-abi-wasm32.md)
 * and RISC-V (a0) will follow.  Both sides include
 * this file.  There is no fixed physical address anywhere in the contract:
 * the kernel reads its input where the loader says.
 *
 * The blob is one contiguous, page-aligned range of physical memory:
 *
 *	BootInfo header		(headersize bytes)
 *	config			plan9.ini text, configlen bytes, NUL-terminated
 *	loader log		the loader's printed text, loglen bytes
 *	device tree		the firmware's flattened device tree, fdtlen bytes
 *	memory map		mmapcount BootMem entries, mmapentsize bytes apart
 *
 * Sections do not overlap each other or the header; their order is the
 * loader's choice, a kernel goes by the offsets.
 *
 * Offsets (…off) are from the start of the blob; addresses of things
 * outside it (acpi, fbbase) are physical.  The blob belongs to the kernel
 * from entry on and stays valid until the kernel itself releases it; it
 * must reserve [blob, blob+totalsize) before handing out any memory the
 * map calls free, as the blob itself sits in EfiLoaderData.
 *
 * Compatibility: a kernel takes a blob with its magic and version and a
 * headersize that reaches at least arch (every v1 loader it takes writes
 * that much).  A shorter header than the kernel's own sizeof(BootInfo) is
 * an older v1 loader's: the fields it lacks read as 0 (the kernel copies
 * the header, port/bootinfo.c), and 0 must mean "none" for every field
 * added at the end.  Fields past what the kernel knows are ignored.  So
 * new fields are added at the end of the header without a new version
 * (fdtoff and fdtlen were; rdbase and rdlen).  A change that breaks that
 * needs a new BootInfoVersion.
 */
enum {
	BootInfoMagic	= 0x49423250,	/* "P2BI" */
	BootInfoVersion	= 1,
};

/* BootInfo.arch: the ISA the loader made the blob for */
enum {
	BootArchAmd64	= 1,
	BootArchArm64	= 2,
	BootArchRiscv64	= 3,
	BootArchWasm32	= 4,	/* the browser: the page is the firmware (docs/boot-abi-wasm32.md) */
};

/* BootMem.type: UEFI EFI_MEMORY_TYPE */
enum {
	BootMemReserved,
	BootMemLoaderCode,
	BootMemLoaderData,
	BootMemBootCode,
	BootMemBootData,
	BootMemRuntimeCode,
	BootMemRuntimeData,
	BootMemConventional,
	BootMemUnusable,
	BootMemACPIReclaim,
	BootMemACPINVS,
	BootMemMMIO,
	BootMemMMIOPort,
	BootMemPalCode,
	BootMemPersistent,
};

typedef struct BootInfo BootInfo;
typedef struct BootMem BootMem;

/* one entry of the UEFI memory map taken just before ExitBootServices */
struct BootMem {
	u64int	base;
	u64int	len;		/* bytes */
	u32int	type;		/* BootMem* */
	u32int	attr;		/* EFI_MEMORY_* attributes, low 32 bits */
};

struct BootInfo {
	u32int	magic;		/* BootInfoMagic */
	u32int	version;	/* BootInfoVersion */
	u32int	headersize;	/* sizeof(BootInfo) of the loader that made it */
	u32int	totalsize;	/* the whole blob: header, sections, padding */
	u32int	flags;		/* zero */

	u32int	configoff;	/* plan9.ini text */
	u32int	configlen;	/* bytes, including the terminating NUL */
	u32int	logoff;		/* the loader's own printed text */
	u32int	loglen;		/* bytes, 0 if none */
	u32int	mmapoff;	/* the UEFI memory map */
	u32int	mmapcount;	/* entries */
	u32int	mmapentsize;	/* bytes from one entry to the next, >= sizeof(BootMem) */

	u64int	acpi;		/* physical address of the ACPI RSDP, 0 if none */

	u64int	tscfreq;	/* AMD64 only: TSC frequency in Hz, measured by the loader;
				 * 0 if unknown and on every other ISA, whose CPU timers
				 * are the kernel's own business */
	u64int	epoch;		/* UTC seconds since 1970, from UEFI GetTime; 0 if unknown */

	/*
	 * Entropy from EFI_RNG_PROTOCOL, rngseedlen bytes valid (0 if the
	 * protocol was not found).  The kernel zeroes this once it has used
	 * it, so it does not sit around in memory as a readable secret.
	 */
	uchar	rngseed[64];
	u32int	rngseedlen;

	/* linear framebuffer from the UEFI graphics output protocol; fbbase is 0 if none */
	u64int	fbbase;
	u64int	fbsize;		/* bytes */
	u32int	fbwidth;	/* visible pixels per line */
	u32int	fbheight;
	u32int	fbstride;	/* pixels per scan line, ie the pitch */
	u32int	fbdepth;	/* bits per pixel */
	char	fbchan[16];	/* Plan 9 channel descriptor, eg x8r8g8b8 */

	/* added at the end, still v1 */
	u32int	arch;		/* BootArch*: a kernel for another ISA must stop */
	u32int	fdtoff;		/* the firmware's flattened device tree, copied whole */
	u32int	fdtlen;		/* bytes, 0 if the firmware gave none (eg ACPI only) */

	/*
	 * added at the end, still v1: an image the loader loaded beside the
	 * blob for the kernel's root, whose format is the kernel's (wasm32:
	 * #R's archive, devrootfs.c); rdbase physical, 0 if none.  The memory
	 * map keeps it (LoaderData) and the kernel keeps it for good.
	 */
	u64int	rdbase;
	u64int	rdlen;		/* bytes */
};
