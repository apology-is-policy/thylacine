---
id: arc-tickless-idle
type: arc
title: "Tickless idle (NO_HZ_IDLE), and the property it silently removed"
status: active
design: ["docs/TICKLESS-IDLE.md", "docs/ARCHITECTURE.md 8.6"]
chunks:
  - chg-2026-06-21-ti-tickless-idle
  - chg-2026-06-22-ti4-work-conservation
follow-ons: [seam-tickless-bare-metal]
created: 2026-08-01
---
## Goal

Stop the 1 kHz tick on a genuinely idle CPU. Measured motivation: under
HVF the never-stopped tick burned **~332% idle CPU** — a per-tick VTIMER
exit plus emulated-GIC MMIO vmexits plus a WFI that never actually parks.

## Shape

- **TI-1/2/3** ([[chg-2026-06-21-ti-tickless-idle]]) — the one-shot
  primitive, the nearest-deadline scan, and the idle park that arms
  `min(deadline, now + backstop)` instead of holding the periodic tick.
  Model-first: [[spec-sched-tickless]], because the arm-race needs
  `EnterWFI` split into a register step and a park step.
- **TI-4** ([[chg-2026-06-22-ti4-work-conservation]]) — push placement,
  the busy-tick overload kick ([[spec-sched-rebalance]]), the
  work-conservation telemetry, the affinity seam, and the 4 ms backstop.

## What made this arc interesting

**The regression was not in the code that changed.**

The 1 kHz tick had silently *been* the work-stealing re-poll: an idle CPU
re-ran `try_steal` every tick, so any work-arrival the best-effort single
kick missed got pulled within a millisecond anyway. Nobody had written
that down, because nobody had needed to — it was a free consequence of a
timer nobody planned to stop. Stopping it stranded queued work on busy
CPUs until the backstop: a **2.4x boot slowdown**.

The fix is the Linux NO_HZ_IDLE shape (cross-checked against FreeBSD ULE
and Zircon): tickless stops only the *idle* tick, so a busy CPU is still
ticking and can drive rebalancing.

## The second finding

The residual slowdown after TI-4 was **not a guest bug at all**. The
telemetry measured 99.85% of tickless parks IPI-woken — the wake path is
push-complete — but resuming a deep-parked vCPU via SGI costs ~0.85 ms
under HVF against ~7 µs hot. An emulation artifact.

So the 4 ms backstop is honestly an **HVF dev-loop warmth knob**, not a
scheduler fix, and the arc's own conclusion is that the design is correct
for the bare-metal target where an SGI to a WFI'd core is nanoseconds.
That claim is owed a measurement: [[seam-tickless-bare-metal]].

## The tail

#363 ([[chg-2026-07-05-33-sys-yield]]) belongs to this arc's consequences
even though it landed under another: the multi-millisecond starved-park
records TI-4d produced were read as peer backlog, and were actually a CPU
parking over its own just-requeued thread. The counter was right; the
interpretation was not, and the fix's witness was that same counter
collapsing by 65%.
