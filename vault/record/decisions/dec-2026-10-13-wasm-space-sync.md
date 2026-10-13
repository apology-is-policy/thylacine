---
id: dec-2026-10-13-wasm-space-sync
type: dec
title: "B-2e: SYS_ICACHE_SYNC gains a mode that context-synchronizes every CPU running a thread of the caller's address space; it replaces JavaScriptCore's suspend-every-thread walk for Wasm"
date: 2026-10-13
status: standing
decided-by: user-vote
affects: [sub-webkit, sub-kernel-syscall-dispatch]
created: 2026-10-13
---
## Fork

When a Wasm compiler thread installs a BBQ or OMG callee, every thread that may
run it must take a context-synchronization event after the new code is flushed
and before it is published. Upstream JavaScriptCore walks a process-wide set of
every thread that ever entered a VM (`Wasm::resetInstructionCacheOnAllThreads`,
`WasmMachineThreads.cpp:54-64`) and suspends and resumes each one; the signal
handler's return is the event (`CalleeGroup.cpp:419-422` says so). Thread
suspension is fatal on Thylacine (B-0), so BBQ and OMG crash at their first
install as the tree stands. What replaces the walk?

## Research

- **What the walk buys, verified in the fork:** not a stopped world -- every
  patch happens after the threads resume. Its three sites: a callee install
  (`CalleeGroup.cpp:331`), the callers' retarget (`:423`) and an OSR-entry
  install (`WasmOSREntryPlan.cpp:170`). The code rewrites are one-instruction
  `bl` retargets in callers; the rest is data read through a register.
- **A cooperative ISB is incomplete:** BBQ and OMG code polls no VM trap
  (`traps()`/`needHandling` occur nowhere in `WasmBBQJIT*.cpp` or
  `WasmOMGIRGenerator.cpp`), an OMG loop has no checkpoint, and new code is
  reachable from Wasm through the retargeted `bl` and the indirect entry points
  without passing through C++.
- **Heritage:** Plan 9's `procflushseg` (`port/proc.c`) marks each processor
  whose current process maps the segment and waits until each has taken its
  next clock interrupt.
- **SOTA:** Linux `membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE)`
  (4.16+): on return, every running thread of the caller's process has executed
  a core-serializing instruction; non-running threads need nothing.
- **The tree:** `gic_synchronize_cpu` (`arch/arm64/gic.c:230`) already sends an
  IPI and waits for the target to acknowledge it.

## Options

- **A. Synchronize this address space's CPUs** (kernel): IPI every other CPU
  currently running a thread of the caller's address space; return once each
  has taken the interrupt. Disturbs only the caller's own threads.
- **B. Synchronize every CPU** (kernel): the same, to all online CPUs (Linux's
  `kick_all_cpus_sync` for kernel text); a simpler ordering argument, but any
  holder of a code region could interrupt every CPU at the syscall's rate.
- **C. Per-thread signals in Pouch**, so upstream's walk runs as written;
  brings back the thread suspension B-0 removed.

## The call

A (operator vote, 2026-10-13). `Wasm::resetInstructionCacheOnAllThreads` calls
the new mode under OS(THYLACINE). The mechanism and its ordering argument are
designed in `docs/JIT-ON-WX-DESIGN.md` at B-2e2, before its code.

## Rationale

It is the requirement exactly: a running thread takes the event at the IPI's
exception entry and return; a thread not running takes one at its next `eret`.
It also covers a thread that enters Wasm after the walk would have read the set.
