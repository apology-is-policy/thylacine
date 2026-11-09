---
id: fnd-noexec-r2-f5
type: fnd
round: adt-noexec-r2
severity: P3
status: fixed
title: "A test released its fixtures on its last lines and claimed more than it tested"
surface: [sub-kernel-exec]
threatens: []
fixed-by: chg-2026-11-05-noexec-r2
regression: "execve.load_says_nomem_on_a_refused_vma: W+X"
created: 2026-11-05
---
## Prosecution

`TEST_EXPECT_EQ` returns from its function, so a failure before the end leaked the Burrow and the Proc for the boot. The W+X leg said "whatever the slab holds" with the slab open.

## Disposition

The test collects its answers, releases, then asserts, and asks W+X both with the slab open and with it refusing.
