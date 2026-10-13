---
id: sub-webkit
type: sub
title: "WebKit (Boosty) — JavaScriptCore on Pouch, its Baseline, DFG, FTL, regexp and Wasm BBQ and OMG JITs on one sealed code region, a collector that never suspends a thread, and what the engine asked the platform for"
parent: moc-userspace
code:
  - usr/ports/webkit/README.md
  - usr/ports/webkit/patches/0001-thylacine-jsconly-b0.patch
  - usr/ports/webkit/patches/0002-thylacine-jsc-jit-b2c.patch
  - usr/ports/webkit/patches/0003-thylacine-jsc-gc-park.patch
  - usr/ports/webkit/patches/0004-thylacine-jsc-dfg-b2d.patch
  - usr/ports/webkit/patches/0005-thylacine-jsc-ftl-b2e1.patch
  - usr/ports/webkit/patches/0006-thylacine-jsc-wasm-b2e3.patch
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
updated: 2026-10-13
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
region whose writer is reachable only through a sealed write thunk, and since
B-2d the DFG, since B-2e1 the FTL, and since B-2e3 Wasm's BBQ and OMG: every
tier upstream builds, each held off at run time until its own chunk opened it
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
  WebAssembly always; the Baseline JIT, the DFG, the FTL, the YARR JIT and
  Wasm's BBQ and OMG when the process holds `CAP_JIT`. Each was forced off in
  `Options::notifyOptionsChanged` under `OS(THYLACINE)` until its chunk opened
  it; patch 0006 removed the last of that gate (BBQ and OMG).
- Every BBQ and OMG install runs `Wasm::resetInstructionCacheOnAllThreads`
  after it writes the code and before it publishes it, and that function is
  `SYS_ICACHE_SYNC_SPACE` ([[dec-2026-10-13-wasm-space-sync]]): every other
  CPU running a thread of the process takes an interrupt, and a thread not
  running takes an `eret` before it runs. A failed sync is a
  `RELEASE_ASSERT`. The build refuses a `jsc` whose function does not put 128
  in `x8` before an `svc #0`.
- With no fault handler, no tier drops a null check on the strength of a
  fault: `maxAcceptableOffsetForNullReference` answers -1.
- A Wasm module's exit stubs are allocations that may fail: a full pool makes
  the compile a `CompileError` (an instance's wasm-to-wasm stubs: a
  `LinkError`), not a crash.
- A thread that resumes running the VM's code executes an ISB first
  (`MachineThreads::unparkCurrentThread`), so code another CPU patched -- the
  collector with the world stopped, a previous JSLock holder -- is never run
  from stale prefetch. Every Wasm instance executes one before it reads an
  exit stub or runs its start function
  (`JSWebAssemblyInstance::finalizeCreation`), and `Wasm::Module::compileSync`
  before it returns a group. The build refuses a `jsc` in which any of the four
  functions that must hold an `isb` holds none, or in which
  `finalizeCreation`'s comes after its first call into the module record.
- Every thread WTF creates (compilers, collector helpers, the Wasm worklist)
  gets a 1 MiB stack, not musl's 128 KiB default.
