---
id: seam-wb-close-flush-slot
type: seam
title: "Write-behind close-flush is best-effort (void Dev.close)"
status: closed
surface: [sub-kernel-ninep-dev9p]
opened-by: chg-2026-07-11-wb-staging
tracker: ""
created: 2026-07-31
updated: 2026-10-07
closed-by: chg-2026-10-07-tag-pool
---
**Owed**: an error-carrying (and bounded/abortable) close-flush. At v1.0
`Dev.close` is void, so the final flush of a staged run at `dev9p_close`
latches-and-drops on failure — the voted NFS-async posture: the bytes are
lost silently at close (bounded < 256 KiB by the cap flushes), and
**fsync is the reliable error channel** (a consumer that cares calls it).

**What closes it**: growing the `Dev.close` slot to return/deliver the
latched errno (a vtable ABI change across every Dev), plus a bound on the
close-flush against a wedged server.

**Risk while open**: a crash-adjacent write-then-close-without-fsync on a
loose mount can lose its tail silently — the documented loose-mount
contract.

## As of 2026-10-07

- The error half is closed by [[chg-2026-10-07-tag-pool]]
  ([[dec-2026-10-07-close-eio]]): `Dev.close` returns `int`, dev9p's last
  close returns the flush's failure or the one latched earlier, and
  `close(2)` reports it as `EIO` with the fd closed. A sync flush no longer
  meets a full tag pool either: it waits for a tag.
- The bound half (a close-flush a wedged or hostile server can hold) is
  [[seam-close-flush-unbounded]], still open: the operator voted that a kill
  during the final close hands the rest of it to the closer kthread
  ([[dec-2026-10-07-exit-close]]), not a deadline.
