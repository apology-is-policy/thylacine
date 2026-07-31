---
id: adt-294-self
type: adt
title: "#294 concurrent self-audit (pre-formal)"
date: 2026-06-21
scope: [sub-kernel-ninep-dev9p-poll, sub-kernel-ninep-session]
reviewer: self
model-start: claude-opus-4-8
model-end: claude-opus-4-8
verdict: clean
counts: {p0: 0, p1: 2, p2: 0, p3: 0}
findings: [fnd-294-self-1, fnd-294-self-2]
round-of: chg-2026-06-21-294-cancel-at-close
created: 2026-07-31
---
Two would-be-P1s found and FIXED before the formal round -- the refs-born-
zero UAF and the awaiting_flush clunk-refusal (the latter BELOW the
model's abstraction, caught by the kernel test driving the real wire
path). The formal prosecutor re-verified both fixes correctly implemented.
The recorded lesson: model the design; the impl + a real-wire test catch
the impl-level bugs beneath it.
