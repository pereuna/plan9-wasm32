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
  devices: #S the disk (OPFS, partitions as sd's), #R the boot root, #l
  an Ethernet card whose frames go over a WebSocket to the origin's
  gateway (9front's devether and IP stack over it), #ω WebAuthn.
- sys/src/9/port additions (BootInfo), sys/include/bootinfo.h.
- sys/src/cmd/3a, 3c, 3l: the wasm32 assembler, compiler and loader.
- libc, libthread, libmp, libsec for wasm32; /wasm32/include.
- patches/9front: changes to 9front's own files (upstream).
- The build: 9front's release (pinned, its ISO's checksum), this tree
  over it, built with 9front's own mkfiles natively (plan9port's mk on
  Linux).
- The distribution (docs/install.md): the tree branch - a snapshot of
  9front's release and this tree over it, the machine's whole source - and an ISO
  with it and its git, built for wasm32 only the base system and the
  tools; the machine builds the rest itself, and updates with git.
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
4. The live image and inst/start (done, 2026-10-06; docs/install.md):
   tools/dist makes the ISO and the live image; it boots as 9front's ISO
   does (9660 root, 9front's init, termrc, rio); inst/start installs onto
   the OPFS disk (hjfs, GPT, prep's 9fat, copydist, bootsetup) and fshalt
   -r reboots; tools/test/run's install does it all with a small ISO.
5. The loader page (done, 2026-10-06): the page boots the disk's FAT -
   its own firmware, kernel and plan9.ini - else the live image.
6. https://plan2001.com/plan9-wasm32/ (done, 2026-10-06): the live image
   and the distribution's ISO (720 MB, read with range requests); it
   boots to rio over the Internet.  ?iso=file boots a copy of the ISO on
   one's own computer instead.
7. The network as 9front's (done, 2026-10-07): #l0, an Ethernet card
   (etherwasm32.c) in place of the WebSocket /net/tcp the import had; 9front's
   devether and IP stack (#I) over it, and ipconfig, cs and the installer's
   confignet as on a PC.  Its wire is a WebSocket to the origin's /ether;
   tools/gateway is the other end - a switch and a TAP, a network of its
   own with DHCP (-n) for offline and the tests, or the host's (-t).
   https://plan2001.com/plan9-wasm32/'s gateway is step 9's.
8. The distribution as 9front's (done, 2026-10-08): the tree
   branch (tools/treebranch), a snapshot of 9front's release and this tree; the ISO
   (tools/dist) its files and .git, the base system and the tools built
   (tools/dist.base); installed from a local copy of the ISO, then bind
   -ac /dist/9front /; cd /sys/src; mk install on the machine - the build
   is the PoC's test, people's to run; tools/test/run dist installs the
   ISO and checks its git and a small build.  The ISO is on
   plan2001.com/iso, the front page has its CD, power, BIOS and help.
9. The site's network (in progress, 2026-10-08; docs/network.md): a
   machine on plan2001.com reaches the internet (the web and git) through
   the server once its owner has shown a passkey - auth.plan2001.com, an
   origin apart from the machine's, the firmware asking before the disk's
   runs; a ticket for the tab.  The server's end is 9front's own:
   aux/ethernetd (DHCP, a filter, limits, a log; a pkt interface and the
   stack's NAT) and aux/etherws (sys/src/cmd/ethergw); tools/test/run
   ethergw tests it on a machine.  The server's side to do:
   docs/plan2001-server.md.  Found on the way, open: after
   rfork(RFPROC|RFFDG|RFMEM) the parent's read of a TCP connection
   (listen1's, fd 0) never returned, though the data was there (etherws
   forks without RFMEM, so it does not meet it); and alarm() did not
   interrupt a libthread proc's read of a pipe (ethernetd's timer has no
   notes).