- The JIT pool is one 128 MiB code region (64 MiB until B-2e3): its exec alias (RX) is the
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
separated-WX heap for `OS(THYLACINE)` with no jump islands and a 64 MiB pool
(128 MiB since B-2e3, still one near branch's reach), so one near branch
reaches the whole pool (a `static_assert` proves it).
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

**The DFG (B-2d, patch 0004, three files).** `Options.cpp` lets the DFG run.
Baseline is unlinked (its calls and property accesses go through data); the
linked DFG rewrites instructions that run: a direct call linked or unlinked
(`CallLinkInfo`, `DirectCallLinkInfo`), a discarded function's invalidation
points replaced by jumps (`JumpReplacement::fire`), an OSR exit's jump pointed
at its lazily compiled ramp (`repatchJump`), an arithmetic IC's inline code
(`JITMathIC`), and, before the function is installed, a constant
(`LazyJSValue::emit`'s main-thread finalization task). Each goes through
`performJITMemcpy` and the thunk and is followed by `cacheFlush`. Baseline's
arithmetic ICs (`JITMathIC::generateOutOfLine`) rewrite a finished Baseline
function too, once each, on the JS thread. Three kinds
of thread write code: the JS thread (its `svc` returns through `eret`, which
synchronizes its own CPU), a `JITWorklist` thread (the JS thread installs its
code after `crossModifyingCodeFence`), and the collector, which discards and
unlinks code in its end phase with the world stopped -- and JavaScriptCore
stops the world only when no thread holds heap access (an access holder is
handed the collection and runs it on its own CPU). `SYS_ICACHE_SYNC`'s ISB
synchronizes only the CPU that called it, and heap access is retaken by a
compare-and-swap with no syscall, so `MachineThreads::unparkCurrentThread`
runs `WTF::crossModifyingCodeFence()` first: gcpark made it the one point every
resume passes through (both `acquireAccess` arms, and a JSLock entered on
access another thread holds), and it also covers a JSLock handed to a thread
on another CPU after its previous holder patched. HotSpot's
`cross_modify_fence` at safepoint exit is the precedent. `build_jsc`
disassembles the symbol-bearing `jsc` and refuses the build when that function
holds no `isb` (after checking it disassembled at least eight instructions),
because no run can show the fence missing. With jump islands off,
`ARM64Assembler.h`'s three 26-bit branch encoders (`linkJumpOrCall`,
`replaceWithJump`, `fillNearTailCall`) checked the range only in debug builds
and masked the offset in release; they `RELEASE_ASSERT` it now. The pool's
`static_assert` puts every branch inside the pool in range; the check catches
a target outside it. The DFG emits ARMv8.0 code only: Pouch compiles for
`armv8-a`, and `collectCPUFeatures` has no Thylacine arm, so LSE atomics,
`FJCVTZS`, FP16 and `FRINT` read their compile-time default, off.

**The FTL and the Wasm plan fence (B-2e1, patch 0005, four files).**
`Options.cpp` lets the FTL run; BBQ and OMG stay off. The FTL adds one kind of
rewrite to the DFG's: its property caches are code, not data (upstream forces
`useHandlerICInFTL` off), so `linkCodeInline` rewrites an access's whole
inline region in place, or `replaceWithJump` points it at a stub. A rewrite of
more than one instruction runs only on the JS thread, which is never inside
the region it rewrites (the slow-path call that triggers it sits outside);
the collector's resets are single instructions written with the world
stopped. Everything else the FTL patches -- OSR-exit jumps, lazy slow paths,
invalidation points, direct calls, arithmetic caches -- is a DFG kind made by
the same three kinds of thread, so B-2d's three rules cover it, and the FTL's
`JITFinalizer::finalize` runs `crossModifyingCodeFence` before it installs a
compiler thread's code, as the DFG's does. The FTL polls for traps
(`CheckTraps` lowers to a load and a lazy slow path), so a runaway FTL loop is
interruptible like a DFG one.

A Wasm module's exit stubs are JIT code written off the JS thread. Its
wasm-to-JS stubs come from the *validation* plan: `Module::Module` takes them
from the `IPIntPlan` of `Module::validateSync` or `validateAsync`, and the first
worklist thread to claim them generates them in `EntryPlan::compileFunctions`.
A CalleeGroup's wasm-to-wasm stubs are written by the thread that completes its
plan; the IPInt group completes on the thread that creates it
(`completeSyncIfPossible`), and another thread may take that group later. A
wait that finds a plan done (`Plan::waitForCompletion`) returns without
sleeping, so a JS thread could run stubs another CPU wrote with no
context-synchronization event between. Upstream has the same gap.
`JSWebAssemblyInstance::finalizeCreation`, which both instance-creation sites
call before any stub is read or the start function runs, and
`Wasm::Module::compileSync`, which returns a group, now run
`WTF::crossModifyingCodeFence()` first ([[dec-2026-10-13-wasm-plan-isb-writers]]); the
first is the one every consumer passes. `build_jsc`'s ISB check became a list
of four functions: `unparkCurrentThread` (patch 0004), the two plan consumers
(0005) and `FTL::JITFinalizer::finalize` (upstream's fence, asked for so that a
merge cannot drop it), and for `finalizeCreation` the `isb` must also come
before its first call into `WebAssemblyModuleRecord` (`initializeExports`, then
`evaluate`, which runs the start function): a fence moved below that call would
still be present. The B-2d `jsc` holds no `isb` in either plan consumer, so the
check can fail; the position test refuses a marker call laid out before the
`isb` and a call that is not there (run on the device `jsc`, 2026-10-13).

