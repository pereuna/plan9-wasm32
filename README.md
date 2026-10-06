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

Status and plan: docs/plan.md.

## Testing

The kernel and the platform are tested in headless Chromium (`chromium`
on PATH), each test the machine's serial console against
`sys/src/9/wasm32/test/NAME.out`:

    PLAN9=$HOME/plan9port tools/test/run            # all, about 25 minutes
    PLAN9=$HOME/plan9port tools/test/run rc init    # these

`tools/test/run` lists the tests.  A failure leaves the machine's output
in `build/test/NAME.txt`, the page's log in `NAME.log`, and the screen in
`NAME.png`.  The network tests talk to `tools/test/authsrv`, a stand-in
for the origin's gateway with a dp9ik auth server, secstore and an rcpu
relay.  A new test is a program or an rc line in `tools/test/run` and its
expected output; the output's first line is `ticks in it: ok` with a
`\r\n` (the kernel's console), the others end in `\n`.

