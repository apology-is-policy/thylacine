---
id: chg-2026-07-31-ninep-pilot
type: chg
title: "The 9P-client pilot: one subsystem end-to-end across all four planes"
date: 2026-07-31
arc: arc-vault
commits: ["719c8bc3"]
touched: []
established:
  - moc-kernel
  - moc-kernel-ninep
  - sub-kernel-ninep-client
  - inv-i9
  - inv-i10
  - inv-i11
  - spec-9p-client
  - spec-reader-frame
  - gate-smp
  - lock-9p-client-c-lock
  - lin-9p-client
  - haz-single-waiter-rendez
  - haz-shared-stream-desync
  - haz-death-path-wake
  - seam-841-mi-harness
  - seam-350-async-eagain
  - seam-845-untrusted-server
  - seam-56-netd-cancelled-tag
  - seam-90-hung-server
  - seam-90-death-half
  - view-closed-sub-kernel-ninep-client
closed: []
opened: []
mirrors-checked: []
depth: rich
---
## What

The migration's shape-judging pilot: the 9P client absorbed end-to-end.
Present plane -- the full dossier (all twelve sections; the Prosecution
section is now the single home of the former CLAUDE.md/ARCH trigger-row
content for this surface), the kernel + 9P MOC spine, three invariant
statements (I-9/I-10/I-11 -- the §28-text single-home per amendment §6.1),
two spec dossiers, the SMP gate, the c->lock note, three hazard classes,
the lineage note, and six seams (five open, one closed-with-lifecycle).
Record plane -- the full #841 → #845 → #349 → #375 → #52/#53 → 8c-3 → #90
backfill: 3 retro arcs (held `active` while backfill accretes; bodies say
so), 7 change notes with real SHAs, 14 audit rounds, 32 findings with
frozen prosecution chains + dispositions. Views -- the parameterized
`closed:<sub-id>` renderer landed in the linter and
[[view-closed-sub-kernel-ninep-client]] is the first committed
do-not-re-report preamble. Tier-3 session hooks wired into
`.claude/settings.json` (advisory, guarded, merge-safe -- main has no
tracked settings.json). The linter also gained the unterminated-flow-list
guard the pilot itself tripped.

## Why

The schema's acid test (§9.5): this surface's history is the richest and
its old CLAUDE.md row interleaved both planes in one stream. Judging the
split here de-risks the sweep.

## Alternatives rejected

One fake "retro hardening arc" for all seven chunks (misrepresents
history -- the chunks belonged to three real eras); creating stub dossiers
to complete inv-i9's full guard list (fake-rich; partial-and-noted instead);
`deferred` status on findings later fixed cross-chunk (closure fields
reflect NOW -- `fixed-by` carries the cross-chunk edge).

## Verification

`lint.py --all` + `--staged` green over 85 notes; `--render` regenerates
five views + the closed preamble; the three multi-line flow lists the
generator emitted were caught only by manual sweep, so the linter now FAILS
on them (revert-probe: the guard fires on the pre-fix files). Deferred, in
scope for the registry passes: heatmap + lock-order + mirrors renderers
(need registry data to be non-trivial); the dossier Provenance
auto-renderer.
