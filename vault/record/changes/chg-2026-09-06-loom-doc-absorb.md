---
id: chg-2026-09-06-loom-doc-absorb
type: chg
title: "absorb docs/reference/107-loom (I-29/I-30 Loom ring transport): fold the device-gone terminal into sub-kernel-ninep-client"
date: 2026-09-06
arc: arc-vault
commits: ["PENDING"]
touched: [sub-kernel-ninep-client]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-06
---

# docs/reference/107-loom.md -> ABSORBED (I-29/I-30 audit-trigger surface)

Absorbed the 1345-line Loom reference doc into a multi-redirect stub. The owning
dossier `sub-kernel-loom` (482 lines, guarded-by inv-i29/i30/i32, updated
2026-08-16) is deep and current -- verified atom-by-atom, it covers the whole
mechanism: the private-counter-is-authority ring (I-30 TOCTOU), copy-first-then-
decide, pin-at-submit / never-re-resolve (the io_uring credential-vs-work class),
back-pressure-at-submit, the callback discipline, multishot + terminal-CQE,
LINK/DRAIN ordering, the poll thread + park condition, the borrow guard, the
join, and the I-32 dual charge-ledger (the two owner pointers at opposite ends of
setup; region-not-ring "who paid"; the thread-ledger backstop + the wrong
one-line fix that would have leaked). All present.

ONE genuine residue, code-grounded and folded:

- **The device-gone terminal (I-29 device-gone extension, Menagerie step 4) was
  in no dossier body.** `sub-kernel-loom` had `spec-loom-devgone` only in
  validated-by; `sub-kernel-ninep-client` documented `client_mark_dead_locked` as
  the sole c->dead setter but omitted its reason parameter. Code-grounded
  (kernel/9p_client.c:187): the signature is `client_mark_dead_locked(c, bool
  devgone)`; the three reader sites pass `rr == 0` (recv-0 = clean EOF = a torn-down
  server/driver endpoint -> `-T_E_NODEV`), the error sites pass `false` (recv-error
  / deadline / malformed -> `-T_E_IO`); line 184 confirms the reason rides only the
  async (POST_CQE / Loom) path (the sync #841 surface untouched). Folded into
  sub-kernel-ninep-client's Fail-close section: the recv-0 -> ENODEV mapping,
  p9_client_mark_devgone (idempotent secondary entry), the async-only scope, and
  the exactly-once (demux clears inflight[tag] before completing -> a late reply on
  a death-completed op dispatches ownerless, never a second terminal CQE). Its home
  is ninep-client because the code lives in 9p_client.c, not loom.c; the loom side
  (loom_async_complete passing status through) is unchanged.

Redirects: mechanism -> sub-kernel-loom; device-gone terminal ->
sub-kernel-ninep-client (reason + devgone entry) + sub-kernel-ninep-transport
(recv-0-vs-(-1) contract + srvconn EOF); ring ABI -> abi-loom-ring; models ->
loom.tla / loom_multishot.tla / loom_order.tla / loom_devgone.tla.

Also flagged: the doc's Status/header self-description understates the file (calls
loom.c "the ring substrate, no op flows yet" -- fifteen opcodes dispatch there),
a drift sub-kernel-loom's Caveats already record.

88 -> 89 absorbed of 157. lint 0-fail.
