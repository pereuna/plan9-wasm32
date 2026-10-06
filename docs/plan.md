# Plan9-wasm32: plan

Plan9-wasm32 is Plan 9 for the wasm32 architecture, the web browser as
the machine.  It is based on 9front (http://9front.org), a project of its
own; "9front" here means that project.  It is installed and booted as
9front is on a PC.

## Installing and booting

1. **Live image.** The browser loads https://plan2001.com/plan9-wasm32/
   (a directory of its own: the browser's trusted features - OPFS,
   WebAuthn, workers - need a secure origin, which file:// is not; offline
   is a small server on localhost).  It gets a minimal live image, as
   9front's ISO: the kernel (9wasm32.wasm), its boot file system
   (bootfs: a ramdisk) and the firmware (index.html, platform.js).  The
   live system is a guest OS, as 9front booted from its ISO: nothing is
   written anywhere.
2. **Installer.** The live image has 9front's installer, inst/start, as
   the ISO has: it lays out the machine's disk (OPFS: #S/sdW0) as 9front
   lays out a UEFI machine's - GPT with esp (FAT) and plan9 (nvram, fs) -
   copies the distribution onto fs, and puts the boot materials on esp:
   index.html, platform.js, 9wasm32.wasm, its bootfs and plan9.ini.  The
   distribution is the live image's, as 9front's is on its ISO: served
   from the same directory, read by the machine as a read-only disk.
3. **Boot from the disk.** Next time the page at the server is a small
   loader: if the disk's esp has the boot materials, it boots them - the
   firmware, the kernel and plan9.ini are the disk's, so the system
   changes only when it is installed again - else the live image.
4. **Again and again.** The disk can be written over as often as needed;
   the users are made up (proof of concept).

## The tree

- sys/src/9/wasm32: the kernel and its platform (platform.js, the page),
  devices: #S the disk (OPFS, partitions as sd's), #R the boot root, #I
  the network over WebSockets to the origin, #ω WebAuthn.
- sys/src/9/port additions (BootInfo), sys/include/bootinfo.h.
- sys/src/cmd/3a, 3c, 3l: the wasm32 assembler, compiler and loader.
- libc, libthread, libmp, libsec for wasm32; /wasm32/include.
- patches/9front: changes to 9front's own files (upstream).
- The build: 9front's release (pinned, its ISO's checksum), this tree
  over it, built with 9front's own mkfiles natively (plan9port's mk on
  Linux); the whole release for wasm32 is the distribution.
- 9front's off-topic files are left out (jokes, quotations); no "legacy
  removed" changes: the platform is new, not a PC.

## Steps

1. The import (done, 2026-10-05): the port, its devices (#ω among them),
   init, the disk's partitions and the firmware's boot from the disk.
2. The build (done, 2026-10-05): the release fetched (its checksum), this
   tree over it, 3c/3l for the host, built natively with 9front's mkfiles
   (tools/build: release, tree, hostcc, native, bootfs); tools/serve
   serves the page.
3. The tests (done, 2026-10-05): tools/test/run - the browser harness
   (wasmapp), a stand-in for the network (authsrv: dp9ik, secstore, an
   rcpu relay), the test programs by mk; the page takes plan9.ini from
   the server.
4. The live image and inst/start for wasm32 (9front's installer: the
   disk's layout, copydist from the distribution disk, bootsetup).  In
   progress (2026-10-06, docs/install.md): tools/dist makes the ISO and the
   live image; the live image boots from it as 9front's ISO does (9660
   root, 9front's init, termrc, rio).  inst/start on the console: configfs
   (hjfs), partdisk, prepdisk, mountfs, confignet, mountdist, copydist (the
   whole distribution onto the OPFS disk), ndbsetup and tzsetup done.
   Left: bootsetup, finish (fshalt -r, the page reloads), the boot from the
   disk, an install test in tools/test/run (a small test ISO).
5. The loader page: boot from the disk's FAT (its firmware, kernel and
   plan9.ini), else the live image.
6. plan2001.com/plan9-wasm32/: the live image and the distribution.
