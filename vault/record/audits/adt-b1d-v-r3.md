---
id: adt-b1d-v-r3
type: adt
title: "B-1d-v round 3: the fixes hold; the round-2 regression witness was red on the kernel it was written for"
date: 2026-09-28
scope: [sub-kernel-devsrv, sub-kernel-territory, sub-kernel-syscall-dispatch, sub-kernel-syscall-abi, sub-kernel-stalk, spec-territory]
reviewer: fable
model-start: "claude-fable-5-1"
model-end: "claude-fable-5-1"
verdict: clean
counts: {p0: 0, p1: 0, p2: 1, p3: 3}
findings: [fnd-b1d-v-r3-f1]
round-of: chg-2026-09-25-b1d-v-emount
created: 2026-09-28
---
## Scope

WIP 6 (b46e555e): round 2's fixes -- the per-post devsrv `qid.path`, the
`devsrv.service_keys_distinct` regression, the extended file-point test, and
the doc / comment corrections. Fable 5.1 at max: family diversity restored
(rounds 1-2 were the Opus-on-Opus fallback) and context independence, every
load-bearing claim re-derived from the code.

## Convergence

0 P0 / 0 P1 / 1 P2 / 3 P3. The devsrv identity fix is sound: unique, never-0
per-post paths, a one-hold walk, no alias surviving across roots or registries,
no consumer that leaned on the old 0. The P2 ([[fnd-b1d-v-r3-f1]]) is the
round-2 regression WITNESS, not the kernel: both its resolution legs crossed a
`devnone` source, which cannot be crossed, so the test was red on the very
kernel it was written to guard -- and it had never run (the WIP was unbuilt).
Rewritten to assert on the mount table (`mount_is_point_id` /
`mount_member_at`), which discriminates without crossing, and verified red on
the reverted kernel and green on the fix. F2 was an unfilled TLC placeholder in
scripture (filled from the re-run), F3 a stranded-mount arm the per-post fix
introduces (documented; the general fix is the OPEN-BUGS mount-key generation),
F4 a record's `threatens` field contradicting its body. A test-only P2 fix,
verified both ways, is a clean close with no round 4.
