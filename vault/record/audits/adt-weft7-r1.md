---
id: adt-weft7-r1
type: adt
title: "Weft-7: the whole Weft EL0 surface (6a-6c) — the arc close"
date: 2026-06-21
scope: [sub-netd-server]
reviewer: opus
model-start: claude-opus-4-8
model-end: claude-opus-4-8
verdict: clean
counts: {p0: 0, p1: 0, p2: 1, p3: 3}
findings: [fnd-weft7-r1-f4]
round-of: chg-2026-06-21-weft7-close
created: 2026-07-31
---
The first + final formal prosecution of the Weft dataplane (I-37;
holotype-reviewer agent, model-overridden to opus). The netd-surface
finding is [[fnd-weft7-r1-f4]]; the kernel-surface findings stay in
this body until the kernel weft sweep: **F1 [P2, FIXED]** — an ungated
`SYS_WEFT_SHARE` let any EL0 Proc squat the fixed 64-slot share
registry (its ids never claimable — only netd's Rweft hands the kernel
one), starving netd's `weft_ensure` → every flow degrades to byte-copy
system-wide (an availability DoS; fixed by the `CAP_HW_CREATE` gate +
the `weft.share_cap_gate` regression; the lesson: "is this syscall
gated to the role that legitimately calls it?" — a resource bound is a
backstop, not the gate). **F2 [P3, seam #289]** — a transient
`SYS_WEFT_MAP` failure permanently pins a flow byte-copy (the consumed
id re-returned idempotently, unclaimable; the `SYS_WEFT_UNSHARE` GC is
the v1.x fix). **F3 [P3, FIXED]** — `dev9p_close`'s plain `p->weft`
clear made ACQUIRE/RELEASE-symmetric. The SOUND set (the cross-Proc
#847 dual-refcount ledger, share_id consume-once/unforgeability, the
I-30 pin + ring TOCTOU, the I-29 clamp, the I-9 RX-defer, W^X,
kernel-owned geometry) traced and survived; spec gate green (weft +
weft_readiness + loom, all cfgs). Also carried: the **#290 bench
correction** — the "weft slower" claim was a measurement artifact
(unmatched sizes + a transport-independent readiness stall
misattributed); instrument before writing a contradicting-design
conclusion into scripture.
