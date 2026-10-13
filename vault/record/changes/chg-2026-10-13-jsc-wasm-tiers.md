---
id: chg-2026-10-13-jsc-wasm-tiers
type: chg
title: "JavaScriptCore's Wasm BBQ and OMG on the space sync: patch 0006, the pool at 128 MiB, the ref.cast null check kept, CanFail exit stubs"
date: 2026-10-13
arc: arc-boosty
commits: ["751e5a27f", "1dbd72da1", "10a0c3f8c"]
touched:
  - sub-webkit
  - sub-substrate-build
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-10-13
---
B-2e3 of the browser arc: JavaScriptCore's Wasm BBQ and OMG tiers run on
the sealed code region, synchronized across CPUs by the kernel call B-2e2
landed ([[chg-2026-10-13-icache-sync-space]],
[[dec-2026-10-13-icache-sync-space]]). No vote was owed: BROWSER-DESIGN 7's
B-2e bullets and the 2026-10-13 votes decide it. This was the last tier held
off at run time; every JavaScriptCore tier is now built and on.

**What.** [[sub-webkit]], patch 0006 (fork `7cf380d5ea` on `5e8cf8cd83`):
`Wasm::resetInstructionCacheOnAllThreads` calls `SYS_ICACHE_SYNC_SPACE` in
place of upstream's walk of every thread that has run Wasm (a signal to
each, which Thylacine's threads cannot take), and release-asserts its
answer; the thread tracking that filled the walk's set is compiled out. Its
three callers (two in `WasmCalleeGroup.cpp`, one in `WasmOSREntryPlan.cpp`)
already call it after writing the code and before the store that lets
another thread reach it, under the CalleeGroup lock; the kernel's wait takes
no userspace lock, so holding it is safe. The THYLACINE force-off of BBQ and
OMG is gone. The executable pool is 128 MiB, at JavaScriptCore's near-jump
range, matching `JIT_REGION_MAX` ([[dec-2026-10-13-jit-region-128]]). With no fault handler,
`maxAcceptableOffsetForNullReference` answers -1 in all three of its
none-cases, so no offset is taken as safe and every caller keeps its null
check (the BBQ and OMG `ref.cast` paths elided it on 0 and loaded from null:
`snare:segv` where the spec traps). The Wasm exit stubs (`wasmToWasm` in
`WasmBinding.cpp`, `wasmToJS` in `js/WasmToJS.cpp`) ask the pool
`JITCompilationCanFail`, so a full pool is a `CompileError` (module) or a
`LinkError` (instance) instead of a crash. The `Wasm::Thunks` and the
JSToWasm shared thunk stay MustSucceed, upstream's design for the pool's 25%
reserve. [[sub-substrate-build]]: `build_jsc` asks the binary for the call --
the last write to `x8` before an `svc #0` in
`resetInstructionCacheOnAllThreads` must be a `mov` of 128 -- and fails
closed.

**Why the call, not the walk.** Upstream's walk suspends each tracked Wasm
thread by a signal (WTF's `sendMessage`) and runs a cross-modifying-code
fence in its handler. Thylacine delivers no such signal to a chosen thread:
with the walk restored (R-sync) the first BBQ install died of `snare:abort`.
The kernel call reaches exactly the CPUs running the caller's address space,
whether or not their thread ever ran Wasm (design
[[chg-2026-10-13-b2e2-design]]).

**Witnesses** (ls-jsc). Each function reads its own tier as
`10*callerIsBBQOrOMGCompiled() + $vm.omgTrue()` (0 IPInt, 10 BBQ, 11 OMG):
`tiers` reads every tier on, with a twin with each tier off and one with
the JIT denied; one call enters OMG by loop OSR; `wasm-patch` reads a BBQ
caller reaching its callee's OMG code through the repatched call (`a(0)`
reaches BBQ while `c` is still interpreted, so the order is fixed); a null
`ref.cast` traps under IPInt, BBQ and OMG; JavaScriptCore's allocation
fuzzer fails every CanFail allocation and the module reads `CompileError`
(`wasm-canfail`), and a counted run lets exactly a module's own allocations
succeed so the instance's stubs fail with a `LinkError` naming them
(`wasm-canfail-group`). Every Wasm leg requires its exact reading AND exit
status 0, through one expect procedure. The modules are hand-encoded (no
wat2wasm on the host) and each leg was run on the macOS jsc first; that jsc
with its fault handler off dies of SIGSEGV on the null `ref.cast` under BBQ
and OMG, the upstream defect.

**Alternatives.** The null fix could have patched the two `ref.cast` paths
(`WasmBBQJIT64.cpp`, `B3LowerMacros.cpp`) to always emit their check; the one
answer of -1 fixes every caller of `maxAcceptableOffsetForNullReference`, four
today, none edited, and any later one. Upstream's exit-stub generators ask
MustSucceed and then test `didFailToAllocate()`, which a MustSucceed allocation
never reports, so that test was dead; CanFail makes it live rather than
deleting it. The pool stays at JavaScriptCore's near-jump range, where its
no-islands `static_assert` holds at equality, and no larger.

**Verification.** Self-audit S1: under R-sync, B-2e1's `wasm-sync` and
`wasm-async` passed although their jsc died of `snare:abort` after printing
the reading (neither checked the exit status); every Wasm leg now requires its
exact reading AND exit status 0 through one expect procedure, checked on a pty
against the macOS jsc (pass, a wrong reading, a death after the reading,
deny). Audit Fable 5.1 round 1: 0/0/0/3, clean (`10a0c3f8c`): F1 `wasm-patch`
pinned an order a slow BBQ install could invert, so `a(x)` now reaches BBQ
before `c` ever runs; F2 the texts named the builtin trampoline as the last
MustSucceed allocation, and now name the `Wasm::Thunks` and the JSToWasm
shared thunk too (upstream's design for the pool's 25% reserve); F3 the
instance route (a `LinkError`) had no witness, now `wasm-canfail-group`.
Device run 1 on the WIP `1dbd72da1`: ls-jsc 58/58 twice; REDs, each red at its
leg (the walk restored -> the build's refusal and `wasm-tier` snare:abort; a
null offset of 0 -> `refcast-bbq` snare:segv at 0x2; MustSucceed ->
`wasm-canfail` COMPILED-COMPILED; the force-off back -> `tiers`
JSCO-1-1-0-0-1-1; no caller repatch -> `wasm-patch` 10-1-10). On the close
`10a0c3f8c`: ls-jsc 59/59 (`wasm-canfail-group` counted 4 allocations); the
walk restored -> `wasm-sync` snare:abort (S1 seen both ways); no repatch ->
10-1-10 on the new shape; only `WasmBinding` MustSucceed ->
`wasm-canfail-group` INSTANCE, its count down to 2. The first attempt at those
REDs stopped at build.sh's disk refusal after a transient 5.8 GB dip whose
author is still unconfirmed (OPEN-BUGS). Land gates on `10a0c3f8c`: the CI
image's ls-jsc 59/59 twice (a fresh jsc at fork `7cf380d5ea`: `mov w8, #0x80;
svc #0; cbnz x0`, pool constant 0x8000000); suite 1993/1993; test-fault 8/8;
ci-smp-gate N=10 50/50 (default smp1/4/8, UBSan smp4/8), 0 corruption, lowest
free disk 10192 MiB; check-production --all PASS. Timings (HVF): a
1e8-iteration Wasm loop takes 123 ms OSR'd into OMG, 260 ms in BBQ and 875 ms
interpreted; SunSpider 1.0.2 90 ms with every JIT on.
