---
id: view-closed-sub-kernel-mm-slub
type: view
title: "Do-not-re-report preamble — sub-kernel-mm-slub"
query: closed:sub-kernel-mm-slub
---
# Do-not-re-report preamble — sub-kernel-mm-slub

Generated from `fnd-*` notes (`quaestor render`; also emitted
on-demand by `quaestor closed sub-kernel-mm-slub`). Paste or
transclude into a prosecutor prompt as the closed-findings preamble.

The RW-1 slice's theme, worth handing any future round: every
Phase-1 "runtime doesn't do that yet" economy became a finding once
the runtime did. The remaining named economy is
[[seam-slub-debug-mode]] (no double-free detection) — a prosecutor
re-reporting it has found the seam.

<!-- generated:begin -->
3 closed findings on [[sub-kernel-mm-slub]] — do NOT re-report
these in a future round (open/deferred findings are NOT listed
here; see the seam inbox):

- [[fnd-rw1-af1]] [P2] kmalloc's page-rounding wrapped near SIZE_MAX — a giant request became a 1-page success (fixed)
- [[fnd-rw1-af2]] [P2] The global cache list had no lock — runtime create/destroy raced every walker (fixed)
- [[fnd-rw1-fs1]] [P2] kmem_cache_destroy's guard tested nr_full — a live object on a PARTIAL slab was freed silently (fixed)
<!-- generated:end -->
