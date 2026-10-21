---
id: chg-2026-10-07-tag-pool
type: chg
title: "The 9P tag table grows to the 16-bit tag space, each kind of op has a share, a sync op waits for a tag, and close(2) reports a failed write-behind flush"
date: 2026-10-07
arc: arc-boosty
commits: ["82c478c22", "a92b3d37b", "bff734a37", "529f427f5", "a22793699", "d5326904b"]
touched:
  - sub-kernel-ninep-session
  - sub-kernel-ninep-client
  - sub-kernel-ninep-dev9p
  - sub-kernel-loom
  - sub-kernel-dev
  - sub-kernel-spoor
  - sub-kernel-handle
  - sub-kernel-syscall-dispatch
  - spec-tag-pool
established:
  - spec-tag-pool
closed:
  - seam-9p-tag-block-on-full
  - seam-wb-close-flush-slot
opened:
  - seam-9p-async-share-poll-reserve
mirrors-checked: []
depth: rich
created: 2026-10-07
---
Every 9P session held 64 tags, and a sync op that found all 64 held failed
`-P9_E_IO` at its build -- against ARCH 21.5's "new requests block until a
slot frees" ([[seam-9p-tag-block-on-full]]). Nothing above the client retries,
so a write-behind flush that met a full pool dropped its data, and its close
said nothing: `Dev.close` was `void` ([[seam-wb-close-flush-slot]]). The
holders that fill a pool are not all bounded by the server's progress: Loom
ops and dev9p poll arms on a deferring server, abandoned ops whose Tflush
found no tag, and a stopped thread's stored reply.

By the operator's votes ([[dec-2026-10-07-tag-pool]], all four parts as one;
[[dec-2026-10-07-close-eio]]), ARCH 21.11:

- **The table grows** ([[sub-kernel-ninep-session]]): tags 0..0xFFFE in
  64-entry chunks, the first inline in the session and the rest kmalloc'd
  under the client's spinlock and kept until destroy; a failed allocation is
  "no free tag". Heritage: 9front's devmnt and Linux's 9p client both bound a
  session by the 16-bit tag space, not a fixed pool.
- **Shares**: an op takes a tag only within `P9_OPS_MAX` (32767), and a
  Tflush takes any free tag. A victim stays active until its Rflush, so
  flushes never outnumber ops and a Tflush always finds a tag. Loom ops and
  dev9p poll arms and snapshots share `P9_ASYNC_MAX` (16384) within the op
  share and get `-P9_E_AGAIN` past it, so no deferring server can hold every
  sync op's tag.
- **A sync op waits** ([[sub-kernel-ninep-client]]): every sync wrapper
  drains replies until a tag is free before its build, killably, instead of
  failing EIO.
- **The reader applies every sync reply** into the waiter's result when it
  reads it, so a stopped thread holds no tag; the client's `inflight[]` moved
  into the session entry's `owner`.
- **close(2) reports the flush** ([[sub-kernel-dev]], [[sub-kernel-spoor]],
  [[sub-kernel-handle]], [[sub-kernel-ninep-dev9p]]): `Dev.close` returns
  `int`; dev9p's last close returns its flush's failure or the latched one,
  and `SYS_CLOSE` maps it to `EIO` with the fd closed.

[[spec-tag-pool]] proves a waiting sync op gets a tag with async ops deferred
forever and a waiter stopped forever, and that a Tflush always fits; each
buggy cfg (no async share, the waiter applying replies, no flush headroom)
violates its property.

**The model.** `specs/tag_pool.tla` (`specs/check-tag-pool.sh`): the clean
cfg holds `SyncProgress` and `FlushAlwaysFits` over 268 distinct states;
`tag_pool_buggy_no_async_cap` (304) and `tag_pool_buggy_waiter_applies` (360)
violate `SyncProgress`, `tag_pool_buggy_no_flush_headroom` (127) violates
`FlushAlwaysFits`, and each counterexample was read for its intended shape.
`9p_client.tla` re-run: clean 197 distinct, its five buggy cfgs violate.

**The witnesses.** Each sabotage turned exactly the predicted tests red, each
at its own assertion, and the tree was restored and rebuilt after every run:
RED-1 (no flush headroom, no async share, a sync op failing EIO instead of
waiting, the reader skipping a stopped owner's reply) seven tests; RED-2 (the
table cannot grow) four; RED-3 (dev9p's latched arm, its flush-now arm,
`sys_close`'s mapping) three, one each; RED-4 (the reader handoff's per-chunk
sync-owner count, its increment and its decrement, run separately) one each.
`9p_client.full_pool_sync_op_gets_a_tag` was written before the fix and ran
red on the seam90 tree. Suite 1914/1914 (TP-3), 1916/1916 (TP-4), 1917/1917
(the audit close, d5326904b).

**The audit.** Round 1 (Fable 5.1, read-and-reason): 0 P0 / 0 P1 / 0 P2 /
8 P3, plus three P3s from a parallel self-audit. Fixed: the reader handoff
scanned every in-flight tag under the client lock (now a per-chunk count of
sync owners lets it skip chunks without one), the client header's errno
contract, a fail-soft registration (now a hard stop), an ARCH overclaim about
a failed chunk allocation, the "as Linux NFS does" parity claim (Linux reports
at every close through `->flush`; this reports at the open file's last
close), a long comment line and stale `inflight[]` prose. Recorded: no poll
reservation inside the async share ([[seam-9p-async-share-poll-reserve]]),
tag-wait fairness and the table that never shrinks (in
[[sub-kernel-ninep-session]]). Tracked in OPEN-BUGS: the three remaining
O(in-flight) walks under the client lock and Loom's widened flood budget. A
clean close (no P0, P1 + P2 = 0, no invasive fix), so no second round.
Found adjacent and owned by the exit-close chunk: a kill inside a flushing
syscall drops write-behind bytes the writer was already told were written.

**The gates.** `ci-smp-gate` N=10 on d5326904b (08:29Z-09:33Z): 50/50 PASS over default-smp1, default-smp4, default-smp8, ubsan-smp4 and ubsan-smp8, no corruption. `ls-ci` on a `--config ci` bake of the same tip in a worktree (baked by 09:40Z): PASS in 55 s. Suite 1917/1917 at 08:20Z, and again at 08:25Z after RED-4.
