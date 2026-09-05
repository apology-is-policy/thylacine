---
id: fnd-ls4-r1-f2
type: fnd
title: "chdir resolves the path lexically, then stalk re-clamps what is already clean"
round: adt-ls4-r1
severity: P3
status: fixed
surface: [sub-kernel-territory]
threatens: []
fixed-by: chg-2026-06-09-ls4-cwd
regression: "none (a comment; the redundancy is deliberate)"
created: 2026-08-01
---
## Prosecution

`sys_chdir_handler` resolves the input to a cleaned absolute path, then
hands that to `stalk`, which runs its own `..` containment — on a string
that provably contains no `.` or `..`. The second clamp can never fire.

## Disposition

Fixed as a comment at the call site, NOT by removing the redundancy.

The point is that this is the belt-and-braces the design wanted. Recorded
as a finding so that a future reader who notices the dead clamp finds an
explanation instead of an optimization opportunity: removing it would
make the lexical resolver the SOLE containment for cwd-relative paths,
converting a redundant wall into a single point of failure on the one
surface ([[inv-i28]]) where that is least acceptable. The cost is a
no-op comparison per component.
