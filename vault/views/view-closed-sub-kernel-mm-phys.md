---
id: view-closed-sub-kernel-mm-phys
type: view
title: "Do-not-re-report preamble — sub-kernel-mm-phys"
query: closed:sub-kernel-mm-phys
---
# Do-not-re-report preamble — sub-kernel-mm-phys

Generated from `fnd-*` notes (`quaestor render`; also emitted
on-demand by `quaestor closed sub-kernel-mm-phys`). Paste or
transclude into a prosecutor prompt as the closed-findings preamble.

Both listed findings are DOCUMENTED dormant geometries, not fixed
bugs: dense-Aff0 ([[seam-sparse-mpidr]]) and the relative RAM cap
([[seam-mm-directmap-cap-absolute]]) are each sound on every
current target and each owed one line at a future board bringup.
The P1-I-D round's fixes (F29/F32/F33/F34/F35/F37) predate
per-finding severity tagging and are recorded in
[[chg-2026-05-05-p1id-closing-audit]] rather than as fnd notes.

<!-- generated:begin -->
2 closed findings on [[sub-kernel-mm-phys]] — do NOT re-report
these in a future round (open/deferred findings are NOT listed
here; see the seam inbox):

- [[fnd-807-f1]] [P3] The fix stands on Aff0 == dense CPU index — a kernel-wide assumption, false on clustered SoCs (documented) — Documented as the canonical statement of a KERNEL-WIDE assumption:
- [[fnd-808-f2]] [P3] The 8 GiB RAM cap is mem_base-relative; the direct map it protects is absolute (documented) — Documented + phys.c carries the full caveat block. The fix is one
<!-- generated:end -->
