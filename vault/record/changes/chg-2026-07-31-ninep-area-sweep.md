---
id: chg-2026-07-31-ninep-area-sweep
type: chg
title: "The 9P-area sweep: wire, session, transports, attach, dev9p, dev9p.poll"
date: 2026-07-31
arc: arc-vault
commits: ["9b4df105"]
touched:
  - moc-kernel-ninep
  - inv-i9
  - inv-i10
  - inv-i11
  - spec-9p-client
established:
  - sub-kernel-ninep-wire
  - sub-kernel-ninep-session
  - sub-kernel-ninep-transport
  - sub-kernel-ninep-attach
  - sub-kernel-ninep-dev9p
  - sub-kernel-ninep-dev9p-poll
  - inv-i38
  - spec-net-poll
  - spec-net-poll-teardown
  - spec-fs-cache
  - lock-dev9p-poll-glock
  - lock-dev9p-wb-priv
  - seam-221-idle-pump-wake
  - seam-223-pump-tail-starvation
  - seam-848-pivot-walk-race
  - seam-wb-close-flush-slot
  - seam-co-fidless-wstat
  - view-closed-sub-kernel-ninep-dev9p-poll
  - view-closed-sub-kernel-ninep-attach
  - view-closed-sub-kernel-ninep-transport
closed: []
opened: []
mirrors-checked: []
depth: rich
---
## What

The per-subsystem sweep's first batch: the six subsystems completing the
9P area, each dossier written FROM THE CODE (all six surfaces read in
full this session — ~7.5k lines), at a depth exceeding the reference
docs they absorb (which were 1–14 months stale: pre-#841 session shapes,
pre-#371 op numbers, "deferred" rows for long-landed slots). Reference
docs 44/45/46/47/48/49/50/55 are absorbed and stubbed (the per-subsystem
store-liveness cutover — 47 retroactively, the pilot's owed stub).
Registry: [[inv-i38]] (statement single-home), three spec dossiers
(net_poll, net_poll_teardown, fs_cache — cfg inventories verified against
`specs/` on disk), two lock notes, five seams. Record backfill: 2 retro
arcs (net, pouch-boot) + 13 chgs (verified SHAs) + 7 audit rounds + 33
findings with frozen prosecution chains — the 16c, net-6b, #294, and #99
histories; existing arcs (identity-detour, go-build) grew their chunk
lists under the active-arc exemption. Two more committed do-not-re-report
preambles (attach, transport — 16c is their complete formal history;
dev9p-poll — net-6b + #294 complete).

## Why

"Adjacent first": these six reuse the pilot's invariants, specs, and
hazards, and complete [[moc-kernel-ninep]] end-to-end — the whole kernel
9P stack now has one navigable, code-verified home. Sweeping against the
code rather than the docs was the session's explicit direction (validate
and EXTEND the detail level), and it paid: every dossier corrects stale
doc claims (op numbers, deadline semantics, vtable coverage) that a
doc-fusion sweep would have propagated.

## Alternatives rejected

Closed views for session/dev9p now (REJECTED — incomplete per-surface
histories: session's rounds are entangled with the client scope's
findings, dev9p's wb/term-4/CF-3/weft/A-3 rounds are not yet backfilled;
an incomplete preamble invites re-reports, so those surfaces keep the
memory closed-lists authoritative until their backfills complete —
noted in each dossier). Backfilling every finding of every skipped round
(REJECTED for this batch — R7 skeletal chgs index them honestly; the
larder/pouch/netd sweeps own their rounds). Fnd notes for out-of-surface
findings (net-6b F4 → pouch, #294 F3 → netd, 16c F12 → joey: recorded in
their rounds' bodies, backfilled when their surfaces exist — an fnd with
a fabricated surface would poison the per-surface views).

## Verification

`lint.py --all` green over the full corpus; views regenerated; the three
new preambles rendered from the fnd set; revert-probes on the new
surfaces (a dangling edge, a stale view, a dropped dossier section — see
the commit); symbol citations spot-verified against the tree during the
code read (op numbers, seven `any_outstanding_on_fid` callers, the
`DEV9P_POLL_MAX_PUMP` bound, budget constants). Known intentional gaps
(carried forward, not dropped): the Larder mechanism + netd + pouch
sweeps own their fnd backfills; `spec-fs-cache.models` grows to the
larder sub at its sweep.
