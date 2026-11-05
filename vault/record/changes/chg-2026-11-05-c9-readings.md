---
id: chg-2026-11-05-c9-readings
type: chg
title: "C9's readings: what the ISB is worth, the FPCR skip not built, and two corrections"
date: 2026-11-05
arc: arc-phase6-pouch
commits: [5c31d1867]
touched:
  - sub-kernel-thread
established: []
closed: []
opened: []
depth: skeletal
created: 2026-11-05
---
Under HVF, within one lease: direct 33 ns, same-space 48, cross-space 51, with-kernel 44, sched 68. With the kernel root's bug put back (S10), so kernel threads do V: direct 42, with-kernel 49. With the MSR always written (G01): direct 38, same-space 52. With no ISB (G02): direct 17, same-space 34, sched 50, so the ISB costs about 16 ns of a switch. Figures from different days are not comparable: the isb piece read 17 ns on an earlier day and 8 here. No real CPU's figures were taken.

G02 was predicted green and went red. Without the ISB, joey's `go-threads` budget run lost `failed to create new OS thread`: 154 bytes of the Go runtime's message reached the console as NULs, against none in the green boot, and the boot ended. It fits a thread that resumes in the kernel and reads user memory before its TTBR0 write has taken effect, which is the reason `context.S` already gives. That is a reading of one boot, not a proof. It does show that this machine can witness the ISB, which the sabotage's own comment said it could not.

FPCR's write is not skipped when the value is equal, though Linux does it: `fpcr-rw` reads 4 ns for a read and a write together, on a switch of 48, and the ISB stays regardless.

Two corrections to earlier records. `docs/JOURNAL.md`'s C8b entry says `TEST_EXPECT_EQ` carries on after a failure. It does not: `test.h` has both macros return from the function they are in. X08 showed several reds because its asserts sit in a helper, so a flat test's `FAIL:` line is its first red and a test with helpers' is its last. And the lease scripts counted the runner's `[test] yield-waits` summary line as a test, so C8a's, C10's and C8b's counts read one high: their boots said 2211, 2216 and 2221.
