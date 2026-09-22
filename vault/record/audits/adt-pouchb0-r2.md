---
id: adt-pouchb0-r2
type: adt
title: "Pouch 0033-0038 round 2: 0035 re-verified on the real sources; a sixth class member, a wild write, and a lock held across four RPCs"
date: 2026-09-21
scope: [sub-pouch-seam, sub-pouch-net, sub-pouch-fs, sub-pouch-thread]
reviewer: fable
model-start: "claude-fable-5-1"
model-end: "claude-fable-5-1"
verdict: dirty
counts: {p0: 0, p1: 0, p2: 2, p3: 7}
findings: [fnd-pouchb0-r2-f1, fnd-pouchb0-r2-f2]
round-of: chg-2026-09-21-pouch-b0-libc
prior-round: adt-pouchb0-r1
created: 2026-09-21
---
## Scope

Branch `browser-b0` @ 9f7613f0: the round-1 fixes (0033..0038 and the four provers). Read-only. The reviewer rebuilt the series (38 / 38, zero fuzz), proved the device build's libc tree byte-identical to it, and drove the REAL patched stdio sources against an independent reference over nine buffer sizes: 0 failures in 288,000 trials, the `>=` first draft failing 1,669 / 2,000 at `buf_size == 1` and nowhere else.

## Convergence

The same shape as round 1, one step further out. `ualarm()` was a sixth member of the class 0037 claimed to have swept, and the closed list said the opposite ([[fnd-pouchb0-r2-f1]]). A pouch socket fd does not fit an `fd_set`, so `FD_SET(sock, &set)` was a store 128 MiB past the set in application code -- pre-existing, but the census paragraph written in round 1 said every remaining tag-unaware call "fails visibly", and the census method could not see a call that takes a bitmap or an array ([[fnd-pouchb0-r2-f2]]). The P3s included 0036's locking being wrong twice under comments asserting the opposite, a link-closure defect no prover can see, five prover legs that could not fail, twelve false sentences (one of them P-4's review inventory, which never existed), and `O_APPEND` never having used the kernel's append bit. Recorded `dirty` because the 0036 fix is a restructure; a third round follows on the fixes.
