# Plan9-wasm32

Plan 9 for the wasm32 architecture: a kernel and its platform that run a
9front-based system in a web browser.  The browser is the machine: its
firmware is a page (`platform.js`, `index.html`), its processors Web
Workers, its disk a file in the origin's private file system (OPFS).

Plan9-wasm32 is based on 9front (http://9front.org), a separate project,
and builds from a pinned 9front release with this tree laid over it: new
files in 9front's root layout (`sys/`, `wasm32/`) and patches to
9front's own files (`patches/9front/`).

Imported 2026-10-05 from github.com/pereuna/Plan2001 (59ee476), where it
was first developed.

## Building

On Linux, with gcc, Python 3, 7z, bwrap and plan9port (mk, rc, yacc):

    PLAN9=$HOME/plan9port tools/build
    tools/serve              # http://127.0.0.1:18300/

- `tools/release` fetches the release (`tools/release.conf`) and checks its checksum.
- `tools/tree` makes the tree from it.
- `tools/hostcc/build` builds 3c, 3l and 3a for the host.
- `tools/native` builds the tree with 9front's mkfiles.
- `tools/bootfs` makes the boot file system.

`build/live` gets the page, the kernel and its boot file system: the
machine with the boot file system alone.

The distribution and its live image, as 9front's ISO, the whole release
built for wasm32 (`docs/install.md`):

    PLAN9=$HOME/plan9port tools/dist
    tools/serve -d build/dist/live     # then inst/start in it

`?iso=file` on the page boots a copy of the ISO on your own computer
instead of the server's.

The machine's network is an Ethernet card (`#l0`) whose wire is a
WebSocket to the page's origin (`/ether`); `tools/serve` relays it to
`tools/gateway`, the other end - a switch with a TAP device, 9front's
DHCP, TCP and the rest over it (`docs/kernel.md`):

    tools/gateway            # the Internet, as the host's programs reach it: passt
                             #   (sudo apt install passt, once); no root
    tools/gateway -n         # a network of its own, 10.0.2.0/24, DHCP, no way out (the tests)
    tools/gateway -t tap0    # a TAP made by root, bridged or NATed to the LAN

Try it: https://plan2001.com/plan9-wasm32/ (a live image, as 9front's ISO:
`inst/start` installs onto the browser's disk; `?iso=file` reads a copy of
the ISO on your own computer).

Status and plan: docs/plan.md.

## Testing

The kernel and the platform are tested in headless Chromium (`chromium`
on PATH), each test the machine's serial console against
`sys/src/9/wasm32/test/NAME.out`:

    PLAN9=$HOME/plan9port tools/test/run            # all, about 25 minutes
    PLAN9=$HOME/plan9port tools/test/run rc init    # these

`tools/test/run` lists the tests.  A failure leaves the machine's output
in `build/test/NAME.txt`, the page's log in `NAME.log`, and the screen in
`NAME.png`.  The network tests run `tools/gateway -n`, and in its
network `tools/test/authsrv` on 10.0.2.2: a dp9ik auth server and secstore
(unprivileged user namespaces and `/dev/net/tun`; `ip` from iproute2).
A new test is a program or an rc line in `tools/test/run` and its
expected output; the output's first line is `ticks in it: ok` with a
`\r\n` (the kernel's console), the others end in `\n`.

