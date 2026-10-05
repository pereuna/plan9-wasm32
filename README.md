# Plan9-wasm32

Plan 9 for the wasm32 architecture: a kernel and its platform that run a
9front-based system in a web browser.  The browser is the machine: its
firmware is a page (`platform.js`, `index.html`), its processors Web
Workers, its disk a file in the origin's private file system (OPFS).

Plan9-wasm32 is based on 9front (http://9front.org), a separate project,
and builds from a pinned 9front release with this tree laid over it: new
files in 9front's root layout (`sys/`, `wasm32/`) and patches to
9front's own files (`patches/9front/`).

Imported 2026-10-05 from Plan2001 (github.com/pereuna/Plan2001, 59ee476),
where it was developed as `plan9/`; Plan2001's services (its accounts
and their passkey login, its cloud) stay there.

## Building

On Linux, with gcc, Python 3, 7z, bwrap and plan9port (mk, rc, yacc):

    PLAN9=$HOME/plan9port tools/build
    tools/serve              # http://127.0.0.1:18300/

- `tools/release` fetches the release (`tools/release.conf`) and checks its checksum.
- `tools/tree` makes the tree from it.
- `tools/hostcc/build` builds 3c, 3l and 3a for the host.
- `tools/native` builds the tree with 9front's mkfiles.
- `tools/bootfs` makes the boot file system.

The live image is put in `build/live`.

Status and plan: docs/plan.md.
