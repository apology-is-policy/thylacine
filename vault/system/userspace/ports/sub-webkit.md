---
id: sub-webkit
type: sub
title: "WebKit (Boosty) — JavaScriptCore on Pouch, its Baseline and regexp JITs on one sealed code region, a collector that never suspends a thread, and what the engine asked the platform for"
parent: moc-userspace
code:
  - usr/ports/webkit/README.md
  - usr/ports/webkit/patches/0001-thylacine-jsconly-b0.patch
  - usr/ports/webkit/patches/0002-thylacine-jsc-jit-b2c.patch
  - usr/ports/webkit/patches/0003-thylacine-jsc-gc-park.patch
  - cmake/Toolchain-aarch64-pouch-cxx.cmake
  - cmake/Platform/Thylacine.cmake
  - usr/ports/webkit/sunspider-run.js
  - usr/lib/thylajit/thyla_jit_thunk.h
  - usr/pouch-hello/pouch-hello-jitthunk.c
  - tools/interactive/ls-jsc.exp
audit: hard
guarded-by: [inv-i12]
validated-by: [prose, gate-interactive]
locks: []
hazards: []
abis: []
design: [docs/BROWSER-DESIGN.md]
created: 2026-09-21
updated: 2026-10-12
---
## Purpose

The WebKit port for Boosty, the Thylacine web browser
([[dec-2026-09-21-browser-engine-order]]: WebKit first, then Servo). It is
still ONE thing: JavaScriptCore alone (`PORT=JSCOnly`), static, built against
Pouch + libc++ + a cross-built ICU, proven to run on the device. B-0 ran it
with every JIT tier off to measure what a real engine asks of the platform
before any kernel work was designed — the findings list in
`docs/browser-status.md` is its product as much as the binary is. Since B-2c
the Baseline JIT and the regexp (YARR) JIT run, on one `SYS_JIT_CREATE` code
region whose writer is reachable only through a sealed write thunk; every
other tier is built and held off at run time until its own chunk opens it
([[dec-2026-10-08-jsc-tiers-run-time]]).

The source is NOT vendored. WebKit is about 13 GB; it lives in a sparse,
partial clone beside the other forks (`~/projects/webkit-thylacine`, branch
`thylacine`), pinned to `webkitgtk-2.54.0` =
`5220e80b97a253c60ed899361654142ab5021998`. This directory carries the patch
series; `tools/forage.sh webkit` gathers the source and the ICU tarball, and
`CHUNK_WEBKIT=y` (default off) builds and bakes it -- the wiring is described in
[[sub-substrate-build]].

## Contract

- A ~69 MB static `ET_EXEC`, segments R / R+E / RW / RW, no `PT_DYNAMIC`, no
  segment both writable and executable ([[inv-i12]]: the image never is; JIT
  code exists only in the code region below).
- Tiers: the offlineasm LLInt (`ENABLE_C_LOOP=OFF`) and IPInt for
  WebAssembly always; the Baseline JIT and the YARR JIT when the process
  holds `CAP_JIT`. DFG, FTL and Wasm's BBQ and OMG are compiled in (upstream
  no longer builds JIT-on with them off on ARM64) and forced off in
  `Options::notifyOptionsChanged` under `OS(THYLACINE)`, after which the
  options are frozen: no flag can turn them on.
- The JIT pool is one 64 MiB code region: its exec alias (RX) is the
  reservation, the writer alias's address lives only as immediates inside an
  execute-only thunk sealed by `SYS_JIT_CREATE_SEALED`, and every write into
  the pool goes through that thunk, which traps (`brk #0xc471`, the Proc dies
  of `snare:brk`) on any write outside the region.
- `jsc` acquires `CAP_JIT` by the SELF form (its own kernel-stamped
  principal) before `JSC::initialize`; `--thylacine-no-cap-jit` skips that,
  and the refused `SYS_JIT_CREATE` (EACCES) leaves the pool empty, so JSC
  runs its interpreters. Any refusal does the same; only EACCES is quiet.
- `USE_SYSTEM_MALLOC=ON`: bmalloc/libpas is out, mallocng is in. Gigacage
  is therefore off.
- `Thread::suspend` is fatal on Thylacine rather than silently wrong, and
  since gcpark no collector path reaches it: the conservative scan reads the
  stack image a thread parks when it releases heap access (see Mechanism).

## Mechanism

