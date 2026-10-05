# Booting: BootInfo v1 and the wasm32 entry

How the firmware hands the machine to the kernel.  The contract has two
parts: the data, a BootInfo blob, the same for every ISA
(`sys/include/bootinfo.h`, which loader and kernel both use), and the
entry, the ISA's own.  On wasm32 the page is the firmware and the loader:
`platform.js`'s `boot()` makes the same BootInfo a UEFI loader makes for
amd64 or arm64 (BootInfo v1 comes from Plan2001's fork, where those
loaders are).

The rule: **the loader says where things are; the kernel does not
guess.**  Neither part has a fixed physical address.

## The blob

One contiguous, page-aligned region; its address is given at the entry.

```
entry ──> +----------------------+  offset 0
          | BootInfo header      |  headersize
          +----------------------+  configoff
          | config (plan9.ini)   |  configlen, NUL-terminated
          +----------------------+  logoff
          | loader log           |  loglen
          +----------------------+  fdtoff
          | device tree (FDT)    |  fdtlen, 0 if none
          +----------------------+  mmapoff
          | memory map           |  mmapcount × mmapentsize (BootMem)
          +----------------------+  totalsize
```

- Offsets (`…off`) are from the blob's start: it moves as a whole.  What
  points outside it (`acpi`, `fbbase`, `rdbase`) is a physical address.
- Header: `magic` `"P2BI"`, `version` 1, `headersize`, `totalsize`,
  `flags`, the sections, and fixed fields: the ACPI RSDP, the
  framebuffer, the UTC time (`epoch`), a random seed.
- `arch`: the ISA the blob is for (`BootArchAmd64` 1, `BootArchArm64` 2,
  `BootArchRiscv64` 3, `BootArchWasm32` 4).  A kernel stops on one not
  its own, as on a wrong magic.
- The memory map: UEFI's types (`BootMem*`), adjacent regions of one
  type merged.
- `rdbase`/`rdlen`: the root's image the loader put beside the blob, in
  the kernel's form (wasm32: `#R`'s archive), in LoaderData memory that
  the kernel keeps.  0: none.
- Sections overlap neither each other nor the header.

The blob is the kernel's from its entry; the kernel reserves it before
it hands out free memory, and keeps it (`confval[]` points into the
config).

## Compatibility

A kernel takes a blob whose `magic` and `version` are its own and whose
`headersize` reaches at least `arch`.  A shorter header than the
kernel's `sizeof(BootInfo)` is an older v1 loader's: the kernel copies
the header, the missing fields 0 (`port/bootinfo.c`), so a field added
at the end means "none" when 0, and **new fields go at the end without a
new version** (`fdtoff`, `fdtlen`, `rdbase`, `rdlen` came so).  Regions
outside the blob are checked against the memory map before the kernel
touches them: the root's image wholly in one LoaderData region and not
on the blob, the framebuffer not in memory the kernel hands out.  A
change that breaks this (a field removed from the middle, a meaning
changed) needs a new `BootInfoVersion`.

## The wasm32 entry

The kernel (`9wasm32.wasm`, 3c and `3l -k`) is a WebAssembly module that
imports its memory (`platform.memory`, shared, one for all the Workers).
The page starts it on the first Worker:

| | |
|---|---|
| `_init()` | first, once: the kernel's data into memory (a passive segment) |
| `sp` | `(stacktop − 16) & ~7` |
| memory at `sp` | **the blob's address** (32 bits, little-endian) |
| `_start()` | calls `main(pa)` (`3l -E main`) |

The address is a C call's first argument in wasm32's convention
(arguments in memory from SP), as `platnewproc`'s procs get theirs.
There is no other state: no interrupts, no page tables.  A linear
memory address is wasm32's physical address (`bootearlymap()` gives it
back as it is, `sys/src/9/wasm32/bootarch.c`).

## Memory

The page makes the memory and writes into it before the entry:

```
0          the kernel's module: data, bss, stack (stacktop) ...   Conventional
           [end, 64 MB) the kernel's heap                          (the same)
64 MB      the disk's registers (4 KiB), if there is a disk        Reserved
           the BootInfo blob (header, config, memory map)          LoaderData
           the root's archive (#R, devrootfs.c), rdbase/rdlen      LoaderData
           the framebuffer, XRGB32, fbstride = fbwidth             Reserved
```

Each region is page-aligned (4 KiB).  The Conventional region starts at
0: the page does not load the kernel's image - WebAssembly instantiates
the module - so it does not know where the data ends; the kernel keeps
`[0, end)` for itself (`confinit`, `main.c`).

## What the blob has on wasm32

| Field | wasm32 |
|---|---|
| config | plan9.ini's text: `init=` (init's argv, rc's quoting, as `tokenize` takes it) and the page's `config` lines.  The kernel reads it with `getconf` (`port/bootargs.c`); it goes into the environment as on 9front.  Without `init=` the kernel stops after its own test. |
| loader log, FDT, ACPI, `tscfreq` | none (0) |
| `epoch` | `Date.now()` in seconds |
| `rngseed` | 64 bytes from `crypto.getRandomValues`, mixed in by `bootinforandinit` and cleared |
| framebuffer | `fbbase`, `fbwidth` × `fbheight`, `fbstride` = `fbwidth`, 32 bits, `x8r8g8b8`.  The kernel draws in it (`screen.c`) and says what changed with `platflush`; the page draws it on its canvas.  `fbbase` 0: no screen. |
| config: `*sdW0=regs bytes` | the machine's disk (`devsdw.c`): its register page and size.  The page is Reserved in the map, right after 64 MB, before the blob.  The firmware writes the line last, so the page's own lines do not replace it.  No disk: no line, no page. |
| `rdbase`, `rdlen` | the boot file system (`tools/bootfs`), which `devrootfs.c` reads in place. |

The platform's calls (`platform.h`) are the machine's devices: the
console, Workers, Atomics, the network, keyboard, mouse, `platflush`,
`platcursor`, WebAuthn (`platwebauthn`), and `platnsec`/`platrandom`, the
clock and random source at run time.

## The kernel's side

- `main(uintptr pa)` sets `bootinfopa = pa` and calls `bootinfoinit()`,
  then `bootargsinit()` (`sys/src/9/port/bootinfo.c`, `bootargs.c`).
- A blob the kernel does not take stops the machine: a wrong magic,
  version or `arch`, a section outside the blob, an empty memory map, the
  archive outside LoaderData, the framebuffer in memory it hands out.
  `halt()` → `plathalt("bootinfo: no blob this kernel can take")`.
- `bootearlymap(pa, size)` takes only what is in memory (`platmemsize()`,
  the WebAssembly.Memory's size now): a map that promises more than there
  is does not lead to an out-of-bounds trap.
- `confinit` makes `conf.mem[]` from the Conventional regions
  (`[end, …)`); the heap's size comes from them.
- After `timersinit`, `bootinforandinit()` and `bootinfoclock()`.
