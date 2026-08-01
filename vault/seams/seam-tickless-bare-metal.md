---
id: seam-tickless-bare-metal
type: seam
title: "The deep-park latency finding is owed a bare-metal confirmation"
status: open
surface: [sub-kernel-sched-smp]
opened-by: chg-2026-06-22-ti4-work-conservation
tracker: "owed at Lazarus / RPi"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

Confirmation, on real hardware, of the finding that shaped the tickless
backstop: that resuming a **deep-parked** vCPU via SGI is an *emulation*
cost, not a scheduler defect.

Measured under HVF: ~0.85 ms to resume a deep-parked vCPU against ~7 µs
when hot, from GICv2-MMIO vmexits plus the host vCPU-thread resume. On
bare metal an SGI to a WFI'd core should be nanoseconds — which is the
claim this seam owes evidence for.

## What rests on it

`TICKLESS_IDLE_BACKSTOP_NS` is 4 ms rather than 100 ms, and the honest
description of that number is **an HVF dev-loop warmth knob, not a
scheduler fix**. It keeps dev vCPUs warm (HVF boot ~7 s instead of
~17–35 s) at the cost of ~5% HVF idle instead of ~0.3%.

If the bare-metal measurement confirms the finding, the 4 ms re-poll is
pure emulation tax and an adaptive or accel-gated backstop
(warm-while-active, deep-when-idle) reclaims the idle without the
dev-boot cost. If it does *not* — if deep-park resume is slow on real
hardware too — then the wake path needs re-examining rather than the
backstop retuning, and the whole "the design is correct for the
production target" conclusion has to be revisited.

## Why the finding is trusted meanwhile

Because it was measured rather than asserted: the wake-source telemetry
split (`tickless_ipi_wakes` vs `tickless_oneshot_wakes`) showed **99.85%
of tickless parks are IPI-woken**, which proves the wake path is
push-complete. A stranding bug would show up as backstop wakes. What was
left was latency, not correctness — and the kick still earns its place
independently, as the fast parallel-surplus catch (cpubench starvation
−34% to −60% across parallel modes).
