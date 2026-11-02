---
id: adt-aux-corona-join-r1
type: adt
title: "The corona join (env-a + env-c into aux-3) round 1"
date: 2026-10-29
scope: [sub-kernel-poll, sub-kernel-ninep-client, sub-kernel-proc, sub-kernel-pts]
reviewer: fable
model-start: "claude-fable-5-1"
model-end: "claude-fable-5-1"
verdict: clean
counts: {p0: 0, p1: 0, p2: 0, p3: 2}
findings: [fnd-aux-corona-join-r1-f1, fnd-aux-corona-join-r1-f2]
round-of: chg-2026-10-29-aux-corona-join
created: 2026-11-02
---
holotype-reviewer on Fable 5.1, cross-family against the Opus implementer, on f7c4b5e14..a68fc0eca (the merge of corona c38f1cb1f into aux-3 b9e41c98c, base 69456f5f8, unbuilt at the time). Verified balanced: the 14 converted client-lock sites and the 4 converted table-lock sites. Withdrawn as guarded: `on_complete` and `progress_abort` under `c->lock`, the private hangup op, the executor's `l->lock`, exec's table -> table_lock -> `l->lock` edge, corona's kthreads. F1 changed where a wake runs, so round 2 was owed on the fixes ([[adt-aux-corona-join-r2]]).
