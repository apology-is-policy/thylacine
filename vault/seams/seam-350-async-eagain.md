---
id: seam-350-async-eagain
type: seam
title: "submit_async treats send EAGAIN as fatal (the async #349)"
status: open
surface: [sub-kernel-ninep-client]
opened-by: chg-2026-06-24-349-flow-control
tracker: "task #350"
created: 2026-07-31
updated: 2026-07-31
---
## Owed

`p9_client_submit_async` (the Loom path) treats `P9_TRANSPORT_EAGAIN` as a
broken stream and latches the shared session dead — the same
congestion-is-not-death class #349 fixed on the sync path and #53 fixed on
the flush paths. Pre-existing (pre-#349 a full ring also returned −1 →
death there), so #349 was inert on it, but the class survives on this one
path. No `out_buf` clobber exposure (build+send never drop the lock).

## What closes it

A focused design round: the clean fix interacts with the async
`on_complete` double-fire (a self-pump's mark_dead fires the registered
async rpc), so it is not a mechanical EAGAIN-retry — the completion seam's
single-fire contract must be preserved across a park/retry.

## Risk while open

A congested async submit (Loom over a bulk-class conn under ring pressure)
kills the shared session for every Proc on the mount. Bounded in practice:
the v1.0 async drivers are kproc pumps on lightly-loaded rings, and EAGAIN
requires a transiently-full c2s.
