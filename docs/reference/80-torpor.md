# 80 — torpor [ABSORBED INTO THE VAULT]

Absorbed at the memory/ipc-wake sweep (`chg-2026-08-01-mm-ipc-sweep`).
Its content now lives, code-verified and current, in:

    vault/system/kernel/ipc-wake/sub-kernel-torpor.md

(the prose I-9 proof, the R-5 pre-fault, the #343 lock-free mismatch,
the die-pending re-check, the death and stop cascade walks, the full
caveat set.)

**What this file got WRONG by the time it was absorbed.** The
best-maintained of the six absorbed docs — the WAIT state machine and
lock-order sections were genuinely updated for R-5 and #343 — which
makes its residue the purest specimen of additive maintenance:

- The Lock order section states the RETIRED chain
  ("`torpor_lock` → `vma_lock` → `buddy` held across the fault-path")
  as a present-tense fact, four lines above the appended paragraph
  recording its two-step retirement. The truth is narrower and worth
  stating exactly: the edge survives only in the decommit-race
  window's non-blocking lazy-anon re-fault.
- The caveats still carry "**No `-EINTR`** at v1.0 — notes/signals
  don't yet propagate through the wait path" — three generations
  stale (#811 TSLEEP_INTR absorbed by fall-through, LS-5c's widened
  predicate, the #19 stop detour), and the F8 "lock held across
  demand-paging" caveat describes the pre-R-5 world.
- Neither `torpor_wake_all_for_proc` nor
  `torpor_stop_wake_all_for_proc` — the two exported cascade walks,
  the surface's death and job-control integration — appears anywhere
  in the file.
- "v1.0 is single-threaded so torpor is uncontended … Not yet
  measured" — the #343 measurement (67.7 M calls per go build,
  54 % on the mismatch path) is the surface's defining number.
