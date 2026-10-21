---
id: chg-2026-10-13-jsc-dfg
type: chg
title: "JavaScriptCore's DFG runs on the sealed code region; a thread resumes the VM's code behind an ISB"
date: 2026-10-13
arc: arc-boosty
commits: ["1685982a2", "211104306", "05c9393a0", "e693ca4d2"]
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
B-2d of the browser arc: the DFG, the optimizing tier, on the code region B-2c
opened ([[dec-2026-10-13-jsc-resume-isb]], decided by research).

**What** ([[sub-webkit]]). Patch 0004: `Options.cpp` lets the DFG run (FTL,
BBQ and OMG stay forced off); `MachineThreads::unparkCurrentThread` executes
`WTF::crossModifyingCodeFence()` (an ISB) first, because the DFG rewrites code
that runs and the collector (world stopped, the JS thread parked) or a JSLock's
previous holder on another CPU may be the one that rewrote it, while
`SYS_ICACHE_SYNC` synchronizes only the calling CPU and heap access is retaken
with no syscall; `ARM64Assembler.h`'s three 26-bit branch encoders
`RELEASE_ASSERT` the range with jump islands off instead of masking it.
`syscall.h`'s cross-CPU contract text names its consumer.

**The build asks the binary** ([[sub-substrate-build]]). The ISB has no
run-time witness, so `build_jsc` disassembles `unparkCurrentThread` in the
symbol-bearing jsc and refuses a build without an `isb` (after a denominator of
eight instructions).

**Witnesses** (`ls-jsc`): dfg with dfg-off / dfg-deny / dfg-sync; tiers
`JSCO-1-0-0-0-1-1`; osr (two exits through one ramp; waits for the worker's
compile); watch (a watchpoint fired from inside a running DFG loop: the hole
reads 42, and the loop was DFG code when it fired); gc-jettison and its twin (a
DFG block holding a dropped closure as a weak reference is discarded by the
collector while jsc is parked: at least one of twenty rounds, none with the
closures kept); loop-baseline and sunspider-baseline record the DFG-off times.

**Verification.** Audit Fable 5.1 r1 0/1/1/2, clean (F1: the first collector-discard leg counted "due to JettisonDueToOldAge" lines, a reason JSC gives only LLInt and Baseline blocks, so it passed with the DFG off -- rebuilt around a dropped closure the DFG code holds weakly; F2 a finished Baseline function IS rewritten, by its arithmetic ICs; F3 the osr leg waits for the worker's compile; F4 a misread upstream comment). On device (HVF, CI image, b2c worktree): ls-jsc 34/34 five times; gc-jettison 9 and 18 discarded vs 0 in its twin; with the DFG off every round reads "never DFG". SunSpider 1.0.2 68 ms all JITs, 105 ms DFG off, 346 ms denied; int32 loop 193 / 433 / 917 ms. REDs: the ISB removed -> build_jsc refused; the DFG forced off -> red at dfg; jump replacement disabled -> red at watch (the stale loop read a Symbol; the leg now fails on the exception, fast). Land gates on e693ca4d2: suite 1991/1991; test-fault 8/8; ci-smp-gate N=10 50/50 (default smp1/4/8, UBSan smp4/8), 0 corruption (the first attempt was stopped by the disk watchdog during a host free-space dip; re-run whole); check-production --all PASS.
