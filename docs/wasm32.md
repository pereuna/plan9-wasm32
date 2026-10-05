# wasm32: WebAssembly as a Plan 9 architecture (3c, 3l)

WebAssembly is not a web port here but a processor architecture:
`objtype=wasm32`, the compiler `3c`, the loader `3l`, objects `.3`, as
5c 6c 7c 8c.  3 was free in 9front (4c was MIPS).  6c run in a browser is
still 6c (host wasm32, target amd64); 3c compiles for wasm32 wherever it
runs.

## Sources

- `sys/src/cmd/3c`: a back end on 9front's cc front end (`../cc`,
  pgen.c, pswt.c), laid out as 6c's: gc.h, txt.c, cgen.c, sgen.c, swt.c,
  list.c, enam.c, machcap.c; `3.out.h` is the instruction set and the
  object format.
- `sys/src/cmd/3l`: obj.c (objects and ar libraries), wasm.c (layout and
  the WebAssembly module).
- `wasm32/include/u.h`, `ureg.h`, `wasm32/mkfile` (CC=3c LD=3l O=3 AS=3a).
- `sys/src/cmd/3a`: a stub that says there is no assembler and fails:
  what other architectures have in assembler is C on wasm32.
- `sys/src/libc/wasm32`: what 386 has in assembler (main9, tas, atom,
  getfcr, setjmp, notejmp), and the system calls (C stubs,
  `patches/9front/libc-9syscall-wasm32.diff`).  The rest of libc is
  9front's port, 9sys and fmt as they are.
- On Linux 3c, 3l and 3a are built by `tools/hostcc/build`.

## The machine

An abstract machine with any number of typed registers: 3l makes them
WebAssembly locals and the browser's JIT allocates real registers, so 3c
has no register allocation and no Sethi-Ullman.

- Classes: `Kw` i32 (char, short, int, long, pointers), `Kv` i64
  (vlong), `Kf` f32, `Kd` f64.  A register is `(n<<2)|class`; n=0 is
  RET, n=1 SP.
- Three-address instructions: `op from, from2, to`, `to = from op
  from2`.  Only the MOVs (B, BU, H, HU, W, V, F, D) touch memory.
  CVT/CVTU convert between classes, CMPxx gives 0 or 1, BZ and BNZ
  branch; JMP, CALL, RET and COPY (memory.copy) are their own.
- ILP32, little-endian.  Struct fields and automatics aligned to 8 as in
  6c; arguments in 4-byte slots as in 8c, so Plan 9's `va_arg` works as
  it is.
- The calling convention: arguments in memory from SP+0 up, the result
  in the globals RET.w, RET.v, RET.f, RET.d.  A function that returns a
  struct gets the target's address at 0(SP), its arguments from 4.
  Every function's wasm type is `() -> ()`, so a function pointer is a
  table index and `call_indirect` works however the pointer was cast.
- The stack is in linear memory and grows down from the global SP.  A
  function takes its frame off it and keeps a copy in local 0.

## Control flow (3l)

pgen.c makes jumps as for every Ken compiler.  3l splits a function into
basic blocks and nests them in `block`s so that block j's code follows
block j's `end`.  A jump forward is a plain `br` out to its block.  A
jump back sets the block's number in local 1 and goes to the loop's
head, where a `br_table` picks the block.  Only functions with jumps
back get the loop.

## setjmp, longjmp and atomics (3l)

`setjmp` is made at its call: jmp_buf[0] gets the frame's SP and
jmp_buf[1] the number of the block after the call; the result is 0.
`longjmp(buf, v)` throws a WebAssembly exception (tag 0, (buf, v)).  A
function that calls setjmp is in a try block; its catch looks whether
buf[0] is its own SP.  If it is, it puts back the global SP (the
epilogues of the frames between were not run), sets the result (v, or 1
if v is 0) and goes through the loop's br_table to buf[1]'s block; else
it rethrows.  The locals are kept: it is the same call.  The legacy
exception instructions (try, catch, throw, rethrow) work in every
current browser.

`_tas`, `ainc`, `adec`, `cas`, `casp`, `casl` and `coherence` are made by
3l with WebAssembly's atomic instructions (xchg, add, sub, cmpxchg,
fence) when a program uses them and does not define them.

## fork: the stack unwound and rebuilt (3l)

A running WebAssembly execution cannot be cloned, so 3l makes the stack
one that can be saved, as asyncify does:

- From the call graph: a function can be on the stack at a fork if it
  calls `_trap`, calls through a pointer, or calls such a function.  Only
  these are changed.
- In a changed function each such call is a block of its own.  After the
  call: if `asstate` is 1 (unwinding), the function saves its block
  number and all its locals on the save stack (`asptr`, 256 KB after the
  bss) and returns.
- At a function's start: if `asstate` is 2 (rewinding), it takes its
  save off the stack, sets SP and goes through the br_table to the call's
  block, which calls the callee again.
- `_trap` ends the rewind: it sets the state to 0 and returns `asret`.

A fork: the kernel sets `asstate` to 1 and the stack unwinds to
`_start`; the kernel copies the memory for the child, and both call
`_start` again in state 2.  The parent's `asret` is the child's pid, the
child's 0.  The cost: changed programs are about 1.4-2 times larger, and
the changed calls slower.

## A process

One module 3l makes is one process, with a memory of its own, its own
address space:

- memory: [0, 4096) empty, then the stack (1 MB by default, `3l -s`),
  data, bss, `end`; the heap grows up from `end` (brk grows the memory);
- exports: `memory`, `sp`, `_start` (calls -E's, `_main` by default);
- the system call: `_trap(NUMBER, &first_argument)`, and the kernel reads
  the arguments from the process's memory (sys.h's numbers, 9front's
  order).

The kernel writes argc and argv[0] ... nil at the stack's top, sets `sp`
and calls `_start`.  `_main(int argc, char *arg0)` gets them as its
parameters, so `&arg0` is argv, as on Plan 9.

The kernel (`sys/src/9/wasm32`) runs each process on a Web Worker
(`platform.js`); its own module is built with `3l -k` and imports the
memory the page makes (`docs/boot.md`).
