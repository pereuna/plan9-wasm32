// Plan9-wasm32's machine: the platform, its firmware (docs/kernel.md).
// The kernel (9wasm32.wasm, 3c and 3l -k) imports its memory - shared,
// one for all its Workers - and the functions of platform.h, which it
// calls as any C function: their arguments are in memory at SP, results
// go back in RET (wasm32's calling convention).  Each Worker is a CPU:
// the first runs main, each platnewproc another (a kproc).  The page
// (kernel.html) is the machine's front: it makes the Workers and is
// #t/eia0's other end.  As the machine's firmware, boot() hands the kernel
// what it is as any loader does: a BootInfo blob (BootInfo v1,
// sys/include/bootinfo.h; docs/boot.md), its address _start's argument.
//
//   import { boot } from './platform.js'; boot('9wasm32.wasm', { eia(bytes), halt(why) })
// For tests, window.plan9: eia0bytes, eia0out, eia0b64, eia0in.

const PAGES = 1024, MAXPAGES = 16384;	// 64 MB, the kernel's (its image, its heap); 3l -k's maximum

/*
 * The BootInfo blob (bootinfo.h; docs/boot.md): the same data as the
 * UEFI loader's for amd64 and arm64.  3c lays the structs out as 6c does
 * (u64int 8-aligned, the size a multiple of 8): BootInfo 224 bytes,
 * BootMem 24.  The page puts, above the kernel's 64 MB, the blob, the
 * root's archive and the framebuffer, each page-aligned, and the memory
 * map says so: Conventional [0, 64 MB) - the kernel's module, its data
 * and stack at the bottom, the kernel keeps [0, end) - LoaderData the
 * blob and the archive, Reserved the framebuffer.
 */
const BI = { magic: 0x49423250, version: 1, arch: 4, size: 224, memsize: 24, pg: 4096 };
const BootMemLoaderData = 2, BootMemConventional = 7, BootMemReserved = 0;
const pground = (n) => Math.ceil(n / BI.pg) * BI.pg;

