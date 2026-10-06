# Installing Plan9-wasm32

As 9front is installed on a PC: boot the live image, run `inst/start`,
reboot from the disk.

## The live image

`tools/dist` builds the whole 9front release for wasm32 (with
`tools/native`: the libraries, the commands, the games; what does not
build yet is listed in `build/dist/failed`, mostly APE programs) and
makes of it an ISO 9660 image, as 9front's ISO: `build/dist/plan9-wasm32.iso`
(plan9port's mk9660, `-c9j` as 9front's mkfile).  9front's jokes and
quotations are left out (`tools/dist.deny`).

The live image is the page, the kernel, its boot file system, plan9.ini
and the ISO: `build/dist/live`.

    PLAN9=$HOME/plan9port tools/dist
    tools/serve -d build/dist/live     # http://127.0.0.1:18300/

plan9.ini there says `cdboot=yes` and `iso=plan9-wasm32.iso`.  The page
(the firmware) makes the ISO the machine's read-only disk `#S/sdW1`,
read from the server with HTTP range requests; or, with `?iso=file`, a
copy of the ISO on the person's own computer, chosen on the page (read
with FileReaderSync, nothing downloaded).  `/boot/init` does what
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
- **mountfs**, **mountdist** (`/`: the live root), **copydist** (the whole
  distribution, from the ISO onto the disk), **tzsetup**.
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
without the disk.

## The test

`tools/test/run install` (with `build/dist` there): `tools/test/iso` makes a
small distribution (what the installer, termrc and rio run) and its live
image; inst/start is answered on the console, `fshalt -r` reboots, and the
page boots from the disk with the disk's firmware, the root the installed
hjfs.  `PROFILE=dir` keeps the browser's profile, and with it the disk,
between runs of `tools/test/wasmapp`.
