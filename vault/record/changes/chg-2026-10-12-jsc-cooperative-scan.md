---
id: chg-2026-10-12-jsc-cooperative-scan
type: chg
title: "JavaScriptCore's conservative scan reads the stack image a thread parks when it releases heap access; no thread is suspended"
date: 2026-10-12
arc: arc-boosty
commits: ["bb70e7f18", "7dd3da86b", "70afffb67", "e7f5c085b"]
touched:
  - sub-webkit
  - sub-substrate-build
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-10-12
---
Operator votes 2026-10-12: the cooperative scan, no kernel change
([[dec-2026-10-12-jsc-cooperative-scan]]); land B-2c first, then this, before
B-2d.

**What** ([[sub-webkit]]). Researching B-2d found that the collector's
conservative scan calls `Thread::suspend` on every other thread in the heap's
group, and when a stop-the-world finds the mutator without heap access the
collector thread conducts and suspends it. B-0 had made `Thread::suspend` a
release assert, because Pouch has no per-thread signal, so jsc exited 127 with
no message at the first such collection. Patch 0003 parks instead: on
`OS(THYLACINE)` every release of heap access (`Heap::releaseAccess` before its
fast CAS, `releaseAccessSlow` before its clearing CAS, and `JSLock::willReleaseLock`
for a thread that entered the lock on access another thread held) calls
`MachineThreads::parkCurrentThread`, which captures the callee-saved registers
and copies the stack from that point to the thread's origin into a per-heap,
per-thread image; every acquire unparks. `gatherFromParkedThreads` replaces
the suspend loop: each group thread other than the scanner and the conductor
must be parked, its image goes to `ConservativeRoots`, and an unparked one is
fatal with a `FATAL: the conservative scan found thread N ...` line.

**Why a copy.** Go records a goroutine's stack pointer at a syscall; here the
frames between the releasing caller and the park point may hold the only copy
of a caller's callee-saved register, and they are reused once the release
returns. The image is complete because a thread without heap access cannot
obtain a cell it did not hold at the release, and cannot reacquire access while
the world is stopped.

**The series check** ([[sub-substrate-build]]). `webkit_series_tree` now
compares the exact tree the pin plus the series builds (forage absolutizes its
roots, names an unreadable patch, keeps git's reason, and refuses a patch that
touches a path outside the sparse cone before any `am`).

**Verification.** Audit Fable 5.1 r1 0/0/1/5 (F1 the access-holder's unlock
did not park; F2 closed at this land by the 0003 audit-trigger row; F3, F4, F6
fixed; F5 fixed after a diagnostic build). On device: green x3 (one first-run
FAIL was ls-jsc's status pattern, which did not allow ut's space in
`JSCQ-ST- 0`); REDs on the gc legs: R0 (no 0003) and R1 (images never marked
parked) die 127 on flash, flash-dft and idle (R1 with the FATAL line), R2 (the
scan ignores the images) loses roots on flash only (40-37). The held leg is
not a control: jsc drops and retakes the JSLock around the script, and the
continuous collector can scan main there (a diagnostic build showed the scan
and main's last park in `JSLock::unlock`). Land gates: On the tip 0b4276ce0: ls-jsc PASS on the CI image, all 25 legs (jsc relinked against the new Pouch, its `abort()` disassembles to `brk #0xab06`; SunSpider 1.0.2 111 ms with the JIT vs 363 ms denied, the int32 loop 450 vs 962 ms); pty-4 PASS (`$status` 130 after `fg` + Ctrl-C); suite 1991/1991 on the default image (deaths legs abort and assert -> SIGABRT, `brk #1` -> SIGTRAP, a NULL store -> SIGSEGV; three `snare:abort` console lines; the dlopen RELRO legs read SIGSEGV); test-fault 8/8; ci-smp-gate N=10 50/50 (default smp1/4/8, UBSan smp4/8), 0 corruption; check-production --all PASS (the lean image builds, boots, and both dev accounts log in)
