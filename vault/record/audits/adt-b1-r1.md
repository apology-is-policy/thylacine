---
id: adt-b1-r1
type: adt
title: "B1 per-attach loose-mode round (the gen-witness catch)"
date: 2026-07-11
scope: [sub-kernel-larder, sub-kernel-ninep-dev9p, sub-kernel-ninep-client]
reviewer: fable
model-start: claude-fable-5
model-end: claude-fable-5
verdict: clean
counts: {p0: 0, p1: 0, p2: 1, p3: 2}
findings: [fnd-b1-r1-f1, fnd-b1-r1-f2, fnd-b1-r1-f3]
round-of: chg-2026-07-11-b1-loose
created: 2026-07-31
---
Fable-5-max prosecutor; the concurrent 12-category self-audit found
NOTHING — the prosecutor found all three, including the one substantive
hole the self-audit's atomicity trace missed. The recorded lesson (the
round's durable export, a sibling of the RC-3 F1 class): **a two-party
serialization argument ("the invalidate precedes or follows") is
incomplete until every THIRD party that can re-establish the observed
state is enumerated** — here a stale-fid reader repopulating
just-invalidated pages at the OLD cvers. F1 additionally PUNCTURED a
prior round's verified-sound entry (the fid-lifecycle "snapshot
atomicity SOUND") — pre-existing since the keeper commit on BOTH strict
and loose paths; the loose review is what re-opened the question.
Verified sound (do-not-re-litigate): strict-path byte-identity; the
loose gate's forward+reverse exactness (grep-complete reader/writer
sets); `larder_walk_serve` cannot full-hit with incomplete sts; cvers
keying coherence (attr installs always wire-sourced);
`pages_cover_locked` re-check completeness incl. partial-page +
size==0; the flag lifecycle set-once-pre-publication; the ABI flag-day
complete (3 EL0 call sites, no stale caller); the joey single-client
premise ground-truthed; error paths balanced; the tests pinning both
sides (loose zero-wire + strict exactly-one-wire as the leak detector).
