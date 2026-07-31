---
id: seam-56-netd-cancelled-tag
type: seam
title: "netd drops a cancelled parked read's tag without replying"
status: open
surface: [sub-kernel-ninep-client]
opened-by: fnd-5253-r1-f1
tracker: "task #56"
created: 2026-07-31
updated: 2026-07-31
---
## Owed

When a parked deferred-reply op (a netd readiness/data read) is cancelled,
netd discards the pending without emitting any reply — the kernel client's
tag for it is reclaimed only by session teardown (one tag per rare event on
the netd client). netd should `Rlerror` cancelled pendings so the tag
retires promptly.

## What closes it

A netd server-side change: every cancellation path of a deferred reply
(teardown / fid_clunk / Tversion / Tflush arms) emits a terminal `Rlerror`
for the held tag. (Userspace-only; the kernel side is already correct — the
`abandoned` discipline frees on any late reply.)

## Risk while open

Slow tag accumulation toward the 64-slot wedge on the netd-backed client
under an abandon-heavy readiness workload; rare in practice (the event
requires a cancelled parked read), and scoped to the `/net` session, not the
FS mounts.