`WTF::stackSize()` gives every thread WTF creates 1 MiB on a libc that is
neither glibc nor bionic, because musl's default stack is 128 KiB; the arm
named only `OS(LINUX)`, so on Thylacine the compiler, collector and worklist
threads ran on 128 KiB (jsc's `PT_GNU_STACK` is empty, which leaves musl's
`__default_stacksize` alone), the same size as JSC's own soft reserved zone.
Patch 0005 adds `OS(THYLACINE)` to the arm.

**Wasm's BBQ and OMG (B-2e3, patch 0006, six files).** `Options.cpp` no
longer forces them off. Their code is written by a Wasm worklist thread (or
by the JS thread, for a plan it waits on) and reached by threads that pass no
fence of their own: a call into a BBQ or OMG function is a near `bl` that
`CalleeGroup::installOptimizedCallee` retargets in each caller
(`updateCallsitesToCallUs`), and an OMG loop entry is a callee
`OSREntryPlan` publishes for a running BBQ loop to jump into; the rest is
data. Upstream suspends every thread that ever entered a VM so that each
takes a context-synchronization event at its signal handler's return. Both
installers call `resetInstructionCacheOnAllThreads` after the new code is
written (its own outgoing calls linked) and before anything lets another
thread reach it -- the indirect-call entrypoint store and the callers'
retargeted calls, or the OSR-entry callee's publication -- under the
CalleeGroup's lock; patch 0006 makes it `SYS_ICACHE_SYNC_SPACE` (B-2e2,
[[sub-kernel-sched]]), whose wait depends on no userspace lock, only on other
CPUs taking an interrupt. The thread set upstream walked is no longer tracked.
`build_jsc` disassembles the function and requires that the last write to
`x8` before an `svc #0` put 128 there; the B-2e1 `jsc` (the thread walk, 90
instructions) fails it.

The pool rises to 128 MiB ([[dec-2026-10-13-jit-region-128]]), the kernel's
`JIT_REGION_MAX` since B-2e2 and exactly `nearJumpRange`, so the pool-size
`static_assert` still proves every branch in range without jump islands.

`maxAcceptableOffsetForNullReference` answered 0 with the fault handler off,
and BBQ's `emitRefTestOrCast` and B3's ref.cast lowering compare
`offset <= answer` without asking whether the handler is on (the other two
callers do ask). A `ref.cast` of a nullable reference to a non-null struct or
array type therefore dropped its null check and loaded the structure ID, at
offset 0, from null; the macOS `jsc` with `--useWasmFaultSignalHandler=false`
dies of SIGSEGV there under BBQ and under OMG and throws under IPInt. The
answer is now -1 wherever no offset is safe (no handler, no unmapped room
above null, off ADDRESS64), so every caller keeps its check and none changes.

