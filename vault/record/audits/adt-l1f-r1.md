---
id: adt-l1f-r1
type: adt
title: "Larder L1f arc-close round (the whole L1c–L1e surface)"
date: 2026-07-09
scope: [sub-kernel-larder, sub-kernel-ninep-dev9p]
reviewer: opus
model-start: claude-opus-4-8
model-end: claude-opus-4-8
verdict: clean
counts: {p0: 0, p1: 1, p2: 0, p3: 2}
findings: [fnd-l1f-r1-f1, fnd-l1f-r1-f2, fnd-l1f-r1-f3]
round-of: chg-2026-07-09-larder-l1f
created: 2026-07-31
---
Opus-4.8-max prosecutor (Fable out of quota, user-directed — the
fallback tier per the reviewer-model rule; MODEL start==end, no
mid-run fallback) + a concurrent 14-trace self-audit, over the whole L1
surface: larder.c/.h, the eight dev9p hook sites, the client init/
destroy + cacheable latch. NOT dirty (the P1 fix is a one-line
invalidate + a regression). The value-of-two-prosecutors instance: F1
was the exact PAGE twin of the self-audit's own SA-10 attr-defense trace
— the self-audit verified the attr child-invalidate present and never
asked about the page twin ("the guard that exists is what stops you
asking whether it is the whole guard"); conversely the self-audit
independently derived F3 (its SA-1) before the agent reported it.
Verified-sound set (converged, do-not-re-litigate): serve/invalidate/
evict/destroy atomicity under the one leaf lock; the gen-guard
resurrection close; page-buffer lifetime (valid ⟹ buffer present; no
UAF/double-free/stale-tail); partial-page/EOF honesty; the cacheable
gate's completeness + fail-safe default; the I-28 fail-ordering
transparency of the dentry serve; I-11 (a bind-form full walk always
RPCs); the LRU/I-32 bound; root qid 0 via the valid bit; the
kernel-to-kernel copy premise (every Dev.read caller passes kernel
buffers). SMP witness: the full gate 40/40, 0 corruption. Coverage note
(honest): the SMP interleavings are not deterministically unit-testable
— the gate is their witness, the standing pattern.