// init's argv as plan9.ini's init=: tokenize's quoting (rc's), a word with ' or blanks in quotes
function initconf(args) {
	if (!args || args.length === 0) return '';
	for (const a of args)
		if (/[\n\r\0]/.test(a)) throw new Error('platform: init\'s argument with a newline: ' + JSON.stringify(a));
	const q = (a) => a !== '' && !/[\s']/.test(a) ? a : "'" + a.replace(/'/g, "''") + "'";
	return 'init=' + args.map(q).join(' ') + '\n';
}

/*
 * front: its config (plan9.ini lines), args (init=), fs (the root's
 * archive), screen ({ w, h }); disks, sdW0, sdW1 ...: { size, ro } or
 * null: the machine's memory, the blob in it - { mem, pa (the blob's
 * address), fb ({ addr, stride, w, h }, or null), regs (each disk's
 * registers, or 0) }
 */
function firmware(front, disks = []) {
	const heap = PAGES * 65536;
	/* each disk's registers (devsdw.c): a page above the heap, Reserved, as *sdWn= says */
	let next = heap;
	const regs = disks.map((d) => d ? (next += BI.pg) - BI.pg : 0);
	const diskconf = disks.map((d, i) => d ? '*sdW' + i + '=0x' + regs[i].toString(16) + ' ' + d.size + (d.ro ? ' ro' : '') + '\n' : '').join('');
	/* the firmware's own device lines last: plan9.ini's later line wins (bootargs.c), and these are not the page's to change */
	const config = new TextEncoder().encode(initconf(front.args) + (front.config ?? '') + diskconf + '\0');
	const rd = front.fs ?? null;
	const sc = front.screen ?? { w: 1024, h: 768 };
	const fbw = sc.w | 0, fbh = sc.h | 0;
	const fbbytes = fbw > 0 && fbh > 0 ? fbw * fbh * 4 : 0;

	/* the blob: header, config, memory map (nmap entries at most) */
	const nmap = 4 + disks.length;
	const configoff = 256, mmapoff = configoff + Math.ceil(config.length / 8) * 8;
	const blobsize = pground(mmapoff + nmap * BI.memsize);
	const pa = next;
	const rdbase = rd && rd.length ? pa + blobsize : 0;
	const fbbase = fbbytes ? pground(pa + blobsize + (rd ? rd.length : 0)) : 0;
	const top = fbbase ? fbbase + pground(fbbytes) : pground(pa + blobsize + (rd ? rd.length : 0));
	const pages = Math.ceil(top / 65536);
	if (pages > MAXPAGES) throw new Error('platform: the machine wants ' + top + ' bytes, more than wasm32 has');
	const mem = new WebAssembly.Memory({ initial: pages, maximum: MAXPAGES, shared: true });

	const map = [[0, heap, BootMemConventional]];
	for (const r of regs) if (r) map.push([r, BI.pg, BootMemReserved]);
	map.push([pa, blobsize, BootMemLoaderData]);
	if (rdbase) map.push([rdbase, pground(rd.length), BootMemLoaderData]);
	if (fbbase) map.push([fbbase, pground(fbbytes), BootMemReserved]);

	const u8 = new Uint8Array(mem.buffer), d = new DataView(mem.buffer);
	const w32 = (o, v) => d.setUint32(pa + o, v, true);
	const w64 = (o, v) => d.setBigUint64(pa + o, BigInt(v), true);
	w32(0, BI.magic); w32(4, BI.version); w32(8, BI.size); w32(12, blobsize); w32(16, 0);
	w32(20, configoff); w32(24, config.length);
	w32(28, configoff + config.length); w32(32, 0);		/* logoff, loglen: no loader log */
	w32(36, mmapoff); w32(40, map.length); w32(44, BI.memsize);
	w64(48, 0);						/* acpi: none */
	w64(56, 0);						/* tscfreq: amd64's */
	w64(64, Math.floor(Date.now() / 1000));		/* epoch */
	const seed = new Uint8Array(64);			/* getRandomValues takes no shared memory */
	crypto.getRandomValues(seed);
	u8.set(seed, pa + 72); w32(136, seed.length);
	if (fbbase) {
		w64(144, fbbase); w64(152, fbbytes); w32(160, fbw); w32(164, fbh); w32(168, fbw); w32(172, 32);
		u8.set(new TextEncoder().encode('x8r8g8b8'), pa + 176);
	}
	w32(192, BI.arch);
	w32(196, configoff + config.length); w32(200, 0);	/* fdtoff, fdtlen: no device tree */
	w64(208, rdbase); w64(216, rdbase ? rd.length : 0);
	u8.set(config, pa + configoff);
	map.forEach(([b, n, t], i) => {
		const o = mmapoff + i * BI.memsize;
		w64(o, b); w64(o + 8, n); w32(o + 16, t); w32(o + 20, 0);
	});
	if (rdbase) u8.set(rd, rdbase);
	/*
	 * a test's (front.bad): a blob the kernel must refuse - for another
	 * ISA, the root's image in memory it hands out, the framebuffer
	 * there; front.oldheader: an older v1 loader's header, shorter than
	 * this kernel's (what lies past it must not be read)
	 */
	if (front.bad === 'arch') w32(192, 2);
	if (front.bad === 'rd') { w64(208, 0x100000); w64(216, 4096); }
	if (front.bad === 'fb') w64(144, 0x200000);
	if (front.oldheader) { w32(8, 208); w64(208, 0xdeadbeef); w64(216, 0x1000); }
	return { mem, pa, regs, fb: fbbase ? { addr: fbbase, stride: fbw * 4, w: fbw, h: fbh } : null };
}

// exec's: the program's frames unwind to platuser's loop, which starts the next
const EXEC = { exec: true };
// noted's: the program's notify handler's frames unwind to its caller (unote)
/* a function's address (3l): TEXTBASE + its index in the module's table, as a text segment's - never a small number */
const TEXTBASE = 0xF0000000;
const fidx = (p) => (p >>> 0) - TEXTBASE;
const NOTED = { noted: true };
// a program's fault: it ends with the note, as a trap would on Plan 9 - the machine goes on
class Trap extends Error {}

// a kernel function, fn(words...), from JS on this Worker: below the kernel's SP
function kcall(env, fn, ...w) {
	const x = env.x, k = x.sp.value, top = (k - 8 - 4*w.length) & ~7, d = new DataView(env.mem.buffer);
	w.forEach((v, i) => d.setUint32(top + 4*i, v, true));
	x.sp.value = top;
	x.table.get(fidx(fn))();
	x.sp.value = k;
}

// rfork(RFMEM): the memory's last proc is gone - the Worker leaves the kernel and ends
const MEMDONE = { memdone: true };

// the kernel's Ufns (platform.h): its functions for the program's Worker
function ufns(env, a) {
	const d = new DataView(env.mem.buffer), w = (i) => d.getUint32(a + 4*i, true);
	return { syscall: w(0), sysprep: w(1), sysdone: w(2), sysfin: w(3), sysret: w(4), coend: w(5), memend: w(6) };
}

// a note (trap.c, platnote): the program's handler(ureg, msg) below its SP,
// until noted (NOTED), it returns, or it jumps out (notejmp)
function unote(env) {
	const nt = env.note, u = env.user, x = u.x, sp = x.sp.value;
	env.note = null;
	const b = new Uint8Array(u.mem.buffer), d = new DataView(u.mem.buffer);
	let top = sp - 64 - (nt.msg.length + 1);
	b.set(nt.msg, top);
	b[top + nt.msg.length] = 0;
	const msg = top;
	top = (top - 8) & ~7;
	const ureg = top;
	d.setUint32(ureg, 0, true);	/* pc: wasm32 has none to give */
	d.setUint32(ureg + 4, sp, true);
	top -= 16;
	d.setUint32(top, ureg, true);
	d.setUint32(top + 4, msg, true);
	x.sp.value = top;
	if (!(fidx(nt.handler) > 0 && fidx(nt.handler) < x.table.length)) {
		/* not a function of this program's: as if it had no handler */
		console.log(`KLOG platform: a note's handler 0x${(nt.handler >>> 0).toString(16)} is not in this program (table ${x.table.length}): ${new TextDecoder().decode(nt.msg)}`);
		x.sp.value = sp;
		kcall(env, nt.done, nt.p);
		throw new Trap(new TextDecoder().decode(nt.msg));
	}
	try {
		x.table.get(fidx(nt.handler))();
	} catch (e) {
		if (e !== NOTED) {
			kcall(env, nt.done, nt.p);
			throw e;
		}
	}
	x.sp.value = sp;
	kcall(env, nt.done, nt.p);
}

// the program on this Worker (platuser), and each one exec makes next
function runuser(env, K, host, pid) {
	for (;;) {
		const job = env.exec;
		env.exec = null;
		env.note = null;	/* the program before's, not this one's */
		if (!job) throw new Error('platuser: no program');
		runprog(env, K, host, pid, job);
	}
}

const QUANTUM = 20000, SLICE = 10, FOREVER = 0x3fffffff;

/*
 * a program: its module, its memory its own.  job: { module, args, argc }
 * from exec, or a fork's child's { module, snap, asptr, base, top, fn }
 * (its memory, its context rewound with 0).  Its procs (rfork RFMEM, the
 * cos) take turns here; while there is one, its calls go to the kernel
 * as they come (syscall), and with more, each is prepared here, done by
 * the proc's helper if it blocks (sysprep, sysdone, sysfin), and the
 * next proc that can go, goes.  Contexts (libthread's threads): a proc's
 * stacks it switches between itself - _ctxnew(fn, arg, stk, n),
 * _ctxswitch(id), _ctxself, _ctxfree (calls 100-103, libc/wasm32/ctx.c).
 * Returns on exec.
 */
function runprog(env, K, host, pid, job) {
	const h = { multi: false, cos: [], cur: 0, ctxs: [], regions: new Map(), memdone: 0,
		block: 0, preempt: 0, ctxswitch: undefined, slice: 0 };
	let x = null;
	const inst = new WebAssembly.Instance(job.module, { plan9: { syscall: (n, a) => sys(n, a >>> 0) } });
	x = inst.exports;
	const mem = x.memory, tbl = x.table;
	env.user = { x, mem };
	const m32 = () => new DataView(mem.buffer);
	const kw = () => env.x.retw.value;

	/* a call's end on the program's side: exec, noted, a note */
	const after = (c, r) => {
		if (env.exec) throw EXEC;
		if (env.noted) { env.noted = false; throw NOTED; }
		if (x.asstate.value === 1) {	/* unwinding (fork): a note waits */
			if (env.note) { c.note = env.note; env.note = null; }
			return r;
		}
		if (!env.note && c.note) { env.note = c.note; c.note = null; }
		if (env.note) unote(env);
		return r;
	};
	const sys = (n, a) => {
		const c = h.cos[h.cur];
		/*
		 * a note that came while the proc was blocked (its call finished
		 * by its helper, it went on rewound: no after() for that call)
		 * goes to its handler now, before its next call - else a proc
		 * that blocks again (exportfs's slaves in rendezvous) never has it
		 */
		if (h.multi && c.note && x.asstate.value === 0) {
			env.note = c.note;
			c.note = null;
			unote(env);
			if (x.asstate.value === 1) return 0n;	/* the handler blocked, or its noted(NDFLT) ends the proc */
		}
		if (n >= 100 && n <= 103)
			return ctxcall(c, n, a);
		if (n === 105) {
			yieldcall(c);
			if (h.preempt) return 0n;
		}
		if (!h.multi) {
			kcall(env, K.syscall, n, a);
			return after(c, env.x.retv.value);
		}
		kcall(env, K.sysprep, c.p, n, a);
		if (kw() === 0) { h.block = 1; x.asstate.value = 1; return 0n; }	/* its helper has it: the next goes */
		kcall(env, K.sysfin, c.p);
		if (kw() === 1) { h.block = 1; x.asstate.value = 1; return 0n; }	/* a note ends it, on its helper */
		kcall(env, K.sysret, c.p);
		return after(c, env.x.retv.value);
	};

	/* the contexts' calls: 100 new, 101 switch, 102 free, 103 self */
	const ctxcall = (c, n, a) => {
		const d = m32(), arg = (i) => d.getUint32(a + 4*i, true);
		switch (n) {
		case 100: {
			const fn = arg(0), farg = arg(1), stk = arg(2), size = arg(3);
			let id = h.ctxs.findIndex((t) => t == null);
			if (id < 0) id = h.ctxs.length;
			h.ctxs[id] = { fn, arg: farg, base: stk, top: (stk + size) & ~15, asptr: stk, started: false };
			return BigInt(id);
		}
		case 101: {
			/* a context is the memory's; one not started goes with the first proc to switch to it */
			const id = arg(0);
			if (id == c.cx) return 0n;
			if (!h.ctxs[id] || h.cos.some((q) => q !== c && q.cx == id && q.state != 'done')) return -1n;
			h.ctxswitch = id;
			x.asstate.value = 1;
			return 0n;
		}
		case 102:
			if (h.ctxs[arg(0)] && !h.cos.some((q) => q.cx == arg(0) && q.state != 'done')) {
				h.ctxs[arg(0)].dead = true;
				h.ctxs[arg(0)] = null;
			}
			return 0n;
		case 103:
			return BigInt(c.cx);
		}
		return -1n;
	};

	/* 3l's preemption point (call 105): another proc goes if this one has had its slice */
	const done = (q) => { kcall(env, K.sysdone, q.p); return kw(); };
	const yieldcall = (c) => {
		x.preempt.value = h.multi ? QUANTUM : FOREVER;
		if (!h.multi || performance.now() - h.slice < SLICE) return;
		if (!h.cos.some((q) => q !== c && (q.state == 'ready' || q.state == 'blocked' && done(q) != 0))) return;
		h.preempt = 1;
		x.asstate.value = 1;
	};

	/*
	 * A context's region: its stack [.., top) and saved frames [base, asptr).
	 * rfork(RFMEM) procs share them at the same addresses, as Plan 9's
	 * private stack segments: memory holds one's, the owner's; the others'
	 * are kept here (t.saved) and swapped in when they go on.
	 */
	const minspOf = (t) => {
		const d = m32();
		let ms = t.top;
		for (let q = t.asptr; q > t.base; ) {
			const size = d.getInt32(q - 4, true), rec = q - size;
			ms = Math.min(ms, d.getUint32(rec + 4, true));
			q = rec;
		}
		return ms;
	};
	const save = (t) => {
		const ms = minspOf(t), b = new Uint8Array(mem.buffer);
		t.saved = { ms, stack: b.slice(ms, t.top), recs: b.slice(t.base, t.asptr) };
	};
	const claim = (t) => {
		const key = t.base + ':' + t.top, r = h.regions.get(key);
		if (!r) { h.regions.set(key, { owner: t }); return; }
		if (r.owner === t) return;
		if (r.owner && !r.owner.dead && r.owner.started) save(r.owner);
		if (t.saved) {
			const b = new Uint8Array(mem.buffer);
			b.set(t.saved.stack, t.saved.ms);
			b.set(t.saved.recs, t.base);
			t.saved = null;
		}
		r.owner = t;
	};
	/* go on with c's current context: rewound, or started; the function to call */
	const resume = (c) => {
		const t = h.ctxs[c.cx];
		claim(t);
		if (!t.started) {
			t.started = true;
			x.asstate.value = 0;
			const sp = (t.top - 16) & ~7;
			m32().setUint32(sp, t.arg, true);
			x.sp.value = sp;
			x.asptr.value = t.base;
			return tbl.get(fidx(t.fn));
		}
		x.asptr.value = t.asptr;
		x.asstate.value = 2;
		x.asret.value = c.ret;
		return t.fn ? tbl.get(fidx(t.fn)) : x._start;
	};
	/* libc's per-proc region (_perproc: _tos, privalloc's): swapped as procs take turns */
	const pp = x.perproc ? x.perproc.value : 0, ppn = x.perprocsize ? x.perprocsize.value : 0;
	const setpid = (v) => { if (pp && ppn >= 52) m32().setUint32(pp + 48, v, true); };	/* Tos.pid */
	const ppsave = () => pp ? new Uint8Array(mem.buffer, pp, ppn).slice() : null;
	const ppload = (v) => { if (pp && v) new Uint8Array(mem.buffer).set(v, pp); };

	let entry = x._start;
	if (job.snap) {
		if (job.snap.length > mem.buffer.byteLength)
			mem.grow(Math.ceil((job.snap.length - mem.buffer.byteLength) / 65536));
		new Uint8Array(mem.buffer).set(job.snap);
		h.ctxs = [{ fn: job.fn, base: job.base || x.asbase.value, top: job.top || x.stacktop.value, asptr: job.asptr, started: true }];
		h.cos = [{ p: host, state: 'run', ret: 0n, cx: 0 }];
		setpid(pid);
		entry = resume(h.cos[0]);
	} else {
		// argc, argv[0] ... nil at sp; the strings above them, as Plan 9's kernel does
		const b = new Uint8Array(mem.buffer), d = m32();
		let top = x.sp.value;
		const ptrs = [];
		for (let i = 0, o = 0; i < job.argc; i++) {
			let e = o;
			while (job.args[e]) e++;
			top -= e - o + 1;
			b.set(job.args.subarray(o, e + 1), top);
			ptrs.push(top);
			o = e + 1;
		}
		top &= ~7;
		top -= 4 * (ptrs.length + 2);
		top &= ~7;
		d.setInt32(top, ptrs.length, true);
		ptrs.forEach((q, i) => d.setInt32(top + 4 + 4*i, q, true));
		d.setInt32(top + 4 + 4*ptrs.length, 0, true);
		x.sp.value = top;
		setpid(pid);
		h.ctxs = [{ fn: 0, base: x.asbase.value, top: x.stacktop.value, asptr: 0, started: true }];
		h.cos = [{ p: host, state: 'run', ret: 0n, cx: 0 }];
	}

	/* an rfork(RFMEM) child of c: its stack and saved frames, at the same addresses, kept until it goes */
	const mkchild = (c, rf) => {
		const t = h.ctxs[c.cx];
		const ct = { fn: t.fn, base: t.base, top: t.top, asptr: t.asptr, started: true };
		claim(t);
		save(ct);
		const priv = ppsave();
		if (priv && priv.length >= 52) new DataView(priv.buffer).setUint32(48, rf.pid, true);
		h.ctxs.push(ct);
		return { p: rf.p, state: 'ready', ret: 0n, cx: h.ctxs.length - 1, priv };
	};

	for (;;) {
		h.block = 0;
		h.preempt = 0;
		h.ctxswitch = undefined;
		env.fork = null;
		env.rfmem = null;
		h.slice = performance.now();
		x.preempt.value = h.multi ? QUANTUM : FOREVER;
		let trap = null;
		try {
			entry();
		} catch (e) {
			if (e === EXEC) return;
			/* the program's fault (WebAssembly's trap, a bad table index, its stack): it ends, not the machine */
			if (!(e instanceof Trap || e instanceof WebAssembly.RuntimeError || e instanceof RangeError || e instanceof TypeError))
				throw e;
			trap = e instanceof Trap ? e.message : 'sys: trap: ' + e.message;
			console.log('KLOG platform: ' + trap);
			postMessage({ log: 'platform: ' + trap + ': ' + e.stack.replace(/\n\s*/g, ' < ') });	/* where, on the page's log */
		}
		let c = h.cos[h.cur];
		if (trap !== null) {
			/* exits(trap): its message at the top of its context's stack, which it has no more use for */
			const t = h.ctxs[c.cx], m = new TextEncoder().encode(trap.slice(0, 120) + '\0');
			const z = (t.top - 256) & ~7;
			new Uint8Array(mem.buffer).set(m, z);
			x.sp.value = z - 64;
			m32().setUint32(z - 8, z, true);
			x.asstate.value = 0;
			sys(8, z - 8);	/* one proc: pexit, no return; with more, its helper has it (h.block) */
			if (!h.block) throw new Error('platform: exits returned');
		}
		if (h.ctxswitch !== undefined) {
			/* another context of the same proc */
			h.ctxs[c.cx].asptr = x.asptr.value;
			c.cx = h.ctxswitch;
			c.ret = 0n;
			entry = resume(c);
			continue;
		}
		if (env.fork) {
			/* fork: a copy of the memory goes with the child's Worker (ready, platnewproc); c goes on */
			const f = env.fork, t = h.ctxs[c.cx];
			t.asptr = x.asptr.value;
			env.forkimage = { module: job.module, snap: new Uint8Array(mem.buffer).slice(),
				asptr: t.asptr, base: t.base, top: t.top, fn: t.fn };
			kcall(env, f.ready, f.p);
			env.forkimage = null;
			c.ret = kw() < 0 ? -1n : BigInt(f.pid);
			entry = resume(c);
			continue;
		}
		if (env.rfmem) {
			/*
			 * rfork(RFMEM): the child a proc of this memory, its calls on its
			 * helper, c's too from now - or, if a Worker could not be made,
			 * neither, and c as it was (sysproc.c rfmemstart)
			 */
			const rf = env.rfmem;
			h.ctxs[c.cx].asptr = x.asptr.value;
			const ch = mkchild(c, rf);
			kcall(env, rf.start, rf.p);
			if (kw() < 0) {
				h.ctxs.pop();
				c.ret = -1n;
			} else {
				if (!h.multi) {
					h.multi = true;
					h.memdone = rf.memdone;
					h.umem = rf.umem;
				}
				h.cos.push(ch);
				c.ret = BigInt(rf.pid);
			}
			c.state = 'ready';
		} else if (h.block) {
			h.ctxs[c.cx].asptr = x.asptr.value;
			c.state = 'blocked';
		} else if (h.preempt) {
			/* its slice is up: another goes, it is ready again */
			h.ctxs[c.cx].asptr = x.asptr.value;
			c.state = 'ready';
			c.ret = 0n;
		} else {
			/* main or a context's function returned: exits(nil) */
			const z = (x.sp.value - 8) & ~7;
			m32().setUint32(z, 0, true);
			x.asstate.value = 0;
			sys(8, z);
			if (!h.block) throw new Error('platform: exits returned');
			c.state = 'blocked';
		}
		/* the next to go: round robin, after what the helpers finished */
		let next = -1;
		for (;;) {
			const i32 = new Int32Array(env.mem.buffer);
			const seen = Atomics.load(i32, h.memdone >> 2);
			for (const q of h.cos) {
				if (q.state != 'blocked')
					continue;
				const st = done(q);
				if (st === 2) {
					q.state = 'done';
					h.ctxs[q.cx].dead = true;
					if (q.p !== host) kcall(env, K.coend, q.p);
				} else if (st === 1) {
					claim(h.ctxs[q.cx]);	/* its results go to its stack: in memory first */
					kcall(env, K.sysfin, q.p);
					if (kw() === 1) continue;
					kcall(env, K.sysret, q.p);
					q.ret = env.x.retv.value;
					if (env.note) { q.note = env.note; env.note = null; }
					q.state = 'ready';
				}
			}
			if (h.cos.every((q) => q.state == 'done')) {
				kcall(env, K.coend, host);
				kcall(env, K.memend, h.umem);
				throw MEMDONE;
			}
			for (let i = 1; i <= h.cos.length; i++) {
				const j = (h.cur + i) % h.cos.length;
				if (h.cos[j].state == 'ready') { next = j; break; }
			}
			if (next >= 0) break;
			Atomics.wait(i32, h.memdone >> 2, seen);
		}
		if (next != h.cur) {
			const was = h.cos[h.cur];
			if (was.state != 'done') was.priv = ppsave();
			ppload(h.cos[next].priv);
		}
		h.cur = next;
		c = h.cos[next];
		c.state = 'run';
		entry = resume(c);
	}
}

// the kernel's imports on a Worker: env { mem, x (its exports, once there), post, user, exec }
function imports(env) {
	const dv = () => new DataView(env.mem.buffer);
	const i32 = () => new Int32Array(env.mem.buffer);
	const arg = (i) => dv().getUint32(env.x.sp.value + 4*i, true);
	const iarg = (i) => dv().getInt32(env.x.sp.value + 4*i, true);
	const str = (p) => { const b = new Uint8Array(env.mem.buffer); let e = p; while (b[e]) e++; return new TextDecoder().decode(b.slice(p, e)); };
	const fns = {
		eiaout: () => env.post({ eia: new Uint8Array(env.mem.buffer).slice(arg(0), arg(0) + iarg(1)) }),
		eiaring: () => env.post({ ring: arg(0) }),	// uartwasm32.c's: r, w, b[8192]
		platnewproc: () => {
			const user = env.forkimage;
			env.forkimage = null;
			env.post({ spawn: { fn: arg(0), arg: arg(1), sp: arg(2), up: arg(3), what: iarg(4), user } }, user ? [user.snap.buffer] : []);
		},
		platnote: () => {
			const k = new Uint8Array(env.mem.buffer);
			let e = arg(1);
			while (k[e]) e++;
			env.note = { handler: arg(0), msg: k.slice(arg(1), e), done: arg(2), p: arg(3) };
		},
		platnoted: () => { env.noted = true; },
		platwait: () => {
			const r = Atomics.wait(i32(), arg(0) >> 2, iarg(1), iarg(2) < 0 ? Infinity : iarg(2));
			env.x.retw.value = r === 'ok' ? 0 : r === 'timed-out' ? 1 : 2;
		},
		platwake: () => { env.x.retw.value = Atomics.notify(i32(), arg(0) >> 2, iarg(1)); },
		platnsec: () => { env.x.retv.value = BigInt(Math.round(performance.timeOrigin * 1e6 + performance.now() * 1e6)); },
		platrandom: () => {
			const b = new Uint8Array(iarg(1));	/* getRandomValues takes no shared memory */
			for (let o = 0; o < b.length; o += 65536) crypto.getRandomValues(b.subarray(o, Math.min(o + 65536, b.length)));
			new Uint8Array(env.mem.buffer).set(b, arg(0));
		},
		platlog: () => env.post({ log: str(arg(0)) }),
		platmemsize: () => { env.x.retv.value = BigInt(env.mem.buffer.byteLength); },
		plathalt: () => env.post({ halt: arg(0) ? str(arg(0)) : '' }),
		platexec: () => {
			const k = new Uint8Array(env.mem.buffer);
			try {
				const module = new WebAssembly.Module(k.slice(arg(0), arg(0) + iarg(1)));
				env.exec = { module, args: k.slice(arg(2), arg(2) + iarg(3)), argc: iarg(4) };
				env.x.retw.value = 0;
			} catch (e) {
				env.x.retw.value = -1;
			}
		},
		platuser: () => runuser(env, ufns(env, arg(0)), arg(1), arg(2)),
		platcopyin: () => {
			const u = env.user, a = arg(1), n = iarg(2);
			if (!u || n < 0 || a + n > u.mem.buffer.byteLength) { env.x.retw.value = -1; return; }
			new Uint8Array(env.mem.buffer).set(new Uint8Array(u.mem.buffer, a, n), arg(0));
			env.x.retw.value = 0;
		},
		platcopyout: () => {
			const u = env.user, a = arg(0), n = iarg(2);
			if (!u || n < 0 || a + n > u.mem.buffer.byteLength) { env.x.retw.value = -1; return; }
			new Uint8Array(u.mem.buffer).set(new Uint8Array(env.mem.buffer, arg(1), n), a);
			env.x.retw.value = 0;
		},
		platustrlen: () => {
			const u = env.user, a = arg(0), max = iarg(1);
			if (!u) { env.x.retw.value = -1; return; }
			const b = new Uint8Array(u.mem.buffer), e = Math.min(b.length, a + max);
			let i = a;
			while (i < e && b[i]) i++;
			env.x.retw.value = i < e ? i - a : -1;
		},
		platrfmem: () => {
			env.rfmem = { p: arg(0), start: arg(1), pid: arg(2), umem: arg(3), memdone: arg(4) };
			env.user.x.asstate.value = 1;	/* unwind when the call returns */
		},
		platfork: () => {
			env.fork = { p: arg(0), ready: arg(1), pid: arg(2) };
			env.user.x.asstate.value = 1;	/* unwind when the call returns */
		},
		platbrk: () => {
			const u = env.user, want = arg(0), have = u.mem.buffer.byteLength;
			try {
				if (want > have) u.mem.grow(Math.ceil((want - have) / 65536));
				env.x.retw.value = 0;
			} catch (e) {
				env.x.retw.value = -1;
			}
		},
		platflush: () => env.post({ flush: [iarg(0), iarg(1), iarg(2), iarg(3)] }),
		platcursor: () => {
			const k = new Uint8Array(env.mem.buffer);
			env.post({ cursor: { x: iarg(0), y: iarg(1), clr: k.slice(arg(2), arg(2) + 32), set: k.slice(arg(3), arg(3) + 32) } });
		},
		platmousering: () => env.post({ mring: arg(0) }),
		platnetopen: () => env.post({ netopen: { id: iarg(0), gen: arg(1), path: str(arg(2)), ring: arg(3), st: arg(4) } }),
		platnetsend: () => {
			const b = new Uint8Array(env.mem.buffer).slice(arg(2), arg(2) + iarg(3));
			env.post({ netsend: { id: iarg(0), gen: arg(1), b } }, [b.buffer]);
		},
		platnetclose: () => env.post({ netclose: { id: iarg(0), gen: arg(1) } }),
		platkbdring: () => env.post({ kring: arg(0) }),
		platwebauthn: () => env.post({ webauthn: { gen: arg(0), req: arg(1) ? str(arg(1)) : null, buf: arg(2), n: iarg(3), word: arg(4) } }),
	};
	// a function the kernel wants and the platform has not: say which
	const platform = new Proxy(fns, { get: (o, k) => k === 'memory' ? env.mem : o[k] ?? (() => { throw new Error('platform: ' + String(k) + ' not here'); }) });
	return { plan9: { syscall: () => -1n }, platform };
}

// on a Worker: the kernel, and what this CPU runs
function cpu({ module, mem, role, fn, arg, sp, user }) {
	const env = { mem, x: null, post: (m, t) => postMessage(m, t ?? []), user: null, exec: null, fork: null, forkimage: null,
		rfmem: null, note: null, noted: false };
	if (user) env.exec = user;	/* a fork's child */
	try {
		const inst = new WebAssembly.Instance(module, imports(env));
		env.x = inst.exports;
		if (role === 'boot') {
			env.x._init();		// the kernel's data, once
			/* the entry ABI (docs/boot.md): _start(BootInfo's address), as a C call - at SP */
			const top = (env.x.stacktop.value - 16) & ~7;
			new DataView(mem.buffer).setUint32(top, arg, true);
			env.x.sp.value = top;
			env.x._start();		// main(pa)
		} else {
			const top = (sp - 16) & ~7;
			new DataView(mem.buffer).setUint32(top, arg, true);
			env.x.sp.value = top;
			env.x.table.get(fidx(fn))();
			// the proc is Dead and this Worker out of the kernel: a Worker less on its Proc and KSTACK (newproc)
			const gone = env.x.retw.value;
			if (gone) {
				const i32 = new Int32Array(mem.buffer);
				Atomics.sub(i32, gone >> 2, 1);
				Atomics.notify(i32, gone >> 2);
			}
			close();
		}
	} catch (e) {
		if (e === MEMDONE) {	/* rfork(RFMEM): the memory's procs are gone (runprog) */
			close();
			return;
		}
		postMessage({ halt: 'platform: ' + e + (e.stack ? ' ' + e.stack : '') });
	}
}

if (typeof WorkerGlobalScope !== 'undefined' && self instanceof WorkerGlobalScope)
	self.onmessage = (e) => e.data.role === 'disk' ? disk(e.data) : e.data.role === 'httpdisk' ? httpdisk(e.data) : cpu(e.data);

/*
 * The disk Worker (devsdw.c): the machine's disk is a file in the
 * origin's private file system (OPFS), the same file each time the page
 * is loaded - what the machine wrote is there after a reload.  Only a
 * Worker can use it synchronously (a sync access handle), and only one
 * at a time: another tab of the same origin gets no disk (after 3 s: a
 * page loaded again waits for the last one's to let go).  First the
 * file, at least size bytes ({ diskready: its size } or { diskfail });
 * then { mem, regs }: the kernel's requests, one at a time, in the
 * registers, into and out of its memory.  Writes go to the file (flush)
 * when the disk has been idle a moment, and on the kernel's flush.
 */
async function disk({ name, size, fail }) {
	let h;
	try {
		const dir = await navigator.storage.getDirectory(), fh = await dir.getFileHandle(name, { create: true });
		/* a page loaded again: the last one's disk Worker may hold the file a moment longer */
		for (let i = 0; ; i++) {
			try {
				h = await fh.createSyncAccessHandle();
				break;
			} catch (e) {
				if (e.name !== 'NoModificationAllowedError' || i >= 30) throw e;
				await new Promise((r) => setTimeout(r, 100));
			}
		}
		if (h.getSize() < size)
			h.truncate(size);
		if (fail === 'flush')	/* a test's: the file fails when it is flushed */
			h.flush = () => { throw new Error('the test fails the flush'); };
	} catch (e) {
		postMessage({ diskfail: String(e) });
		close();
		return;
	}
	self.onmessage = (e) => diskloop(h, e.data.mem, e.data.regs);
	postMessage({ diskready: h.getSize() });
}

/*
 * A read-only disk Worker (devsdw.c's ro): a file on the server, read with
 * HTTP range requests, or a file of the person's own (a File they chose:
 * FileReaderSync) - both synchronous, which a Worker may do, so the
 * disk's loop stays the OPFS disk's (diskloop).  The file is read in
 * blocks, kept in a cache (the last ones used), and a read that misses
 * takes some blocks after it too: a program is read from its start on.
 * First its size ({ diskready } or { diskfail }), then { mem, regs }.
 */
const HBLOCK = 64 * 1024, HCACHE = 512, HAHEAD = 8;
function httpdisk({ url, file }) {
	const get = file ? (start, end) => new Uint8Array(new FileReaderSync().readAsArrayBuffer(file.slice(start, end))) : (start, end) => {
		const x = new XMLHttpRequest();
		x.open('GET', url, false);
		x.responseType = 'arraybuffer';
		x.setRequestHeader('Range', 'bytes=' + start + '-' + (end - 1));
		x.send();
		if (x.status !== 206)
			throw new Error(url + ': ' + x.status + (x.status === 200 ? ' (no range requests)' : ''));
		return new Uint8Array(x.response);
	};
	let size;
	try {
		if (file) {
			size = file.size;
			if (!(size > 0)) throw new Error(file.name + ': empty');
		} else {
			const x = new XMLHttpRequest();
			x.open('HEAD', url, false);
			x.send();
			size = Number(x.getResponseHeader('Content-Length'));
			if (x.status !== 200 || !(size > 0))
				throw new Error(url + ': ' + x.status);
			get(0, 1);	/* ranges, or fail now */
		}
	} catch (e) {
		postMessage({ diskfail: String(e) });
		close();
		return;
	}
	const cache = new Map();	/* block number -> its bytes, in the order used */
	const block = (b) => {
		let v = cache.get(b);
		if (v) {
			cache.delete(b);
			cache.set(b, v);
			return v;
		}
		let n = 1;
		while (n < HAHEAD && (b + n) * HBLOCK < size && !cache.has(b + n))
			n++;
		const all = get(b * HBLOCK, Math.min((b + n) * HBLOCK, size));
		for (let i = 0; i < n; i++) {
			const x = all.subarray(i * HBLOCK, (i + 1) * HBLOCK);
			cache.set(b + i, x);
			if (i === 0) v = x;
		}
		while (cache.size > HCACHE)
			cache.delete(cache.keys().next().value);
		return v;
	};
	const h = {
		getSize: () => size,
		read: (v, { at }) => {
			let n = 0;
			while (n < v.length && at + n < size) {
				const b = Math.floor((at + n) / HBLOCK), o = (at + n) % HBLOCK, x = block(b);
				const k = Math.min(v.length - n, x.length - o);
				v.set(x.subarray(o, o + k), n);
				n += k;
			}
			return n;
		},
		write: () => { throw new Error('a read-only disk'); },
		flush: () => {},
		close: () => {},
	};
	self.onmessage = (e) => diskloop(h, e.data.mem, e.data.regs);
	postMessage({ diskready: size });
}

/* the disk's state Dead, and the request the kernel waits for ended with -1 (devsdw.c: Rstate, Rresult, Rdone) */
function diskdead(mem, regs) {
	const i32 = new Int32Array(mem.buffer), R = regs >> 2;
	Atomics.store(i32, R + 8, -1);
	i32[R + 7] = -1;
	Atomics.store(i32, R + 1, Atomics.load(i32, R));
	Atomics.notify(i32, R + 1);
}

function diskloop(h, mem, regs) {
	const i32 = new Int32Array(mem.buffer), R = regs >> 2;
	/* a view of the shared memory, or - if the handle takes none - a copy */
	const io = (op, addr, len, at) => {
		const v = new Uint8Array(mem.buffer, addr, len);
		try {
			return op === 1 ? h.read(v, { at }) : h.write(v, { at });
		} catch (e) {
			if (!(e instanceof TypeError)) throw e;
			const b = new Uint8Array(len);
			if (op === 1) {
				const n = h.read(b, { at });
				v.set(b.subarray(0, n));
				return n;
			}
			b.set(v);
			return h.write(b, { at });
		}
	};
	/* from the last request done: one the kernel made before the memory came here is still to do */
	let seen = Atomics.load(i32, R + 1), dirty = false;
	try {
		for (;;) {
			if (Atomics.load(i32, R) === seen && Atomics.wait(i32, R, seen, dirty ? 250 : Infinity) === 'timed-out') {
				h.flush();
				dirty = false;
				continue;
			}
			const seq = Atomics.load(i32, R);
			if (seq === seen)
				continue;
			seen = seq;
			const op = i32[R + 2], len = i32[R + 3], addr = i32[R + 4] >>> 0, at = (i32[R + 5] >>> 0) + (i32[R + 6] >>> 0) * 2 ** 32;
			let res = -1;
			if (op === 1 || op === 2) {
				res = io(op, addr, len, at);
				if (op === 2) dirty = true;
			} else if (op === 3) {
				h.flush();
				dirty = false;
				res = 0;
			}
			i32[R + 7] = res;
			Atomics.store(i32, R + 1, seq);
			Atomics.notify(i32, R + 1);
		}
	} catch (e) {
		/* the file failed (quota, I/O, the browser): the disk is dead, the kernel's request ends with -1 (devsdw.c) */
		console.log('KLOG platform: disk: ' + e + ': the disk is dead');
		diskdead(mem, regs);
		try { h.close(); } catch (e) {}
		close();
	}
}

// on the page: the machine
// front: { eia(bytes), halt(why), fs (the root's archive, devrootfs.c), args (init's argv: plan9.ini's init=),
//	disk ({ name, size }: the OPFS file that is the machine's disk, #S/sdW0, at least size bytes),
//	dist ({ url } or { file }: a file on the server, or a File the person chose, as a read-only disk,
//	#S/sdW1: the distribution),
//	config (more plan9.ini lines), canvas, screen ({ w, h }, 1024x768 without: the framebuffer, shown on
//	the canvas; its pointer the mouse) - what the firmware puts in BootInfo (firmware()),
//	ws (the machine's webterm: ws://host:port, the network's WebSockets),
//	failfork, failhelper, failrfmem (a test's: the nth fork's child, helper, rfork(RFMEM) child gets no Worker) }
/*
 * The firmware's boot from the disk (as UEFI boots from an esp): the OPFS
 * file read as it is (a File: no lock, before the disk Worker takes it),
 * a FAT on it: 9fat in the GPT's plan9 partition (prep's), else the EFI
 * system partition (9front's useesp), FAT12/16/32 with VFAT's long names;
 * on it plan9.ini, the kernel (bootfile=, default 9wasm32.wasm) and its
 * boot file system (bootfs=, default the kernel's name with .fs) - the
 * installer puts them there (inst/bootsetup).  null: no such disk, the network's then.
 */
export async function diskboot(name = 'sdW0') {
	const no = (why) => { console.log('KLOG platform: no disk boot: ' + why); return null; };
	/* a page loaded again: the last one's disk Worker may hold the file a moment longer (its sync handle) */
	let f = null;
	for (let i = 0; i < 20 && !f; i++) {
		try {
			f = await (await (await navigator.storage.getDirectory()).getFileHandle(name)).getFile();
		} catch (e) {
			if (e.name === 'NotFoundError' || e.name === 'TypeError') return no('no disk');	/* no disk yet */
			await new Promise((r) => setTimeout(r, 100));
		}
	}
	if (!f) return no('the disk is held');
	const rd = async (off, n) => new DataView(await f.slice(off, off + n).arrayBuffer());
	if (f.size < 34*512) return no('too small');
	const h = await rd(512, 92);
	if (String.fromCharCode(...new Uint8Array(h.buffer, 0, 8)) !== 'EFI PART') return no('no GPT');
	const lba = Number(h.getBigUint64(72, true)), nent = h.getUint32(80, true), esz = h.getUint32(84, true);
	const ents = await rd(lba*512, Math.min(nent, 128)*esz);
	const ESP = 'c12a7328-f81f-11d2-ba4b-00a0c93ec93b', PLAN9 = 'c91818f9-8025-47af-89d2-f030d7000c2c';
	const guid = (o) => {
		const b = new Uint8Array(ents.buffer, o, 16), x = (i) => b[i].toString(16).padStart(2, '0');
		return [3, 2, 1, 0].map(x).join('') + '-' + [5, 4].map(x).join('') + '-' + [7, 6].map(x).join('') + '-' +
			[8, 9].map(x).join('') + '-' + [10, 11, 12, 13, 14, 15].map(x).join('');
	};
	/* the FATs it can boot from: 9fat in the plan9 partition (prep's table, its sector 1: part NAME START END,
	   in its sectors) - 9front's own, the installer's default - else the esp */
	const fats = [];
	for (let i = 0; i < Math.min(nent, 128); i++) {
		const g = guid(i*esz), st = Number(ents.getBigUint64(i*esz + 32, true));
		if (g === PLAN9) {
			const t = new TextDecoder().decode(new Uint8Array((await rd((st + 1)*512, 512)).buffer));
			const m = t.startsWith('part') && t.match(/^part 9fat (\d+) (\d+)$/m);
			if (m) fats.unshift((st + Number(m[1]))*512);
		} else if (g === ESP)
			fats.push(st*512);
	}
	if (!fats.length) return no('no 9fat, no esp');
	const why = [];
	for (const start of fats) {
		const fat = await fatfiles(f, rd, start);
		if (typeof fat === 'string') { why.push(fat); continue; }
		const ini = await fat.get('plan9.ini');
		if (!ini) { why.push('no plan9.ini in ' + fat.names.join(' ')); continue; }
		const conf = new TextDecoder().decode(ini);
		const bootfile = (conf.match(/^bootfile=(\S+)/m) ?? [])[1] ?? '9wasm32.wasm';
		const bootfs = (conf.match(/^bootfs=(\S+)/m) ?? [])[1] ?? bootfile.replace(/\.wasm$/, '') + '.fs';
		const kernel = await fat.get(bootfile), fs = await fat.get(bootfs);
		if (!kernel || !fs) { why.push('no ' + bootfile + ' or ' + bootfs + ' in ' + fat.names.join(' ')); continue; }
		return { kernel, fs, conf, get: fat.get };
	}
	return no(why.join('; '));
}

/* a FAT12/16/32 file system at start in f (rd: a DataView of its bytes): { names, get(name): its bytes or null }, or why not */
async function fatfiles(f, rd, start) {
	const b = await rd(start, 512);
	const bps = b.getUint16(11, true), spc = b.getUint8(13), rsv = b.getUint16(14, true), nfat = b.getUint8(16);
	const nroot = b.getUint16(17, true), tot = b.getUint16(19, true) || b.getUint32(32, true);
	const fsz = b.getUint16(22, true) || b.getUint32(36, true);
	if (bps !== 512 || !spc || !nfat || !fsz) return 'not FAT';
	const rootsecs = Math.ceil(nroot*32/bps), data = rsv + nfat*fsz + rootsecs;
	const nclus = Math.floor((tot - data)/spc), bits = nclus < 4085 ? 12 : nclus < 65525 ? 16 : 32;
	const fat = new Uint8Array(await f.slice(start + rsv*bps, start + (rsv + fsz)*bps).arrayBuffer());
	const fv = new DataView(fat.buffer);
	const next = (c) => bits === 32 ? fv.getUint32(c*4, true) & 0x0fffffff : bits === 16 ? fv.getUint16(c*2, true) :
		(c & 1 ? fv.getUint16(c + (c >> 1), true) >> 4 : fv.getUint16(c + (c >> 1), true) & 0xfff);
	const end = bits === 32 ? 0x0ffffff8 : bits === 16 ? 0xfff8 : 0xff8;
	const csz = spc*bps, coff = (c) => start + (data + (c - 2)*spc)*bps;
	const chain = async (c, len) => {	/* a file's (len bytes) or a directory's (len < 0) clusters' bytes */
		const parts = [];
		let got = 0;
		for (let n = 0; c >= 2 && c < end && (len < 0 || got < len) && n < 1 << 22; n++, c = next(c)) {
			parts.push(new Uint8Array(await f.slice(coff(c), coff(c) + csz).arrayBuffer()));
			got += csz;
		}
		const a = new Uint8Array(len < 0 ? got : len);
		let o = 0;
		for (const p of parts) { a.set(p.subarray(0, Math.min(p.length, a.length - o)), o); o += p.length; if (o >= a.length) break; }
		return a;
	};
	const dir = bits === 32 ? await chain(b.getUint32(44, true), -1) :
		new Uint8Array(await f.slice(start + (rsv + nfat*fsz)*bps, start + data*bps).arrayBuffer());
	const files = new Map();
	let lfn = '';
	for (let o = 0; o + 32 <= dir.length && dir[o] !== 0; o += 32) {
		const e = new DataView(dir.buffer, dir.byteOffset + o, 32), attr = dir[o + 11];
		if (dir[o] === 0xe5) { lfn = ''; continue; }
		if (attr === 0x0f) {	/* VFAT: 13 UCS-2 characters, the last part first */
			let t = '';
			for (const k of [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]) {
				const ch = e.getUint16(k, true);
				if (ch === 0 || ch === 0xffff) break;
				t += String.fromCharCode(ch);
			}
			lfn = dir[o] & 0x40 ? t : t + lfn;	/* the entry with 0x40 holds the name's end */
			continue;
		}
		const short = String.fromCharCode(...dir.subarray(o, o + 8)).trim() + (dir[o + 8] !== 0x20 ? '.' + String.fromCharCode(...dir.subarray(o + 8, o + 11)).trim() : '');
		if (!(attr & 0x18))
			files.set((lfn || short).toLowerCase(), { clus: (bits === 32 ? e.getUint16(20, true) << 16 : 0) | e.getUint16(26, true), len: e.getUint32(28, true) });
		lfn = '';
	}
	return { names: [...files.keys()], get: async (n) => { const x = files.get(n.toLowerCase()); return x ? chain(x.clus, x.len) : null; } };
}

export async function boot(url, front = {}) {
	/* the kernel: a URL, or its bytes (the disk's: diskboot) */
	const module = typeof url === 'string' ? await WebAssembly.compileStreaming(fetch(url)) : await WebAssembly.compile(url);
	/*
	 * the disks first, as firmware finds its devices before it boots: sdW0 the OPFS file
	 * (front.disk), sdW1 the server's or a local file (front.dist: { url } or { file }, read-only);
	 * each a Worker and its size
	 */
	const want = [front.disk ? { role: 'disk', name: front.disk.name ?? 'sdW0', size: front.disk.size, fail: front.disk.fail } : null,
		front.dist ? { role: 'httpdisk', url: front.dist.url, file: front.dist.file } : null];
	const dws = await Promise.all(want.map(async (w, i) => {
		if (!w) return null;
		const dw = new Worker(import.meta.url, { type: 'module' });
		const r = await new Promise((done) => {
			dw.onmessage = (e) => done(e.data);
			dw.onerror = (e) => done({ diskfail: e.message });
			dw.postMessage(w);
		});
		if (!r.diskready) {
			console.log('KLOG platform: no disk sdW' + i + ': ' + r.diskfail);
			dw.terminate();
			return null;
		}
		dw.onmessage = (e) => { if (e.data.log !== undefined) console.log('KLOG ' + e.data.log); };
		return { dw, size: r.diskready, ro: w.role === 'httpdisk' };
	}));
	const { mem, pa, fb, regs } = firmware(front, dws.map((d) => d && { size: d.size, ro: d.ro }));
	dws.forEach((d, i) => {
		if (!d) return;
		Atomics.store(new Int32Array(mem.buffer), (regs[i] >> 2) + 8, 1);	/* Online (devsdw.c's Rstate) */
		d.dw.onerror = (e) => { console.log('KLOG platform: disk Worker: ' + e.message + ': the disk is dead'); diskdead(mem, regs[i]); };
		d.dw.postMessage({ mem, regs: regs[i] });
	});
	const eia = [];
	let ring = 0;		/* #t/eia0's input: the kernel's ring */
	let kring = 0;		/* #b/kbd's (devkbd.c) */
	const kbdq = [];	/* its messages the ring has had no room for yet */
	let kbdtimer = 0;
	const kbdpump = () => {
		kbdtimer = 0;
		if (!kring) { if (kbdq.length) kbdtimer = setTimeout(kbdpump, 20); return; }
		const i32 = new Int32Array(mem.buffer), u8 = new Uint8Array(mem.buffer), N = 4096;
		let w = Atomics.load(i32, (kring + 4) >> 2) >>> 0;	/* modulo 2^32, N a power of 2 */
		const w0 = w;
		while (kbdq.length && ((w - (Atomics.load(i32, kring >> 2) >>> 0)) >>> 0) + kbdq[0].length <= N)
			for (const c of kbdq.shift()) u8[kring + 8 + (w++ & (N - 1))] = c;
		if (w !== w0) {
			Atomics.store(i32, (kring + 4) >> 2, w | 0);
			Atomics.notify(i32, (kring + 4) >> 2);
		}
		if (kbdq.length) kbdtimer = setTimeout(kbdpump, 5);	/* the ring is full: when the kernel has read */
	};
	const me = import.meta.url;
	// a proc's Worker the page could not make: -1 in its word (procspawn waits on it)
	const failed = (job, why) => {
		console.log('KLOG platform: no Worker for a proc: ' + why);
		if (!job.up) return false;
		const i32 = new Int32Array(mem.buffer);
		if (Atomics.compareExchange(i32, job.up >> 2, 0, -1) !== 0) return false;
		Atomics.notify(i32, job.up >> 2);
		return true;
	};
	/*
	 * the screen (screen.c): the kernel's framebuffer, XRGB32 in the shared
	 * memory, drawn on front.canvas where it changed, once a frame; the
	 * canvas's pointer the mouse, into the kernel's ring
	 */
	const screen = { f: null, dirty: null, mring: 0, img: null, flushes: 0 };
	const canvas = front.canvas;
	screen.fb = (fb) => {
		screen.f = fb;
		if (!canvas) return;
		canvas.width = fb.w;
		canvas.height = fb.h;
		screen.img = canvas.getContext('2d').createImageData(fb.w, fb.h);
	};
	const paint = () => {
		const r = screen.dirty, f = screen.f;
		screen.dirty = null;
		if (!r || !f || !canvas) return;
		const src = new Uint8Array(mem.buffer), dst = screen.img.data;
		for (let y = r[1]; y < r[3]; y++) {
			let s = f.addr + y*f.stride + r[0]*4, d = (y*f.w + r[0])*4;
			for (let x = r[0]; x < r[2]; x++, s += 4, d += 4) {
				dst[d] = src[s + 2];
				dst[d + 1] = src[s + 1];
				dst[d + 2] = src[s];
				dst[d + 3] = 255;
			}
		}
		canvas.getContext('2d').putImageData(screen.img, 0, 0, r[0], r[1], r[2] - r[0], r[3] - r[1]);
		/* the kernel draws once (screeninit's grey); the next is a program's (rio): the page's to know */
		if (++screen.flushes === 2) front.drawn?.();
	};
	screen.flush = (r) => {
		const d = screen.dirty;
		screen.dirty = d ? [Math.min(d[0], r[0]), Math.min(d[1], r[1]), Math.max(d[2], r[2]), Math.max(d[3], r[3])] : r;
		if (!d) requestAnimationFrame(paint);
	};
	/* Plan 9's cursor (16x16: set black, clr white) as the canvas's */
	screen.cursor = (c) => {
		if (!canvas) return;
		const cc = document.createElement('canvas');
		cc.width = cc.height = 16;
		const g = cc.getContext('2d'), im = g.createImageData(16, 16);
		for (let i = 0; i < 256; i++) {
			const byte = i >> 3, bit = 0x80 >> (i & 7);
			const set = c.set[byte] & bit, clr = c.clr[byte] & bit;
			if (set || clr) {
				const v = set ? 0 : 255;
				im.data.set([v, v, v, 255], 4*i);
			}
		}
		g.putImageData(im, 0, 0);
		const hx = Math.min(15, Math.max(0, -c.x)), hy = Math.min(15, Math.max(0, -c.y));
		canvas.style.cursor = `url(${cc.toDataURL()}) ${hx} ${hy}, auto`;
	};
	/* the mouse: x, y, buttons (1 2 4; the wheel 8 16), msec into the kernel's ring */
	let buttons = 0;
	/*
	 * Events the ring has no room for wait here, in order: a move after a
	 * waiting move with the same buttons takes its place (the pointer is
	 * where it is now), a button or the wheel is never lost - a lost up
	 * would leave a button down
	 */
	const mouseq = [];
	let mousetimer = 0;
	const mousepump = () => {
		mousetimer = 0;
		if (!screen.mring) return;
		const i32 = new Int32Array(mem.buffer), r = screen.mring >> 2;
		let w = Atomics.load(i32, r) >>> 0;	/* modulo 2^32 */
		const w0 = w;
		while (mouseq.length && ((w - (Atomics.load(i32, r + 1) >>> 0)) >>> 0) < 64) {
			const [x, y, b, ms] = mouseq.shift(), e = r + 2 + 4*(w & 63);
			i32[e] = x; i32[e + 1] = y; i32[e + 2] = b; i32[e + 3] = ms;
			w++;
		}
		if (w !== w0) {
			Atomics.store(i32, r, w | 0);
			Atomics.notify(i32, r);
		}
		if (mouseq.length) mousetimer = setTimeout(mousepump, 5);
	};
	const mouse = (x, y, b, move) => {
		if (!screen.mring) return;
		const last = mouseq[mouseq.length - 1];
		if (move && last && last.move && last[2] === b)
			mouseq[mouseq.length - 1] = Object.assign([x, y, b, performance.now() | 0], { move });
		else
			mouseq.push(Object.assign([x, y, b, performance.now() | 0], { move }));
		if (!mousetimer) mousepump();
	};
	if (canvas) {
		const at = (e) => {
			const b = canvas.getBoundingClientRect();
			return [Math.round((e.clientX - b.left) * canvas.width / b.width), Math.round((e.clientY - b.top) * canvas.height / b.height)];
		};
		/* a finger is button 1, or the one the page's key bar has chosen (front.touchbutton: 1 2 4) */
		const bits = (e) => e.pointerType === 'touch' ? (e.buttons & 1 ? (front.touchbutton?.() ?? 1) : 0) :
			(e.buttons & 1 ? 1 : 0) | (e.buttons & 4 ? 2 : 0) | (e.buttons & 2 ? 4 : 0);
		canvas.addEventListener('pointermove', (e) => {
			const b = bits(e), moved = b === buttons;	/* buttons change on a move too (chords) */
			buttons = b;
			mouse(...at(e), buttons, moved);
		});
		canvas.addEventListener('pointerdown', (e) => { canvas.setPointerCapture(e.pointerId); buttons = bits(e); mouse(...at(e), buttons); e.preventDefault(); });
		canvas.addEventListener('pointerup', (e) => { buttons = bits(e); mouse(...at(e), buttons); });
		canvas.addEventListener('wheel', (e) => {
			const p = at(e), w = e.deltaY < 0 ? 8 : 16;
			mouse(...p, buttons | w);
			mouse(...p, buttons);
			e.preventDefault();
		}, { passive: false });
		canvas.addEventListener('contextmenu', (e) => e.preventDefault());
	}

	/*
	 * the network (devwsnet.c): conversation n a WebSocket to front.ws +
	 * path (the machine's webterm); what it brings into the kernel's ring
	 * (r, w, closed, b[64K]), the kernel woken; past a full ring the
	 * connection fails rather than lose bytes, as drawterm's wsock.c
	 */
	/*
	 * WebAuthn (devwebauthn.c, docs/webauthn.md): the machine's request
	 * shows a button - the browser wants a gesture for a passkey - and its
	 * click asks navigator.credentials, with the PRF extension; the answer
	 * goes into the kernel's buffer, then its length into the word.  A new
	 * request or the kernel's cancel (req null) takes the old one's
	 * button away, and its late answer is not written.  rp is the page's
	 * host or a domain above it
	 */
	const passkey = (() => {
		let cur = null, ui = null;
		const enc = (b) => { let s = ''; for (const c of new Uint8Array(b)) s += String.fromCharCode(c); return btoa(s).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, ''); };
		const dec = (s) => { const t = atob(s.replace(/-/g, '+').replace(/_/g, '/') + '==='.slice((s.length + 3) % 4)); return Uint8Array.from(t, (c) => c.charCodeAt(0)); };
		const hide = () => { ui?.remove(); ui = null; };
		const answer = (r, text) => {
			if (cur !== r) return;
			cur = null;
			hide();
			const b = new TextEncoder().encode(text), i32 = new Int32Array(mem.buffer);
			if (b.length > r.n) { Atomics.store(i32, r.word >> 2, -1); Atomics.notify(i32, r.word >> 2); return; }
			new Uint8Array(mem.buffer).set(b, r.buf);
			Atomics.store(i32, r.word >> 2, b.length);
			Atomics.notify(i32, r.word >> 2);
		};
		/*
		 * how: this device's (the browser picks: its password manager, Windows Hello ...),
		 * 'phone' a passkey on a phone (hints hybrid: the browser's QR code), 'key' a security
		 * key (hints security-key) - the last two, to make one, a cross-platform authenticator
		 */
		const ask = async (r, q, how) => {
			const away = how === 'phone' || how === 'key';	/* not this device's: a cross-platform authenticator */
			const hints = how === 'phone' ? { hints: ['hybrid'] } : how === 'key' ? { hints: ['security-key'] } : {};
			const salt = q.salt ? dec(q.salt) : null, ext = salt ? { prf: { eval: { first: salt } } } : {};
			const challenge = q.challenge ? dec(q.challenge) : crypto.getRandomValues(new Uint8Array(32));
			if (r.kind === 'create') {
				const c = await navigator.credentials.create({ publicKey: {
					rp: { id: q.rp, name: 'Plan9-wasm32' },
					user: { id: dec(q.user), name: q.name, displayName: q.name },
					challenge, pubKeyCredParams: [{ type: 'public-key', alg: -7 }],	/* ES256: libsec's P-256 */
					authenticatorSelection: { residentKey: 'required', userVerification: 'preferred', ...(away ? { authenticatorAttachment: 'cross-platform' } : {}) },
					...hints, attestation: 'none', extensions: ext } });
				const x = c.getClientExtensionResults().prf, pk = c.response.getPublicKey?.();
				return 'ok id=' + enc(c.rawId) + ' prf=' + (x?.results?.first ? enc(x.results.first) : 'none') +
					' prfok=' + (x?.enabled ? 1 : 0) + (pk ? ' pubkey=' + enc(pk) : '') +
					' client=' + enc(c.response.clientDataJSON) + ' attest=' + enc(c.response.attestationObject);
			}
			const allow = q.allow ? q.allow.split(',').map((id) => ({ type: 'public-key', id: dec(id) })) : [];
			const c = await navigator.credentials.get({ publicKey: { rpId: q.rp, challenge, allowCredentials: allow,
				userVerification: 'preferred', ...hints, extensions: ext } });
			const x = c.getClientExtensionResults().prf, u = c.response.userHandle;
			return 'ok id=' + enc(c.rawId) + ' prf=' + (x?.results?.first ? enc(x.results.first) : 'none') +
				(u ? ' user=' + enc(u) : '') + ' auth=' + enc(c.response.authenticatorData) +
				' client=' + enc(c.response.clientDataJSON) + ' sig=' + enc(c.response.signature);
		};
		return {
			request(m) {
				if (cur) answer(cur, 'error superseded');
				hide();
				if (m.req === null) { cur = null; return; }	/* the kernel's cancel */
				const r = { ...m, kind: m.req.split(' ')[0] }, q = {};
				for (const w of m.req.split(' ').slice(1)) { const i = w.indexOf('='); if (i > 0) q[w.slice(0, i)] = w.slice(i + 1); }
				cur = r;
				const host = location.hostname;
				if (!q.rp || !(host === q.rp || host.endsWith('.' + q.rp))) { answer(r, 'error rp ' + (q.rp ?? '') + ' is not this page\'s'); return; }
				if (r.kind === 'create' && (!q.user || !q.name)) { answer(r, 'error create wants user and name'); return; }
				if (!window.PublicKeyCredential) { answer(r, 'error no WebAuthn in this browser'); return; }
				ui = document.createElement('div');
				ui.id = 'webauthn';
				ui.style.cssText = 'position:fixed;left:50%;top:40%;transform:translate(-50%,-50%);z-index:10;display:flex;gap:12px;padding:16px;background:#ffffea;border:2px solid #000;font:16px sans-serif';
				const mk = (id, text) => { const b = document.createElement('button'); b.id = id; b.textContent = text; return b; };
				const go = mk('webauthn-go', (r.kind === 'create' ? 'Create a passkey for ' + q.name : 'Sign in with a passkey') + ': this device');
				const ph = mk('webauthn-phone', 'With a phone (QR code)'), key = mk('webauthn-key', 'Security key'), no = mk('webauthn-cancel', 'Cancel');
				const run = (how) => { go.disabled = ph.disabled = key.disabled = true; ask(r, q, how).then((t) => answer(r, t), (e) => answer(r, 'error ' + (e?.name || e))); };
				go.onclick = () => run('device');
				ph.onclick = () => run('phone');
				key.onclick = () => run('key');
				no.onclick = () => answer(r, 'error cancelled');
				ui.append(go, ph, key, no);
				document.body.append(ui);
			},
		};
	})();
	const net = { ws: new Map() };	/* n -> its conversation: { gen, ws, opened } */
	const NRING = 64*1024;		/* a power of 2: the counters run on modulo 2^32, the index masked */
	/*
	 * What comes faster than the kernel reads waits in the conversation's
	 * queue and goes into its ring as the kernel makes room; past QMAX the
	 * other end is not keeping to any flow control and the connection
	 * fails.  The other way the kernel reserves: it adds a piece to sendq
	 * in the ring before it sends it and waits past Sendhigh (devwsnet.c);
	 * the page takes off what has left the WebSocket's bufferedAmount.
	 */
	const QMAX = 1024*1024;
	net.open = ({ id, gen, path, ring, st }) => {
		net.close({ id });	/* an older gen's, if its close has not come yet */
		const i32 = new Int32Array(mem.buffer);
		const word = (a, v) => { Atomics.store(i32, a >> 2, v); Atomics.notify(i32, a >> 2); };
		/* the ring empty: only this thread writes it, and the kernel reads it only once this gen is open
		   (front.ringstart, a test's: the counters start there - across 2^31 and 2^32) */
		Atomics.store(i32, ring >> 2, (front.ringstart ?? 0) | 0);
		Atomics.store(i32, (ring + 4) >> 2, (front.ringstart ?? 0) | 0);
		Atomics.store(i32, (ring + 8) >> 2, 0);
		Atomics.store(i32, (ring + 12) >> 2, 0);
		const c = { gen, ws: null, opened: false, q: [], qlen: 0, pump: 0 };
		c.ring = ring;
		net.ws.set(id, c);
		const mine = () => net.ws.get(id) === c;
		const end = (why) => {
			if (!mine()) return;
			net.ws.delete(id);
			if (!c.opened) word(st, -1);
			else { Atomics.store(i32, (ring + 8) >> 2, why); Atomics.notify(i32, (ring + 4) >> 2); }
		};
		/* the queue into the kernel's ring, as much as it has room for; again soon while some is left */
		const deliver = () => {
			c.pump = 0;
			if (!mine()) return;
			const u8 = new Uint8Array(mem.buffer);
			let w = Atomics.load(i32, (ring + 4) >> 2) >>> 0;
			let room = NRING - ((w - (Atomics.load(i32, ring >> 2) >>> 0)) >>> 0), n = 0;
			while (c.q.length && room > 0) {
				const b = c.q[0], k = Math.min(b.length, room);
				for (let i = 0; i < k; i++) u8[ring + 16 + (w++ & (NRING - 1))] = b[i];
				room -= k;
				n += k;
				if (k === b.length) c.q.shift(); else c.q[0] = b.subarray(k);
			}
			if (n) {
				c.qlen -= n;
				Atomics.store(i32, (ring + 4) >> 2, w | 0);
				Atomics.notify(i32, (ring + 4) >> 2);
			}
			if (c.q.length) c.pump = setTimeout(deliver, 5);
		};
		const put = (b) => {
			if (c.qlen + b.length > QMAX) {
				end(3);
				c.ws?.close();
				return;
			}
			c.q.push(b);
			c.qlen += b.length;
			if (!c.pump) deliver();
		};
		let ws;
		try {
			ws = new WebSocket((front.ws ?? '') + path);
		} catch (e) {
			end(2);
			return;
		}
		ws.binaryType = 'arraybuffer';
		c.ws = ws;
		ws.onopen = () => {
			if (!mine()) return;
			if (!c.opened) { c.opened = true; word(st, 1); }
		};
		ws.onclose = ws.onerror = () => end(1);
		ws.onmessage = (e) => {
			if (!mine()) return;
			put(typeof e.data === 'string' ? new TextEncoder().encode(e.data) : new Uint8Array(e.data));
		};
	};
	/* k bytes of the kernel's the page no longer holds for c: off its reservation (never below 0), its writer woken */
	net.release = (c, k) => {
		if (k <= 0) return;
		const i32 = new Int32Array(mem.buffer), a = (c.ring + 12) >> 2;
		let v;
		do v = Atomics.load(i32, a);
		while (Atomics.compareExchange(i32, a, v, Math.max(0, v - k)) !== v);
		Atomics.notify(i32, a);
	};
	/* a test's: the most the kernel has had reserved in the page, as the page sees it */
	const seen = (c) => { net.sendqmax = Math.max(net.sendqmax ?? 0, Atomics.load(new Int32Array(mem.buffer), (c.ring + 12) >> 2)); };
	/* what has left the WebSocket's buffer is let go, looked at again until all has */
	const rawq = (c) => {
		c.rawt = 0;
		if (net.ws.get(c.id) !== c) return;
		const out = c.posted - (c.ws?.bufferedAmount ?? 0);
		net.release(c, out - c.released);
		c.released = out;
		if (c.posted > c.released) c.rawt = setTimeout(() => rawq(c), 20);
	};
	net.send = ({ id, gen, b }) => {
		const c = net.ws.get(id);
		if (!c || c.gen !== gen) return;
		seen(c);
		if (c.ws?.readyState === 1) {
			c.ws.send(b);
			c.id = id;
			c.posted = (c.posted ?? 0) + b.length;
			c.released ??= 0;
			if (!c.rawt) rawq(c);
		} else
			net.release(c, b.length);	/* nowhere to go: not held */
	};
	net.close = ({ id, gen }) => {	/* gen undefined: whichever */
		const c = net.ws.get(id);
		if (!c || gen !== undefined && c.gen !== gen) return;
		net.ws.delete(id);
		if (!c.ws) return;
		c.ws.onopen = c.ws.onclose = c.ws.onerror = c.ws.onmessage = null;
		c.ws.close();
	};

	/* a test's: the nth Worker of a kind is not made - a fork's child, a helper, an rfork(RFMEM) child's (platform.h) */
	const fails = { 1: front.failfork, 2: front.failhelper, 3: front.failrfmem }, made = { 1: 0, 2: 0, 3: 0 };
	const spawn = (job) => {
		if (fails[job.what] && ++made[job.what] === fails[job.what]) {
			failed(job, 'the test fails Worker ' + made[job.what] + ' of kind ' + job.what);
			return;
		}
		let w;
		try {
			w = new Worker(me, { type: 'module' });
		} catch (e) {
			if (!failed(job, e)) console.log('KERNEL-HALT platform: worker: ' + e);
			return;
		}
		w.onmessage = (e) => {
			const m = e.data;
			if (m.eia) { eia.push(m.eia); front.eia?.(m.eia); }
			if (m.spawn) spawn({ module, mem, role: 'proc', ...m.spawn });
			if (m.log !== undefined) console.log('KLOG ' + m.log);
			if (m.ring !== undefined) ring = m.ring;
			if (m.flush) screen.flush(m.flush);
			if (m.cursor) screen.cursor(m.cursor);
			if (m.mring !== undefined) screen.mring = m.mring;
			if (m.netopen) net.open(m.netopen);
			if (m.netsend) net.send(m.netsend);
			if (m.netclose) net.close(m.netclose);
			if (m.kring !== undefined) kring = m.kring;
			if (m.webauthn) passkey.request(m.webauthn);
			if (m.halt !== undefined) { console.log('KERNEL-HALT ' + m.halt); front.halt?.(m.halt); }
		};
		w.onerror = (e) => { if (!failed(job, e.message)) console.log('KERNEL-HALT platform: worker: ' + e.message); };
		w.postMessage(job, job.user ? [job.user.snap.buffer] : []);
	};
	window.plan9 = {
		get flushes() { return screen.flushes; },
		get sendqmax() { return net.sendqmax ?? 0; },
		eia0bytes: () => { const b = new Uint8Array(eia.reduce((n, c) => n + c.length, 0)); let o = 0; for (const c of eia) { b.set(c, o); o += c.length; } return b; },
		eia0out: () => new TextDecoder().decode(window.plan9.eia0bytes()),
		eia0b64: () => { let s = ''; for (const c of window.plan9.eia0bytes()) s += String.fromCharCode(c); return btoa(s); },
		eia0in: (x) => {
			if (!ring) return -1;
			const b = typeof x === 'string' ? new TextEncoder().encode(x) : new Uint8Array(x);
			const i32 = new Int32Array(mem.buffer), u8 = new Uint8Array(mem.buffer), N = 8192;
			let w = Atomics.load(i32, (ring + 4) >> 2) >>> 0, n = 0;	/* modulo 2^32, N a power of 2 */
			for (const c of b) {
				if (((w - (Atomics.load(i32, ring >> 2) >>> 0)) >>> 0) >= N) break;
				u8[ring + 8 + (w & (N - 1))] = c;
				w++;
				n++;
			}
			Atomics.store(i32, (ring + 4) >> 2, w | 0);
			Atomics.notify(i32, (ring + 4) >> 2);
			return n;
		},
		/* #b/kbd's messages (devkbd.c): r and a rune down, R up, c typed, each with its 0 */
		/*
		 * #b/kbd's messages (devkbd.c): r and a rune down, R up, c typed,
		 * each with its 0.  None is lost: what the ring has no room for
		 * waits here, in order, and goes in as the kernel reads
		 */
		kbd: (s) => {
			kbdq.push(new TextEncoder().encode(s));
			kbdpump();
			return 1;
		},
		eia0inb64: (b) => window.plan9.eia0in(Uint8Array.from(atob(b), (c) => c.charCodeAt(0))),
	};
	if (fb) screen.fb(fb);
	spawn({ module, mem, role: 'boot', arg: pa });
}