`wasmToWasm` (`WasmBinding.cpp`) and `wasmToJS` (`WasmToJS.cpp`) asked the
pool `JITCompilationMustSucceed` and then tested `didFailToAllocate`, which
cannot fire: a full pool crashed the process where `EntryPlan` meant a
`CompileError` ("Out of executable memory at stub generation"). They ask
`JITCompilationCanFail`, so a stub fails at the pool's 25% reserve: a
wasm-to-JS stub (the module's validation plan) fails the compile with a
`CompileError`, a wasm-to-wasm stub (an instance's group) the instantiation
with a `LinkError`. The builtin trampoline keeps MustSucceed; it is reached
only under `JIT_CAGE`, which this port does not build. The `Wasm::Thunks`
(each generated by the first BBQ or OMG compile that needs it) and the
JSToWasm shared thunk also stay MustSucceed, as upstream's reserve intends: a
pool already full past the reserve at the first tier-up crashes there.

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
tests check their own result), `tiers` (every tier asked for by flag; Baseline
and DFG on), and `thunk`/`thunk-oob` (the probe above; each out-of-bounds call must
die of `snare:brk`, read from the kernel's `user fault:` console line, and
`snare:segv` would mean the store ran outside the region). Every marker is printed in lower case by the
guest and asserted in UPPER case through `tr`, and no expected string occurs
in the typed line (inputs are spelled `6*7`, `4e5`, `64<<20`): the terminal
echoes what is typed, and an expect the echo can satisfy proves nothing.
`$errstr` is never consulted; `ut` does not clear it after a success.

B-2d adds the DFG legs, each with a twin one flag away. `dfg`: a hot
function reaches DFG code (`$vm.dfgTrue()` folds to true only there);
`dfg-off` (`--useDFGJIT=false`) stops at Baseline, `dfg-deny` reaches
neither, `dfg-sync` (`--useConcurrentJIT=false`) compiles on the JS thread
and gets there too. `osr`: a DFG function speculating int32 gets a double
and a string -- two OSR exits through the one ramp compiled on the first,
its jump patched -- and both answers are right (the leg calls on until the
worker's compile has landed); `numberOfDFGCompiles` reads 1000000
whenever the DFG is off, so the leg asks for at least 1 and fewer than that.
`watch`: a DFG loop reading an array hole calls out to code that puts an
indexed getter on `Array.prototype`; the watchpoint discards the running
loop's code, the patched invalidation point sends it to Baseline, and the hole
reads 42 (a broken discard reads `undefined`); the call passes
`$vm.dfgTrue()`, so the leg also shows the loop was DFG code when the
watchpoint fired. `gc-jettison`/`gc-jettison-ctl`: twenty rounds each make
a fresh function DFG code that calls a fresh closure, which the DFG folds in
as a constant -- a weak reference of the DFG block. The closure is dropped
(the function kept) and jsc parks in `flashHeapAccess` under the continuous
collector; a DFG block whose weak reference died is unmarked, and the
collector's end phase discards it (`JettisonDueToWeakReference`), firing its
invalidation points and unlinking its callers. `$vm.codeBlockFor` reads the
function's tier without calling it, so DFG gone back to Baseline with no call
in between is a collector discard. The leg needs every round to reach DFG
code, every call right and at least one discard; its twin keeps each closure
alive and must see none (on the macOS `jsc`: 10-13 of 20 against 0; with the
DFG off no round reaches DFG code). The first version counted "due to
JettisonDueToOldAge" log lines, which JSC gives only an LLInt or Baseline
block: it passed with the DFG off (Fable r1 F1). Which thread conducted each end phase is not
observable. `loop-baseline` and `sunspider-baseline` record the DFG-off times
beside the all-on ones.

B-2e1 adds the FTL legs, the DFG's again: `ftl` (`$vm.ftlTrue()` folds to true
only in FTL code, and is false in DFG code, so it is never vacuous), `ftl-off`
(`--useFTLJIT=false` stops at the DFG), `ftl-deny`, `ftl-sync`. `ftl-osr`: an
FTL function speculating int32 takes a double and then a string, two exits
through one exit, and `--verboseFTLOSRExit` must log exactly one exit-stub
compile: the second exit took the jump the first patched. The tier probes
match the JIT-type word in `$vm.codeBlockFor`'s dump (`, FTLFunction`), since
the dump also prints `(FTLFail)` and `(HadFTLReplacement)` for other tiers.
`ftl-ic`: an FTL property cache meets four new shapes, one a getter, 1000
times each, and must stay in the same FTL code block (a call that left FTL
code would add 1000); with the FTL off it reads red. It cannot see a rewrite
that does nothing: with `replaceWithJump` gone from
`rewireStubAsJumpInAccess` and `linkCodeInline` refusing, it still passes,
since every access then takes the slow path, which answers right and returns
into the same code. It shows that the rewrites run without breaking the code;
`ftl-osr` is the discriminating patch witness. `wasm-sync` and
`wasm-async`: a module whose export calls a JS import, compiled each way, runs
its exit stubs 1001 times. `stack`: a runaway recursion on a `$.agent` thread
(a WTF thread like the compilers) must reach more than 131072/48 calls; an
LLInt frame is at least 48 bytes, so no 128 KiB stack holds that many
(`--useJIT=false` keeps one call per frame). `loop-dfg` and `sunspider-dfg`
record the FTL-off times; `gc-jettison` pins `--useFTLJIT=false`, because
its probe reads a DFG tier.

B-2e3 adds the Wasm tier legs, on hand-encoded modules (the host has no
`wat2wasm`; each module was validated, and each leg's exact command run, on
the macOS `jsc` first). A function reads its own tier as
`10*callerIsBBQOrOMGCompiled() + $vm.omgTrue()`, both imports looking at the
calling Wasm frame: 0 interpreted, 10 BBQ, 11 OMG. `wasm-tier` calls one until
it reads 11; `-bbq` (`--useOMGJIT=false`) stops at 10, `-ipint`
(`--useBBQJIT=false`) and `-deny` stay at 0. `wasm-patch`: `a(x)` calls `c()`
(when x is 1) through the near call BBQ links at a's install; `a(0)` runs until
a runs BBQ code, so c is still interpreted, then c alone is called until it
runs OMG code, and `a(1)` reads 11 only if c's installs retargeted a's call
(left alone it lands in c's interpreted code and reads 10).
`wasm-osr` and its `-bbq` and `-ipint` twins: one call of a 1e8-iteration
loop that then reads its own tier, so a call that starts interpreted and ends
in OMG (or BBQ) code entered it by loop OSR; each prints its time, the
benchmark. `refcast-bbq`, `-omg`, `-ipint`: a null `ref.cast` to a struct type
after the function reached each tier must throw a `RuntimeError`.
`wasm-canfail`: with `--useExecutableAllocationFuzz=true
--fireExecutableAllocationFuzzAtOrAfter=1` every CanFail allocation fails, and
a module with imports must be a `CompileError` naming the stubs, compiled
synchronously and by `WebAssembly.compile`; its twin without the fuzzer
compiles both ways. The macOS `jsc` (MustSucceed stubs) compiles under the
fuzzer, the behaviour the leg refuses. `wasm-canfail-group` covers the other
kind: one run counts the CanFail allocations a module costs (the verbose
fuzzer prints the count at exit), a second lets exactly those succeed, and
the instance's group must fail its first wasm-to-wasm stub with a
`LinkError`. Every Wasm leg, `wasm-sync` and `wasm-async` included, now needs
exit status 0 as well as its reading: with upstream's thread walk restored,
those two printed their readings and then died of `snare:abort` at a
background BBQ install, and passed. `tiers` reads every tier on.

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
cross-PE contract of `SYS_ICACHE_SYNC` for Baseline. The DFG adds two
writers that are not the thread that runs the code next: the collector, which
patches with the world stopped while the JS thread is parked, and a JSLock's
previous holder on another CPU. Both are covered by the ISB in
`MachineThreads::unparkCurrentThread`, which every resume passes (see
Mechanism). The FTL's compiler threads are the DFG's rule again (the
finalizer's fence). Wasm's exit stubs, written off the JS thread, are fenced
where an instance is created (B-2e1). BBQ and OMG code reaches threads that
pass no fence: upstream suspends every VM thread to publish it
(`resetInstructionCacheOnAllThreads`), and here that function is
`SYS_ICACHE_SYNC_SPACE` (B-2e3; [[dec-2026-10-13-wasm-space-sync]]), which
interrupts every other CPU running a thread of the process and returns once
each has taken the interrupt; a thread not running takes an `eret` before it
runs (B-2e2, [[sub-kernel-sched]]). It is called under the CalleeGroup's lock
(and the tier-up counter's); the kernel's wait takes no userspace lock.

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
remaining `Thread::suspend` users (a sampling profiler, signal-based VM traps)
stay off, and Wasm's instruction-cache reset no longer suspends (B-2e3); a
kernel thread suspend for the others is a separate question.

## Invariants enforced

- **[[inv-i12]]** (W^X, and I-42: JIT is a capability). No page of the pool
  is ever writable and executable through one alias; the writer alias is
  named only by the sealed, execute-only thunk; every write is followed by
  the fail-loud `SYS_ICACHE_SYNC` publish, and the thread that next runs
  patched code synchronizes its own CPU first (the `eret` after its own
  publish, `crossModifyingCodeFence` before installing a compiler thread's
  code, the ISB in `unparkCurrentThread` after any other CPU's, and for
  BBQ and OMG code the space sync before it is published); without
  `CAP_JIT` the pool is empty and nothing is generated.
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
`RELEASE_ASSERT`, and so is a failed space sync (the call returns 0). A Wasm
exit stub the pool refuses fails the module's compile with a `CompileError`,
or an instance's with a `LinkError` ("Out of executable memory at stub
generation" either way); a BBQ or OMG compile the
pool refuses leaves the function on its current tier.

A thread in the heap's group that is neither scanning, conducting nor parked
when the scan runs is fatal (`FATAL: the conservative scan found thread N
running without a published stack image`), never skipped: skipping it would
lose its roots silently.

A WebKit `CRASH()`/`RELEASE_ASSERT` is `abort()`. Since gcpark-b (2026-10-12)
a Pouch `abort()` dies of `snare:abort` ([[sub-pouch-process]]): the console
prints `user fault: pid=… reason="snare:abort" … pc=… lr=…`, so a crash
names its program counter without any diagnostic build, and a parent's
`waitpid` reads `WIFSIGNALED` `SIGABRT` (`ut` sets `$status` 134 under job
control and 1 on a plain wait). Before that it was a SILENT `_Exit(127)` —
the same number `ut` reports for "command not found", and `ut` does not
clear `$errstr` after a success, so a stale "spawn failed" could sit next to
it. B-0 lost an hour to exactly that pairing, and gcpark's first stop-the-world
death read the same way. The diagnostic that ended B-0's is twelve lines: an
`abort()` override linked ahead of `libc.a` that prints return addresses via
`_Unwind_Backtrace`, symbolized with `llvm-nm -n --demangle` (return
address − 4); the user-fault line's `pc` and `lr` now give the first two
frames for free.

## Performance

`fib(30)` = 832040 in 71–72 ms on the LLInt (HVF, M2). With the Baseline
JIT (B-2c, HVF, one run of `ls-jsc` on 2026-10-12): SunSpider 1.0.2 totals
112 ms with the JIT and 352 ms denied (each test the mean of three runs);
the 2e7-iteration int32 loop takes 442 ms against 931 ms. With the DFG (B-2d,
HVF, `ls-jsc` on 2026-10-13, one run): SunSpider 68 ms with every JIT on, 105
ms with the DFG off, 346 ms denied; the loop 193 / 433 / 917 ms. With the FTL
(B-2e1, HVF, three `ls-jsc` runs on 2026-10-13): SunSpider 95 / 91 / 97 ms with
every JIT on, 68 / 82 / 69 ms with the FTL off, 107 / 111 / 106 ms with the DFG
off, 342 / 367 / 347 ms denied; the loop 220 / 221 / 220, 192 / 193 / 193,
437 / 435 / 435 and 1008 / 916 / 915 ms. The FTL makes both short runs slower
in every run (its compile costs more than these runs save); claiming any FTL
gain needs a longer benchmark.

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
  the deny path. B-2d (patch 0004) has its own row: enumerate every thread
  that writes code and show the thread that next runs it synchronizes its CPU
  first; no multi-instruction rewrite of a range some thread is executing;
  the 26-bit release checks; the tier gate with the DFG on. gcpark (patch
  0003) has its own row: prosecute that every
  release of heap access parks and every acquire unparks, that the image is a
  copy covering the callee-saves and the stack to its origin, the exclusion
  that keeps records stable, and that no `Thread::suspend` caller is reachable.
  B-2e1 (patch 0005) has its own row: every FTL rewrite placed under B-2d's
  rules, the multi-instruction property-cache rewrites on the JS thread only
  and never inside a range it is executing, the two plan consumers' fences
  covering every path to the stubs, the tier gate with BBQ and OMG still off,
  and the worker stack size. B-2e3 (patch 0006) has its own row: every path
  that publishes BBQ or OMG code or retargets a call to it reaches the space
  sync after the write and before the publishing store; no tier drops a null
  check on a fault; a refused stub reaches a `CompileError` on every route.
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
kernel-emitted thunk and the `CAP_JIT` drop; and the Wasm cross-PE publish,
whose plan half closed at B-2e1 and whose BBQ and OMG half closed at B-2e3 on
B-2e2's kernel sync. B-2e3 also fixed the `ref.cast` that dropped its null
check with no fault handler and the exit stubs that crashed on a full pool
(both OPEN-BUGS 2026-10-13).

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
- A long BBQ or OMG loop polls no trap, upstream too (the signal path of
  `VMTraps` handles JS only), so once they run the only way to stop one is to
  kill the process.
- The pool is 128 MiB ([[dec-2026-10-13-jit-region-128]]): above 96 MiB
  JSC refuses every compile that may fail (the optimizing tiers and, since
  B-2e3, the Wasm exit stubs), and an allocation that must succeed (a JS
  thunk, a lazy stub) crashes the process if the last 32 MiB fill.
- B-0 found three Pouch libc defects by running (0033, 0034, 0035); they
  are described where they live, in [[sub-pouch-thread]] and
  [[sub-pouch-seam]].

## Provenance
