---
id: view-closed-sub-pouch-fs
type: view
title: "Do-not-re-report preamble — sub-pouch-fs"
query: closed:sub-pouch-fs
---
# Do-not-re-report preamble — sub-pouch-fs

Generated from `fnd-*` notes (`quaestor render`; also emitted
on-demand by `quaestor closed sub-pouch-fs`). Paste or transclude
into a prosecutor prompt as the closed-findings preamble.

Read it WITH the resolution history: `openat` has three generations
(0009's per-component walk, 0021's single stalk-resolved open, 0024's
create arm), and the middle one was a CORRECTNESS fix — the walk loop
opened every intermediate with the final omode, so it structurally could
not open a write-mode file through a directory or across a mount. A
finding that pouch duplicates resolver work is a finding that the
migration missed a site.

The `struct t_stat` mirror appears three times in this surface and three
more outside it; a per-mirror `_Static_assert` proves nothing about the
kernel.

<!-- generated:begin -->
0 closed findings on [[sub-pouch-fs]] — do NOT re-report
these in a future round (open/deferred findings are NOT listed
here; see the seam inbox):

<!-- generated:end -->
