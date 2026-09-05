---
id: adt-rw4-r1
type: adt
title: "HOLOTYPE RW-4 (namespace/FS + 9P + Loom-DELTA) round 1"
date: 2026-06-10
scope: [sub-kernel-stalk, sub-kernel-srvconn, sub-kernel-ninep-session, sub-kernel-ninep-client, sub-kernel-devsrv]
reviewer: fable
model-start: "claude-fable-5"
model-end: "claude-fable-5"
verdict: dirty
counts: {p0: 0, p1: 2, p2: 2, p3: 2}
findings: [fnd-rw4-sa-f1, fnd-rw4-rev1-f1, fnd-rw4-rev2-f1, fnd-rw4-rev2-f2, fnd-rw4-rev3-f1, fnd-rw4-rev3-f2]
round-of: chg-2026-06-10-rw4-fixes
created: 2026-08-01
---
## Scope

FOUR parallel Fable holotype-reviewers split by sub-surface — R1
territory/stalk/spoor/dev.c, R2 devramfs/dev9p/devsrv/srvconn/devnone/
devctl, R3 the whole 9P stack, R4 loom.c DELTA + the 9p_client seam
(all four MODEL start==end, no fallback) — plus an Opus self-audit.
The counts are the FIXED set (2 P1 + 2 P2 + 2 P3); six more items were
REGISTERED, not fixed, per the fix policy (below).

## Convergence

The round's systemic theme, encoded as scripture at the close: **the P6
multi-thread-Proc lift outran the serialization of per-Proc SHARED
state.** SA-F1 (the Opus self-audit's Territory ns_lock P1 — R1 had
rated the same facts "still dormant" and was OVERRULED: the kernel must
be sound against any EL0 program, not the current in-tree set) and
R2-F1 (the byte-mode srvconn single-waiter extinction) are the same
class as RW-2's findings; the cross-cut ordered a sweep of EVERY
per-Proc-shared structure. The R4-F1 SQPOLL wait-guard P2 (fixed,
loom-surface — enters the Record at the loom sweep) falsified a Loom-5
disposition. REGISTERED (unfixed, surface-tagged for their sweeps):
R3-F2 monotonic-fid burn ([[fnd-rw4-rev3-f2]] →
[[seam-fid-monotonic-reclaim]]); R3-F3 the ARCH-21.5
block-on-tag-full scripture reconcile ([[seam-9p-tag-block-on-full]],
a USER call); R3-F4 partial-walk -EIO vs Plan 9 last-bound-qid
(note-only); R4-F2 SQPOLL busy-poll-reap strand, R4-F3 multi-client
ring pump starvation, R4-F4 the io_uring SINGLE_ISSUER/NODROP SOTA
surface (loom-side, pend that sweep). Verified-SOUND (adopt): the
stalk I-28 set (containment, crossed-root X-ordering, refcount balance,
amode fail-closed), territory I-3 enforcement, spoor refcount, the R2
dev-driver set, the R3 9P set (the #841/#845 closed-list claims HELD
under a fresh 2-in-flight + mid-RPC-death prosecution), the R4 Loom
delta. The invasive fixes (a lock-order lift + 3 wait/wake changes)
made the close DIRTY → [[adt-rw4-r2]].
