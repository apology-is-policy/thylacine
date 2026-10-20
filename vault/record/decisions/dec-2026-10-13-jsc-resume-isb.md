---
id: dec-2026-10-13-jsc-resume-isb
type: dec
title: "B-2d: a thread that resumes running JavaScriptCore's code executes an ISB first (MachineThreads::unparkCurrentThread); no kernel broadcast"
date: 2026-10-13
status: standing
decided-by: research-collapsed
affects: [sub-webkit]
created: 2026-10-13
---
## Fork

The DFG rewrites instructions that run (direct-call link and unlink, jump
replacement at a discard, OSR-exit jumps, arithmetic IC inline code), and some
rewrites are made by a thread that does not run the code next: the collector,
which discards and unlinks code in its end phase, and a JSLock's previous holder
on another CPU. `SYS_ICACHE_SYNC` invalidates by broadcast, but its trailing ISB
synchronizes only the calling CPU; its cross-CPU contract (syscall.h) requires
any other CPU to take a context-synchronization event before running the code.
Where does that event come from for B-2d?

## Research

- **Who writes, verified in the fork:** the JS thread (its own `svc` returns
  through `eret`, a context-synchronization event); `JITWorklist` threads (new
  code only; the JS thread installs it after `crossModifyingCodeFence`,
  `DFGJITFinalizer.cpp:60`, `BaselineJITPlan.cpp:118`, `JITThunks.cpp:161-171`);
  the collector in `runEndPhase` (`finalizeUnconditionalFinalizers`,
  `deleteUnmarkedCompiledCode`), a world-stopped phase.
- **When the collector's thread is the writer:** `Heap::stopTheMutator`
  (Heap.cpp ~2003) stops the world only when no thread holds heap access; an
  access holder is handed the collection and runs it on its own CPU. So a
  collector-thread patch always lands while the JS thread is parked, and it
  resumes through `acquireAccess` (a compare-and-swap; on Thylacine no syscall
  is guaranteed on that path).
- **Every resume passes one function:** gcpark (patch 0003) made
  `MachineThreads::unparkCurrentThread` run on both `acquireAccess` arms and on
  a JSLock entered on access another thread holds -- the same set of points.
- **Heritage and SOTA:** Plan 9 has no JIT. HotSpot runs `cross_modify_fence`
  (an ISB on AArch64) in each thread as it leaves a safepoint -- the same shape.
  Linux offers `membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE)`, which
  interrupts the process's other running threads so each takes a
  core-serializing event. Darwin JSC uses `crossModifyingCodeFence` at its
  install points and thread suspension elsewhere.

## Options

- **A. An ISB in `unparkCurrentThread`** (userspace, no kernel change): one
  instruction per resume; covers the collector's patches and a JSLock handed
  across CPUs; honors the existing contract as written.
- **B. A `SYS_ICACHE_SYNC` mode that broadcasts the event** (membarrier
  SYNC_CORE): a new kernel path with IPIs and its own audit; the collector would
  call it once per end phase; it also serves writers with no resume point.
- **C. Both.**

## The call

A. The research collapsed the fork: every collector-side patch precedes a resume
through the function gcpark already put on every resume path, so the fence
there leaves no case open for B-2d, and B adds a kernel surface for none. B
stays a B-2e question: Wasm's `resetInstructionCacheOnAllThreads` signals every
VM thread and has no resume point to hang a fence on.

## Rationale

The fence is placed where the thread learns the world resumed, after the
compare-and-swap that observes the collector's release and before any JIT code
runs (verified by Fable r1: the first instruction after the prologue). It has no
run-time witness -- a missing ISB shows only as a rare stale fetch -- so
`build_jsc` asks the built binary for it and refuses a jsc without one.
