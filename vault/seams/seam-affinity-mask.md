---
id: seam-affinity-mask
type: seam
title: "thread_may_run_on is a plugged, always-true affinity predicate"
status: open
surface: [sub-kernel-sched-smp]
opened-by: chg-2026-06-22-ti4-work-conservation
tracker: "a future SYS_SCHED_SETATTR"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

A per-thread affinity mask, and with it a real body for
`thread_may_run_on(t, cpu)` — today `return true;`.

## Why it was built this way

The seam was landed deliberately, inert, at the point the
work-conservation redesign touched both CPU-binding decisions. It is
consulted at exactly two sites — placement (`select_target_cpu`) and the
steal victim pick (`try_steal`) — so a future mask plugs into **one
function** rather than being retrofitted into a rebalancer that assumed
it could put any thread anywhere.

Being always-true makes it provably inert: `select_target_cpu`'s two
guards collapse to their originals, and `try_steal`'s
`cand->cpu_pinned || false` is byte-identical to the prior condition. No
placement or steal behavior changed.

`cpu_pinned` stays a **separate, stronger** predicate — the hard pin for
per-CPU idles and `kthread`, which is a structural property (they run on
a CPU-owned static stack), not a policy one. The two must not be merged.

## Cost of leaving it

None today. The point of recording it is the opposite of most seams: it
is here so that the day a mask lands, nobody re-derives where it goes.
