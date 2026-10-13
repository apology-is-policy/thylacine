---
id: chg-2026-10-13-jsc-ftl
type: chg
title: "JavaScriptCore's FTL runs on the sealed code region; every Wasm instance takes an ISB before its exit stubs; worker threads get 1 MiB stacks"
date: 2026-10-13
arc: arc-boosty
commits: ["a34e48ca1", "de1a1e5e9", "b9b1ef007", "a59710f64"]
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
B-2e1 of the browser arc: the FTL, JavaScriptCore's top tier, on the code
region B-2c opened, and the first half of the Wasm cross-CPU publish
([[dec-2026-10-13-wasm-plan-isb-writers]], by research; the address-space sync
for BBQ and OMG is [[dec-2026-10-13-wasm-space-sync]], B-2e2).

**What** ([[sub-webkit]]). Patch 0005: `Options.cpp` lets the FTL run (BBQ and
OMG stay forced off). Every rewrite of installed FTL code falls under B-2d's
three rules; its one new kind, property caches that are code
(`useHandlerICInFTL` is off upstream), rewrites more than one instruction only
on the JS thread, outside the region it rewrites. `JSWebAssemblyInstance::
finalizeCreation` and `Wasm::Module::compileSync` run
`WTF::crossModifyingCodeFence()` before a thread runs exit stubs another CPU
wrote: the wasm-to-JS stubs come from the validation plan's worklist thread, a
group's wasm-to-wasm stubs from the thread that completes it, and a wait that
finds a plan done takes no context-synchronization event (live since B-2c with
IPInt; upstream has the same gap). `WTF::stackSize()` gives every thread WTF
creates 1 MiB on Thylacine, as on musl Linux; they had musl's 128 KiB (jsc's
`PT_GNU_STACK` is empty), the size of JSC's own soft reserved zone.

**The build asks the binary** ([[sub-substrate-build]]). `build_jsc`'s ISB check
is a list: `unparkCurrentThread`, the two plan consumers and
`FTL::JITFinalizer::finalize`, and `finalizeCreation`'s `isb` must come before
its first call into `WebAssemblyModuleRecord` (address order, failing closed).

**Witnesses** (`ls-jsc`, 45 legs): ftl with ftl-off / ftl-deny / ftl-sync
(`$vm.ftlTrue()`, false in DFG code); ftl-osr (two exits, one exit-stub
compile: the patched jump); ftl-ic (functional: a rewrite that does nothing
passes it); wasm-sync / wasm-async (a JS import called 1001 times through JIT
exit stubs); stack (a `$.agent` recursion deeper than any 128 KiB stack holds);
tiers `JSCO-1-1-0-0-1-1`, failing fast; loop-dfg and sunspider-dfg.

**Verification.** Audit Fable 5.1 r1 0/0/0/3, clean
([[chg-2026-10-13-b2e1-audit-close]]: the stub writers' map corrected, the
fence's position checked, the binding generators' must-succeed allocation
queued into B-2e3). On device (HVF, CI image, b2c worktree): ls-jsc 44/44 once
and 45/45 four times. REDs: the `compileSync` fence removed -> `build_jsc`
refused; the FTL forced off -> red at tiers; the FTL exit's `repatchJump`
removed -> ftl-osr read two compiles; the stack arm removed -> the agent
reached 4 calls against 9562; the property-cache rewrites removed -> ls-jsc
passed (ftl-ic is functional, recorded). SunSpider 1.0.2 95 / 91 / 97 ms all
JITs, 68 / 82 / 69 FTL off, 107 / 111 / 106 DFG off, 342 / 367 / 347 denied: the
FTL is slower on runs this short. Land gates on 823415ae1: ls-jsc 45/45 x2;
suite 1991/1991; test-fault 8/8; ci-smp-gate N=10 50/50 (default smp1/4/8,
UBSan smp4/8), 0 corruption (the first attempt was stopped by the disk
watchdog during the gate's own pool re-bake; re-run with the watchdog at 3072
MiB, lowest free 5980); check-production --all PASS.
