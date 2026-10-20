---
id: dec-2026-10-12-jsc-cooperative-scan
type: dec
title: "gcpark: JavaScriptCore's conservative scan reads a stack image each thread parks when it releases heap access; no thread is ever suspended"
date: 2026-10-12
status: standing
decided-by: user-vote
affects: [sub-webkit]
created: 2026-10-12
---
## Fork

Researching B-2d found that JavaScriptCore's collector suspends threads in its
conservative scan, and a device experiment confirmed it: when a stop-the-world
phase finds the mutator without heap access, `Heap::stopTheMutator` stops the
world at once, the collector thread conducts, and
`MachineThreads::tryCopyOtherThreadStacks` calls `Thread::suspend` on the one
JS thread. B-0 made `Thread::suspend` a release assert, because Pouch has no
per-thread signal, so jsc died with exit status 127 and no message. Every
WebContent process would hit this between events. The question was how the scan
reads a thread it cannot suspend.

## Research

- **The mechanism, pinned on the device** (2026-10-12, scratch experiment 2: a
  diagnostic write before the suspend). A 25 ms wait that holds heap access
  completed, and the same wait releasing it (`flashHeapAccess`) printed the
  marker and exited 127, under the default and the continuous collector. With
  the concurrent collector off, the run completed once, but that switch changes
  only the scheduler (the collector thread still conducts), so it is not a
  mitigation.
- **Where access is released.** `Heap::releaseAccess` is the only function that
  clears `hasAccessBit` (fast path and `releaseAccessSlow`). Its callers are
  `JSLock::willReleaseLock` (every outermost unlock, including `DropAllLocks`),
  `ReleaseHeapAccessScope` and jsc's `flashHeapAccess`. `Atomics.wait` is not a
  release in this WebKit: `WaiterListManager::waitSyncImpl` blocks holding
  access.
- **Heritage and the state of the art.** Go records a goroutine's stack pointer
  when it enters a syscall and scans from there; HotSpot records the last Java
  frame when a thread enters native code; V8 parks threads at safepoints. All
  three record a position, which works because their compiled code keeps no
  live reference in a callee-saved register across the transition. Kernels
  that suspend a thread for its collector: Fuchsia (`zx_task_suspend` +
  `zx_thread_read_state`), Mach (`thread_suspend` + `thread_get_state`), Linux
  (a signal and its `ucontext`).

## Options

1. **Cooperative scan, no kernel change.** A thread publishes its registers and
   stack position when it releases heap access, and the scan reads that.
2. **A kernel thread suspend and register read**, as on Fuchsia and Mach: a new
   syscall surface, and an audit of every state a thread can be stopped in.
3. **Both, the cooperative scan first.**

## The call

Option 1 (operator, 2026-10-12, AskUserQuestion, "Cooperative scan
(Recommended)"). A kernel thread suspend stays a separate question for the
users that cannot be made cooperative: B-2e's Wasm instruction-cache reset and
a sampling profiler. Ordered after the B-2c land and before B-2d.

## Rationale

The defect sits in a protocol JavaScriptCore already has: a thread without
heap access cannot obtain a cell it did not hold when it released (allocation
and heap reads need access), and it cannot reacquire access while the world is
stopped. So an image taken at the release is a complete root set for that
thread for as long as the world can stop around it, and no kernel mechanism is
needed. The image is a copy of the stack from the release point to its origin
plus the callee-saved registers, not a recorded stack pointer as in Go: in C++
the frames between the caller and the release point may hold the only copy of
a caller's callee-saved register, and they are reused once the release
returns. Images are written only by their own thread while it holds access and
read only while no other thread can. The cost is one copy of the live stack
per release, which at a JSLock drop is the shallow stack of an event loop.
BROWSER-DESIGN section 7, "Corrected 2026-10-12", carries the design.
