# Installing Plan9-wasm32

As 9front is installed on a PC: boot the live image, run `inst/start`,
reboot from the disk; then the machine builds its own system from its
source, and updates it with git.

## The distribution

A binary base, the tools, and the whole source with its git:

- **The source** is this repository's `tree` branch (`tools/treebranch`),
  a snapshot: 9front's release (its commit's files, not its history -
  9front's own is `github.com/9front/9front`) and this tree's files over
  it (the port, 3c/3a/3l, the patched files), without 9front's jokes and
  quotations (`tools/dist.deny`); a new release or a new main is one more
  commit.  Its root is the machine's `/`.
- **The ISO** (`tools/dist`: `build/dist/plan9-wasm32.iso`, plan9port's
  dump9660, `-c9j` as 9front's mkfile) has the branch's files, and built
  for wasm32 only the base system and the tools (`tools/dist.base`: the
  file servers, rc and the commands the live system, termrc, rio and
  inst/start run, the network, an editor; 3c/3a/3l, mk, yacc, lex, ar,
  pcc, git), the libraries and the boot materials - and in
  `/dist/9front/.git` the branch, its work tree the root, as 9front's
  ISO has its repository.  `tools/dist` builds the whole
  release here too (`build/dist/failed`: what does not build yet, mostly
  APE programs): a check of the source, and where the base comes from.
- **On the site** the ISO is `https://plan2001.com/iso/`, as 9front's is
  `9front.org/iso/`: downloaded once, installed from the computer it is
  on (`?iso=file`).  The page and its kernel come from the site, as a
  machine's firmware: `index.html` is the browser tab's (the origin's)
  boot, and it gives the machine over to the ISO.

After the install, on the machine (as 9front's sysupdate):

    bind -ac /dist/9front /
    cd /sys/src
    mk install                  # the rest of the system, built here

and to update it, from this repository (the machine's network must
reach GitHub: `tools/gateway`, passt's, as offline the page's own):

    bind -ac /dist/9front /
    cd / && git/pull            # origin: https://github.com/pereuna/plan9-wasm32, branch tree
    cd /sys/src && mk install

A machine without `/dist/9front` gets it with
`git/clone -b tree https://github.com/pereuna/plan9-wasm32 /dist/9front`.
The kernel, once built (`cd /sys/src/9/wasm32; mk install`), goes onto
the boot FAT as inst/bootsetup puts it there.

## The live image

The live image is the page, the kernel, its boot file system, plan9.ini
and the ISO: `build/dist/live`.

    PLAN9=$HOME/plan9port tools/dist
    tools/serve -d build/dist/live     # http://127.0.0.1:18300/

plan9.ini there says `cdboot=yes` and `iso=plan9-wasm32.iso`.  The page
(the firmware) makes the ISO the machine's read-only disk `#S/sdW1`,
read from the server with HTTP range requests; or, with `?iso=file`, a
copy of the ISO on the person's own computer, chosen on the page (read
with FileReaderSync, nothing downloaded; the server gives the page, the
kernel and its boot file system only) - tools/test/run's isofile.  `/boot/init` does what
9front's bootrc does with `cdboot`: the CD's file system (9660srv) is the
root, and 9front's own init runs from it - `/lib/namespace`, termrc,
glenda's profile, rio.  The live system is a guest: nothing is written.

## inst/start

9front's installer, as it is, from a rio window or the console:

    inst/start

- **configfs**: hjfs, gefs or cwfs64x.
- **partdisk**: the disk is `sdW0`, a file in the origin's private file
  system (OPFS, 4 GB by default, `?disk=MB`); `gpt`, then edisk's
  suggestion (`w`, `q`): esp and plan9.
- **prepdisk**: prep's suggestion: 9fat, nvram, fs (or the esp as 9fat).
- **mountfs**.
- **confignet**: the Ethernet card, `/net/ether0` (`docs/kernel.md`):
  automatic (DHCP from the gateway's network) or manual.
- **mountdist** (`/`: the live root), **copydist** (the whole
  distribution, from the ISO onto the disk).
- **ndbsetup**: the machine's name and its card's address in the
  installed `/lib/ndb/local`, so that termrc configures it when it boots
  (the address is the browser's, the same each time).
- **tzsetup**.
- **bootsetup**: the boot FAT (9fat, or the esp) gets plan9.ini
  (`bootfile=9wasm32.wasm`, `bootargs=local!/dev/sdW0/fs ...`), the kernel
  and `/wasm32/9fat`: the kernel's boot file system (`9wasm32.fs`) and the
  page that is its firmware (`index.html`, `platform.js`) -
  `patches/9front/inst-bootsetup-9fat.diff`.
- **finish**: `fshalt -r`; the kernel flushes the disk and the page loads
  itself again (`#c/reboot`).

## Booting from the disk

The page at the server is the loader.  It looks at the disk first, as a
PC's firmware does (`platform.js`'s `diskboot`): the GPT, then 9fat in the
plan9 partition (prep's table) or the esp, and on it plan9.ini, the kernel
and its boot file system.  If the FAT has its own firmware (`index.html`,
`platform.js`: bootsetup put them there), the page gives the machine over
to it - the disk's page and platform run the disk's kernel - so the system
is the disk's until it is installed again (`window.plan9firmware` is
`disk` there).

`/boot/init` then mounts `bootargs`' file system (hjfs on `/dev/sdW0/fs`)
as the root and runs 9front's init from it.  The root's file server runs
in a name space of its own: the root, bound before `/` for 9front's init,
is not in the server's, or its halt (fshalt) would walk through itself.

`?boot=net` boots the live image again (to install anew); `?disk=0` boots
without the disk.  `?boot=` and `?iso=` are for one boot, as a PC boots
once from its CD: the reboot (`fshalt -r`, the installer's finish) loads
the page without them, so the installed system boots from its disk.

## The tests

`tools/test/run install` (with `build/dist` there): `tools/test/iso` makes a
small distribution (what the installer, termrc and rio run) and its live
image; inst/start is answered on the console, `fshalt -r` reboots, and the
page boots from the disk with the disk's firmware, the root the installed
hjfs.  `PROFILE=dir` keeps the browser's profile, and with it the disk,
between runs of `tools/test/wasmapp`.

`tools/test/run dist`, only when named (the ISO is large): the
distribution's own ISO installed from this computer (`?iso=file`), then
on the disk its git as sysupdate has it (`bind -ac /dist/9front /`: the
branch, at origin, the work tree clean) and a small build with the
machine's own tools - echo and hoc from `/sys/src/cmd` (mk, 3c, 3l,
yacc), installed and run.  The whole system's build (`cd /sys/src; mk
install`) is people's to run.
