---
id: chg-2026-10-08-weft-reap-test-mark
type: chg
title: "The live weft reaper never sweeps a test's binding"
date: 2026-10-08
arc: arc-boosty
commits: ["910d74cc2", "bc05696fa"]
touched:
  - sub-kernel-weft
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-10-08
---
A test-harness soundness chunk between vmaguard and the exec + DMA question,
from vmaguard's audit r2 (F4). The G-3 reaper tests in `test_weft_share.c`
register a binding, kill its session and drive the sweep themselves on a
synthetic clock (1000-5000 s). The live reaper thread runs beside them: `main.c`
readies it before `test_run_all`, and once anything is registered it sweeps
every second on the real clock. A live sweep between a test's kill and its own
sweeps stamped the binding with real uptime, so the test's next sweep found the
stamp far past the grace and reclaimed one sweep early, or reclaimed the binding
itself and the window-hook tests' hook never ran. Whether a suite boot failed
depended on where the reaper group fell against the thread's second.

**What** ([[sub-kernel-weft]]). A test registers through
`weft_reap_register_for_test`, which marks the binding `reap_test_only` (a field
that exists only in test builds) and wakes nothing. The thread's
`weft_reap_sweep` skips a marked binding and `weft_reap_sweep_for_test` skips an
unmarked one; both are `weft_reap_sweep_in(now, test_only)`, and the mark is
written before the binding is linked under `g_weft_reap_lock`, the lock every
sweep reads it under. The two register forms share one body,
`weft_reap_link`. The production sweep compiles the comparison out and is
byte-identical in what it reclaims. Parking the thread for the reaper group was
rejected: a park set at a test's start is released only by a test that reaches
its last line, and every sibling returns on a failed assert, so one failing test
would leave the reaper parked for the rest of the suite. A mark has nothing to
release.

**Witness.** `weft.reap_live_sweep_leaves_test_bindings` drives the thread's own
sweep against a dead test binding at a clock far past the grace (no stamp, no
reclaim), then the test sweep one call away (stamp, then reclaim). The other
half -- the test sweep never touches a real binding -- holds by the same
comparison and has no witness: no real binding exists while the suite runs.

**Verification.** Audit by Fable 5.1 (start == end): r1 0/0/0/4, clean. The
four P3s were fixed in the close: the dossier's "sweeps while anything is
registered" (a test registration wakes nothing), the WIP commit's claim that a
live stamp makes the test's `now - stamp` wrap (on those clocks it exceeds the
grace), the duplicated register bodies, and the unwitnessed half named above.
Gates on bc05696fa: RED 4/4 as predicted (base and green 1964/1964; W1 red only at the witness's live-sweep line; W2 red at the witness plus the five converted tests whose own sweep must reclaim); ci-smp-gate N=10 50/50 (default smp1/4/8, UBSan smp4/8, no corruption).