**CMake needs a real platform module, not `Generic`.** `CMAKE_SYSTEM_NAME
Generic` loads `Platform/Generic.cmake`, which leaves `UNIX` unset after
the toolchain file runs — so a `set(UNIX 1)` in the toolchain is reset and
WebKit stops at "Unknown OS 'Generic'". `cmake/Platform/Thylacine.cmake`
sets `UNIX`, declares no shared-library support and the `.a` suffixes;
`cmake/Toolchain-aarch64-pouch-cxx.cmake` names the system `Thylacine`,
puts that directory on `CMAKE_MODULE_PATH`, selects the fork clang
(`~/projects/llvm-thylacine/build`, the driver that defines
`__thylacine__`) with the host LLVM's `ar`/`ranlib`/`nm`, points at
`build/sysroot`, takes extra roots (`THYLACINE_EXTRA_ROOTS`, how the ICU
stage is found), links `-static`, and sets
`CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY` so configure probes never
need a runnable link. Both files are port-neutral: any C++ CMake port can
use them.

**ICU is cross-built in two passes** (host tools first, then
`--with-cross-build`), static data packaging, `--disable-dyload`. The
target flags go in `CC` itself: with `--target` only in `CFLAGS`, ICU's
dependency-generation steps compile for macOS.

**All of WTF compiled against Pouch unpatched except five files, and all of
JavaScriptCore except two.** The whole delta is one patch, ten files,
+60/−3:

- `PlatformOS.h` — `OS(THYLACINE)` from the compiler's `__thylacine__`.
- `ARM64Assembler.h` `cacheFlush` — `SYS_ICACHE_SYNC` (103) by raw `svc`,
  `RELEASE_ASSERT` on failure. Dead code at B-0; since B-2c it is the publish
  of every JIT write.
- `InlineASM.h` + offlineasm `arm64.rb` — Thylacine takes the ELF branches
  (`HIDE_SYMBOL`, `.L` local labels, the `globaladdr` sequence).
- `MemoryFootprintGeneric.cpp` — upstream bit-rot: it calls `memoryStatus()`
  on platforms that do not declare it. Returns 0 elsewhere.
- `WasmFaultSignalHandler.cpp` + `Options.cpp` — the Wasm fault handler is
  compiled out where there is no machine context, and
  `useWasmFaultSignalHandler` is forced off there; upstream never builds
  Wasm-on with no machine context. `usePollingTraps` is on for Thylacine.
- `OptionsJSCOnly.cmake` — API tests off (the sparse clone has no gtest).
- Three PROBE-ONLY tolerances, each a recorded finding rather than a fix:
  `ThreadingPOSIX.cpp` skips the `SIGUSR1` suspend handler (Pouch's
  `sigaction` admits SIGINT/TERM/PIPE/CHLD only) and makes
  `Thread::suspend` fatal (F7); `OSAllocatorPOSIX.cpp` does not trim the
  slack of an aligned reservation and tolerates the `munmap` failure in
  `releaseDecommitted` (F3). Guard pages and decommit are silently absent
  because WTF ignores those results itself (F4, F5).

**The JIT (B-2c, patch 0002, five files).** `PlatformEnable.h` selects the
separated-WX heap for `OS(THYLACINE)` with no jump islands and a 64 MiB pool,
so one near branch reaches the whole pool (a `static_assert` proves it).
`ExecutableAllocator.cpp` `createThylacineJITPool` calls `SYS_JIT_CREATE`,
has `thyla_jit_thunk_create` emit and seal the write thunk, scrubs the
returned `thyla_jit_region` with volatile stores, installs the thunk as
`g_jscConfig.jitWriteSeparateHeaps` and adopts the exec alias through
`PageReservation::adoptExecutable` (`PageReservation.h`); commit and decommit
are no-ops there, because the region's pages are demand-zero and charged on
first touch (B-2a). `jsc.cpp` strips `--thylacine-no-cap-jit` from argv and
otherwise calls `thyla_acquire_cap_jit` (SELF form only:
`THYLA_CAPJIT_SELF_ONLY` compiles the bearer form out). `Options.cpp` holds
the run-time tier gate.

**The write thunk** is 33 hand-encoded A64 words from
`usr/lib/thylajit/thyla_jit_thunk.h`, never assembled at run time, so no
LinkBuffer ever holds a readable copy of the writer address. `write(offset,
src, n)`: `brk` if `offset > size` or `n > size - offset`, both unsigned (a
negative offset is a huge one); an 8-byte write at an 8-aligned destination
is one 64-bit store, so an Atomic repatch stays one store; otherwise 32-bit
stores, then a byte tail; `x3`, the register that held the writer address,
is zeroed before `ret`. The same header builds
`usr/pouch-hello/pouch-hello-jitthunk.c`, the witness: JSC never writes out
of range, so only a probe reaches the bounds. In bounds it writes at the
region's edges through the thunk (each store path, and a zero-length write at
the end) and reads every byte back through the exec alias; with an argument
it makes one out-of-bounds call (`1@end`, `5@end-4`, `8@-8`, `size+1@0`), one
process each because a Proc holding a live code region cannot fork (B-2a F3;
`jsc` itself never forks: no fork or spawn in JSC, WTF or bmalloc).

