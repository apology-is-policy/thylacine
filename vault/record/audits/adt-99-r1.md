---
id: adt-99-r1
type: adt
title: "#99 holotype round (create-errno + the stale negative dentry)"
date: 2026-07-19
scope: [sub-kernel-ninep-dev9p]
reviewer: fable
model-start: claude-fable-5
model-end: claude-fable-5
verdict: clean
counts: {p0: 0, p1: 1, p2: 0, p3: 4}
findings: [fnd-99-r1-f1, fnd-99-r1-f2, fnd-99-r1-f3, fnd-99-r1-f4, fnd-99-r1-f5]
round-of: chg-2026-07-19-99-create-errno
created: 2026-07-31
---
Fable-5-max holotype (MODEL start==end) + concurrent self-audit + the SMP
gate: all three independently found the SAME F1 gap (the stale negative
dentry surviving an EEXIST race) and prescribed the SAME fix -- a
three-way convergence. NOT dirty (the P1 fix is a two-line invalidate on
existing arms). Verified sound and not re-litigated: the clamp exactness,
sign/width through the go asm decode, the no-sharing lifetime of
create_errno, non-dev9p Devs byte-identical, the fid ledger.
