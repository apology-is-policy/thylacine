---
id: view-closed-sub-kernel-ninep-dev9p-poll
type: view
title: "Do-not-re-report preamble — sub-kernel-ninep-dev9p-poll"
query: closed:sub-kernel-ninep-dev9p-poll
---
# Do-not-re-report preamble — sub-kernel-ninep-dev9p-poll

Generated from `fnd-*` notes (`meta/lint.py --render`; also emitted
on-demand by `lint.py --closed sub-kernel-ninep-dev9p-poll`). Paste or
transclude into a prosecutor prompt as the closed-findings preamble.

<!-- generated:begin -->
7 closed findings on [[sub-kernel-ninep-dev9p-poll]] — do NOT re-report
these in a future round (open/deferred findings are NOT listed
here; see the seam inbox):

- [[fnd-294-r1-f1]] [P3] The teardown test's mid-test op-count snapshot races the GC kthread (fixed) — Fixed (the prosecutor's endorsed option): the racy snapshot dropped; the
- [[fnd-294-r1-f2]] [P3] The session-ref leg + the SMP grab-vs-borrow race are reasoning-validated only (documented) — Documented: both prosecutor and self-audit traced the balance by hand (+1
- [[fnd-294-self-1]] [P1] Poll-state refs born zero -- the first op teardown frees it under p->poll (fixed) — Fixed (self-found, pre-formal): `cand->refs = 1` BEFORE the RELEASE
- [[fnd-net6b-r1-f1]] [P1] The global poll-pump pumped only the head op's client -- a second QTPOLL client starves (fixed) — Fixed: `dev9p_poll_collect_clients` -- distinct clients deduped into a
- [[fnd-net6b-r1-f2]] [P3] OOM with no covering op parks an infinite-timeout poll unwakeably (fixed) — Fixed: degrade to always-ready when no path to a COVERING completion
- [[fnd-net6b-r1-f5]] [P3] The lockless p->poll fast-path read was unannotated (fixed) — Fixed: ACQUIRE load paired with the RELEASE publish. Hygiene +
- [[fnd-net6b-r2-f2]] [P3] Narrower-live-op under sustained OOM makes no progress (fixed) — Fixed: folded into the F2 degrade condition (not just a comment) -- the
<!-- generated:end -->
