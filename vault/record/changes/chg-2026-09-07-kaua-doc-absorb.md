---
id: chg-2026-09-07-kaua-doc-absorb
type: chg
title: "absorb docs/reference/112-kaua (the console TUI substrate): clean redirect to sub-kaua (ahead of the doc's stale Status table)"
date: 2026-09-07
arc: arc-vault
commits: ["PENDING"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-07
---
The Kaua console-TUI substrate (usr/lib/kaua). Audit-trigger surface. quaestor
owner: input/term/query/source (+ the pure layers) -> sub-kaua (audit:light,
updated 2026-08-03). NOTE: sub-kaua-term is a DIFFERENT surface (the kaua-TERM app,
doc 152, KT-1). Verified atom-by-atom.

ALREADY COVERED (verified, sub-kaua current + ahead of the doc): the O(1) total
parser (feed accepts any bytes, csi_overflow latch, param-flood yields no event,
UTF-8 bounded at 4, the per-round drain cap); I-27 consumed-not-introduced (fd 0/1
ONLY, never consctl, never console-attach -- "a negative property"); the crash
backstop (Drop restores clean, panic=abort so Drop skipped on crash -> ut post-reap
restore is authoritative, both idempotent); the #117 CPR handshake (F1 total-deadline
bounded, F2 lossless -- stops at R, returns pending type-ahead for with_pending
replay, late-CPR-as-resize); the diff renderer + the Loom EventSource seam +
layout/widget.

Zero-fold. The doc's Status table is internally STALE (says T-4 dance + audit "not
started" while its own header says T-4 @77386f7 landed + audit CLOSED 0P0/0P1/0P2/3P3
at #106); sub-kaua reflects the landed+closed reality. Width-1 cells,
query-at-launch-only winsize, richer-layout/LoomSource are live v1.x seams the
dossier holds. Redirect stub. Zero code change.
