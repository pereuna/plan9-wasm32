# The kernel: 9front's port/ on a browser

`sys/src/9/wasm32` is 9front's kernel (`port/`) on a machine whose CPUs
are Web Workers, whose memory is one shared WebAssembly.Memory, and whose
firmware and devices are the page (`platform.js`, `kernel.html`).  The
page boots it as firmware boots a PC's kernel (`docs/boot.md`).

## Procs and memory

- No MMU, no paging: the kernel's memory is the platform's shared
  memory, its addresses physical, from 0 (`mem.h`).  The kernel has 64 MB
  (its image and heap); the blob, the boot file system and the
  framebuffer are above it.
- A Proc is a Worker of its own - its own instance of the kernel module,
  its own `m` and `up`, the kernel's memory shared (`proc.c`).  The
  browser schedules the Workers: sleep waits on the proc's state with
  `Atomics.wait`, ready stores and notifies.  There is no run queue,
  priority or preemption in the kernel.  To `port/`, the machine has one
  CPU, `MACHP(0)`, the boot Worker, which is the clock (`clock.c`).
- A user program is a module of its own on its proc's Worker, its memory
  its own (`docs/wasm32.md`: one module, one address space).  Its system
  call, `plan9.syscall(n, a)`, comes to `trap.c`: the arguments and the
  strings and buffers they point at are copied into the kernel, so
  9front's `sys*` and the devices see kernel addresses only, and what the
  call wrote goes back after it.
- fork: 3l's unwind and rewind of the stack (`docs/wasm32.md`); the
  kernel copies the memory for the child's Worker.  rfork(RFMEM) shares
  it: the child's Worker gets the same Memory.

## Devices

`devtab.c` is the configuration (no config file, no mkdevc).  9front's
devices from `port/` - root, cons, env, pipe, dup, srv, mnt, uart, proc,
draw, mouse, tls, ether (`#l`) - and its IP stack from `ip/` (`#I`, the
protocols and media pc's config has), and the platform's own:

| | | |
|---|---|---|
| `#R` | `devrootfs.c` | the boot file system, read-only, from BootInfo's `rdbase`/`rdlen` (`tools/bootfs`) |
| `#S` | `devsdw.c` | the disks: `sdW0`, a file in the origin's private file system (OPFS), a Worker's sync handle; `sdW1`, read-only, the distribution's ISO (the server's, by HTTP range requests, or a local file); GPT and prep partitions as sd's (`part`, `delpart`); `sdctl` |
| `#l0` | `etherwasm32.c` | the Ethernet card, `ether0`, under 9front's devether: its frames over a WebSocket to the machine's gateway |
| `#b` | `devkbd.c` | the keyboard: the browser's key events as runes for kbdfs |
| `#t` | `uartwasm32.c` | the serial port `eia0`: the page's console |
| `#ω` | `devwebauthn.c` | WebAuthn (`docs/webauthn.md`) |
| `#¶` | `devswap.c` | `swap`: the memory, as 9front's devswap says it (no paging, no swap) |
| screen | `screen.c` | XRGB32 in the kernel's memory, drawn by the page on its canvas (`platflush`) |

The network is 9front's own: ipconfig, DHCP, ARP, TCP, UDP, cs and dns
over an Ethernet card, as on a virtual machine.  The card's wire is a
WebSocket of the page's to its origin, `/ether` (a Worker waiting in
`Atomics.wait` would not hear it), a frame a binary message each way;
plan9.ini's `ether0=type=wasm32 ea=...` is the firmware's, the address
the same each time in one browser (localStorage).  What comes in the page
writes into the card's ring in kernel memory, and a kproc gives it to
devether; past a full ring a frame is dropped, as a card's would be.
What goes out goes to the page a frame at a time, and is dropped while
the WebSocket is not open or is full.  The link is up while it is open.
A frame shorter than its header or longer than 1514 bytes is dropped
both ways, never cut to size: the wire is not trusted.  The other end is
the machine's gateway (`tools/gateway`): a switch, the machines on it
and an uplink its ports - by default passt, QEMU's user network's
successor (no root: the machine's DHCP, DNS and connections out are
passt's, as the host's own programs'), or a network of its own with
DHCP and no way out (`-n`, the tests: QEMU's user network's addresses),
or a TAP device (`-t`, bridged or NATed by root).  It takes whole frames only, one MAC
address a machine, pages of its allowed origins (`-o`, loopback
without), and bounds its table; it has no authentication, so it is not
yet for a public site.  As a learning switch it gives a machine only its
own, broadcast and multicast frames: promiscuous mode sees no more.

## Boot

`main` takes the BootInfo, makes the memory, the procs and the devices,
and starts init: `init=` in the config (BootInfo's plan9.ini), by default
`/bin/rc /boot/init`.  The root is `#/`, as 9front's, with the boot file
system `#R` bound over it; a clean name space (newns) is `#/` alone.

`/boot/init` (`sys/src/9/wasm32/init`) does what 9front's bootrc does on a
terminal: the mount points, kbdfs on the console, factotum, the disk's
partitions; then the root - `bootargs=local!DEVICE` (the installed file
system: hjfs, gefs or cwfs on `/dev/sdW0/fs`), or with `cdboot` the
distribution's CD (`/dev/sdW1/data`, 9660srv: the live image).  Its file
server is posted as `/srv/boot`, the root goes before the boot file
system, and 9front's own init runs from it: `/lib/namespace`, termrc,
glenda's profile, rio (`docs/install.md`).  Without a root, the boot file
system alone: glenda's home on the disk (`/boot/disk`), the keys from
secstore (`/boot/secstore`), her rc and rio.

`#c/reboot` (fshalt -r) flushes the disks, and the page loads itself
again, as firmware starts over.