**The device gate.** `tools/interactive/ls-jsc.exp` runs six legs as the logged-in user, each of
which is the measured symptom of something: `hello` (exec + the LLInt),
`icu` (`1.234.567,891` through the static ICU data), `recurse` (a graceful
`RangeError` -- needs true main-thread stack bounds, pouch 0033), `heap`
(400,000 object pushes -- needs a true RAM figure, pouch 0034), `typed` (a
64 MiB typed array written at its last byte), `wasm` (the object exists and
validates a header, IPInt). B-2c adds the JIT legs, each paired with its
denied twin one flag away (a "false" or a "Can't JIT" alone is also what a JIT
that never started prints): `jit`/`jit-deny` (`$vm.baselineJITTrue()` in a
hot function), `rx`/`rx-deny` (a pattern through the YARR JIT; denied, JSC
says "Can't JIT"), `loop`/`loop-deny` (the same int32 sum both ways),
`sunspider`/`sunspider-deny` (SunSpider 1.0.2, three runs; 23 of its 26
tests check their own result), `tiers` (every tier asked for by flag, only
Baseline on), `dfg` (a hot function reaches Baseline code and never DFG code;
with the DFG on, `$vm.dfgTrue()` turns true within ~4e4 calls on a host
`jsc`), and `thunk`/`thunk-oob` (the probe above; each out-of-bounds call must
die of `snare:brk`, read from the kernel's `user fault:` console line, and
`snare:segv` would mean the store ran outside the region). Every marker is printed in lower case by the
guest and asserted in UPPER case through `tr`, and no expected string occurs
in the typed line (inputs are spelled `6*7`, `4e5`, `64<<20`): the terminal
echoes what is typed, and an expect the echo can satisfy proves nothing.
`$errstr` is never consulted; `ut` does not clear it after a success.

## Data structures

None of its own.

**The collector's scan (gcpark, patch 0003, `dec-2026-10-12-jsc-cooperative-scan`).**
JavaScriptCore's conservative scan suspended every thread in the heap's group
but the scanning one, and when a stop-the-world found the mutator without heap
access the collector thread conducted and suspended it -- fatal here (found
2026-10-12; jsc exited 127 at the first collection that stopped the world while
its one JS thread waited or idled with the JSLock dropped). On `OS(THYLACINE)`
a thread now *parks* whenever it releases heap access: `Heap::releaseAccess`
(before its fast CAS) and `releaseAccessSlow` (again before its clearing CAS,
because finalizers may have run in between) call
`MachineThreads::parkCurrentThread`, which captures the callee-saved registers
(`DECLARE_AND_COMPUTE_CURRENT_THREAD_STATE`, the same capture the scanning
thread uses on itself) and COPIES the stack from that point to the thread's
origin into a per-heap, per-thread image. Acquiring access unparks
(`acquireAccess`, both arms), and so does entering the JSLock on access some
other thread holds (`JSLock::didAcquireLock`), so an image is never older than
the thread's last use of the heap. `gatherFromParkedThreads` replaces the
suspend loop: every group thread other than the scanning and conducting ones
must be parked, its image goes to `ConservativeRoots`, and a thread that is not
is fatal with a `FATAL: the conservative scan found thread N ...` line. Records
of threads that left the group are pruned at each scan.
The image is a copy, not a recorded stack pointer as Go keeps for a goroutine
in a syscall: the frames between the releasing caller and the park point may
hold the only copy of a caller's callee-saved register, and they are reused as
soon as the release returns. It is complete because a thread without heap
access cannot obtain a cell it did not hold at the release.

## Concurrency

Baseline plans compile on `JITWorklist` threads: the worker's publish (svc
103: `IC IVAU` broadcast + `DSB ISH`) and the main thread's
`crossModifyingCodeFence` (`isb`) before `installCode` discharge the
cross-PE contract of `SYS_ICACHE_SYNC` for Baseline. Wasm thunks generated on
one agent thread and entered from another are B-2e's question.

