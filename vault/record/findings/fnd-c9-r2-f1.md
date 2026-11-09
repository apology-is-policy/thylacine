---
id: fnd-c9-r2-f1
type: fnd
round: adt-c9-r2
severity: P3
status: fixed
title: "A test resolves ASIDs for a CPU it is not on, and marks that CPU's owed flush paid"
surface: [sub-kernel-thread]
threatens: []
fixed-by: chg-2026-11-05-c9-r2
regression: "`asid.rollover_preserves` and `asid.distinct_active`, kernel test builds only"
created: 2026-11-05
---
## Prosecution

`test_asid.c` named CPU 1 from the runner on CPU 0. `asid_resolve` writes the named slot, so what CPU 1 really ran lost its reservation, and the exhausting resolve ran CPU 1's owed flush with `tlbi vmalle1` on CPU 0's TLB and cleared the flag. A later Proc on CPU 1 then takes the fast path over stale entries, the `fast_no_flush_check` state of `specs/asid.tla`. It needs an ASID-tagged TLB and a user Proc's entries on CPU 1 when the test runs, which the suite's order does not arrange.

## Disposition

A test build has one slot past the real CPUs, `asid_fixture_slot()`, so the fixture no longer touches a slot a CPU runs on, in any configuration including smp8. The fix is one macro in `asid.c`; a production build's arrays and loops are as they were. The test now asserts the rollover left CPU 1's flush owed and its own slot paid. S11 points the slot back at CPU 1.
