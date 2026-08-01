---
id: chg-2026-06-21-ti-tickless-idle
type: chg
title: "TI-1/2/3: NO_HZ_IDLE — stop the tick on a genuinely idle CPU"
date: 2026-06-21
arc: arc-tickless-idle
commits: ["f1277c27", "6e67fa60", "f2380e19"]
touched: [sub-kernel-sched-smp]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
## What

An idle CPU stops taking the 1 kHz periodic tick and arms a **one-shot**
to `min(nearest pending deadline, now + backstop)` instead. The shared
park body (`sched_idle_park`) serves both cpu0's idle loop and each
secondary's.

TI-1 landed the timer one-shot primitive and the nearest-deadline scan
with no behavior change; TI-2 wired the loop; TI-3 closed the arc with
the SMP gate, an HVF re-measure, and a focused audit.

## Why

Measured under HVF: the never-stopped tick burned **~332% idle CPU** —
a per-tick VTIMER exit plus emulated-GIC MMIO vmexits plus a WFI that
never actually parks, at a 1 ms period. TCG was 21.5%; dropping to
250 Hz gave 5.2%.

## The two ordering rules

- **Announce before arming.** `idle_in_wfi` is set *before* the one-shot
  arm and the WFI, so a peer placing work either sees the announcement
  and kicks, or sends an IPI the WFI takes pending. Register-then-observe;
  [[spec-sched-tickless]] is the model, written model-first, with
  `BUGGY_PARK` as the park-before-register counterexample.
- **Restore before running.** On wake, `timer_arm_this_cpu()` re-arms the
  periodic tick *and* `timerwait_tick()` is called explicitly — because
  the re-arm **deasserts** the one-shot's pending IRQ, so a one-shot that
  fired on a passed deadline would otherwise never have its scan run: a
  busy-spin of re-arm, fire, deassert, repeat.

## What it broke

The 1 kHz tick had silently *been* the work-stealing re-poll. Removing it
stranded queued work on busy CPUs until the backstop — a 2.4x boot
slowdown, addressed at [[chg-2026-06-22-ti4-work-conservation]]. Worth
recording as a shape: the regression was not in the code that changed, it
was in a property nobody had written down that the changed code had been
providing for free.