JSC's GC and compiler threads exist as pthreads. B-0 recorded that the
collector's stop-the-world uses `Thread::suspend` "and no B-0 workload reached
it"; one JS thread was enough, and gcpark replaced the suspend with the parked
images. A record is written only by its own thread while that thread holds
heap access, and read only by the scan, which runs with the world stopped
(`Heap::gatherStackRoots` release-asserts it): no thread but the conductor can
hold access then, and a released thread cannot reacquire it until the world
resumes (`acquireAccessSlow` parks on `stoppedBit`). The thread group's lock
guards the records as well; records are per heap, so a thread using two VMs
cannot race one VM's scan with its record for the other. `Atomics.wait` blocks
holding access in this WebKit, so it is not a release and needs no image. The
remaining `Thread::suspend` users (a sampling profiler, signal-based VM traps,
Wasm's instruction-cache reset) stay off; a kernel thread suspend for them is a
separate question.

## Invariants enforced

- **[[inv-i12]]** (W^X, and I-42: JIT is a capability). No page of the pool
  is ever writable and executable through one alias; the writer alias is
  named only by the sealed, execute-only thunk; every write is followed by
  the fail-loud `SYS_ICACHE_SYNC` publish; without `CAP_JIT` the pool is
  empty and nothing is generated.
- The thunk's bounds: no write through it lands outside the region
  (witnessed by `thunk-oob`).
- Root completeness without suspension: every cell a thread will use after it
  reacquires heap access is in the image it parked (witnessed by `ls-jsc`'s
  gc legs: an object held only in a JS frame across a released wait survives
  40 rounds of collection, and jsc exits 0).

## Error paths

A refused `SYS_JIT_CREATE` or `SYS_JIT_CREATE_SEALED` leaves the reservation
empty (the region is destroyed if the thunk was refused), `canUseJIT` is
false, and JSC runs the LLInt and IPInt; EACCES is the deny path and prints
nothing, any other errno is logged. A write outside the pool executes the
thunk's `brk` and the Proc dies of `snare:brk`; a failed publish is a
`RELEASE_ASSERT`.

A thread in the heap's group that is neither scanning, conducting nor parked
when the scan runs is fatal (`FATAL: the conservative scan found thread N
running without a published stack image`), never skipped: skipping it would
lose its roots silently.

A WebKit `CRASH()`/`RELEASE_ASSERT` is `abort()`, which on Pouch is a
SILENT `_Exit(127)` — the same number `ut` reports for "command not found",
and `ut` does not clear `$errstr` after a success, so a stale "spawn
failed" can sit next to it. B-0 lost an hour to exactly that pairing. The
diagnostic that ended it is twelve lines: an `abort()` override linked
ahead of `libc.a` that prints return addresses via `_Unwind_Backtrace`,
symbolized with `llvm-nm -n --demangle` (return address − 4).

## Performance

`fib(30)` = 832040 in 71–72 ms on the LLInt (HVF, M2). With the Baseline
JIT (B-2c, HVF, one run of `ls-jsc` on 2026-10-12): SunSpider 1.0.2 totals
112 ms with the JIT and 352 ms denied (each test the mean of three runs);
the 2e7-iteration int32 loop takes 442 ms against 931 ms.

## Prosecution

- The three tolerances are probe posture. None may survive into a shipped
  browser: each either leaks VA (F3), never returns memory (F4), or removes
  a safety net (F5 guard pages; F7's suspend is answered for the collector
  by gcpark and stays fatal for every other user).
- The pin is a tag AND a hash; a codified build must check the hash.
- The JIT landed (B-2c): `cacheFlush` is live, this dossier is `audit: hard`,
  and the surface has its own row in `docs/AUDIT-TRIGGERS.md`. Prosecute: the
  thunk's bounds and encodings (the header is the one source; JSC and the
  probe both build from it), every pool write funnelling through the thunk,
  every write followed by a publish, the tier gate surviving every flag, and
  the deny path. gcpark (patch 0003) has its own row: prosecute that every
  release of heap access parks and every acquire unparks, that the image is a
  copy covering the callee-saves and the stack to its origin, the exclusion
  that keeps records stable, and that no `Thread::suspend` caller is reachable.
- Writer-address residue beyond the scrubbed struct and array (compiler
  temporaries, the kernel's out-struct copy) and a `CAP_JIT` that is never
  dropped are recorded (Fable r1 F4); the answer is a kernel-emitted thunk
  and a drop after setup, after B-2e.

## Seams

The platform findings F3–F9 in `docs/browser-status.md` are this port's
seams, and by the operator's direction (2026-09-21) their kernel design is
a conversation first. B-2c carries three owned obligations (OPEN-BUGS
2026-10-08): the content process must run without `/proc` in its namespace
(`/proc/self/maps` shows the writer row to the target itself); the
kernel-emitted thunk and the `CAP_JIT` drop; and the Wasm cross-PE publish at
B-2e.

## Caveats

- `build_sysroot` wipes `build/pouch/`, which holds the ICU and JSC objects
  ON PURPOSE (a static binary linked against the old `libc.a` must not
  survive a libc change) -- so with the chunk on, every Pouch patch costs a
  ~40 minute rebuild. That is the price of static linking, paid knowingly.
- The host has 8 GiB of RAM: `ninja -j5` for JSC; all of WebKit belongs on
  the remote builder.
- `tools/forage.sh webkit` widens an existing checkout's sparse cone with
  `sparse-checkout add`, never `set`, so a directory added by hand survives
  a re-forage.
- B-0 found three Pouch libc defects by running (0033, 0034, 0035); they
  are described where they live, in [[sub-pouch-thread]] and
  [[sub-pouch-seam]].

## Provenance
