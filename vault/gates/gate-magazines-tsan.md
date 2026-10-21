---
id: gate-magazines-tsan
type: gate
title: "The per-CPU magazines under ThreadSanitizer"
proves: "That the actual mm/magazines.c bodies (extracted, not copied by hand) let no peer touch a CPU's magazine set outside that set's lock: three host threads churn their own sets through mag_alloc/mag_free while a fourth runs magazines_drain_all, and the run draws no TSan report, hands no page to two owners and returns every page to the zone exactly once. Its discrimination is shown in the same run: dropping the drain's lock, mag_alloc's or mag_free's, or modelling the pre-lock tree (b057de8eb), must each draw a race report."
blind-to: "ARM weak memory (TSan models the host's C11 semantics, not ldaxr/stlr); guest timing and the real spinlock, IRQ mask and preempt accounting (the guest twin is phys.drain_meets_live_owners); the clustered-SoC fold of two CPUs onto one slot; and any caller outside the extracted functions, since the zone and the CPU id are host doubles."
invocation: "python3 -I tools/test-magazines.py --mutants --logs <dir> (Homebrew LLVM clang; under a mac lease)"
created: 2026-10-21
updated: 2026-10-21
---
## Method

`tools/test-magazines.py` extracts the nine magazine functions from
`mm/magazines.c` and the structs and sizes from `mm/magazines.h` into
`tools/host-tests/magazines.c`, whose shims stand in for the kernel: a
mutex-backed zone that counts a double free, a thread-local CPU id, and
spinlocks where `spin_lock_irqsave(NULL)` is an IRQ mask and so excludes
nothing on a peer CPU. A binary without `__tsan_` symbols is refused, since
an uninstrumented run reports nothing.

The clean leg passes only with no TSan output, exit 0 and a clean
conservation audit. Every other leg (three one-sided lock drops and the
pre-lock source read from git at b057de8eb) passes only when TSan reports a
race.

## What it has and has not caught

Its predecessor, the operator-directed double of 2026-10-07
(work/oct5-as-r9/mag-double), measured 7575 silent double allocations on
the pre-lock bodies with the #807 count guard firing zero times
([[sub-kernel-mm-phys]]). Manifestation of the corruption is schedule
dependent; the race report is not.

Its first run (2026-10-21, at 72298812e) passed the clean leg and the three
lock drops. The pre-lock leg did not build: that body never initialises a
lock, so the harness's inline `spin_lock_init` went unused, which
`-Wunused-function` reports and `-Werror` refuses. The shim is now a macro,
and all five legs' sources pass a syntax-only build.
