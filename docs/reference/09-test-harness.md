# 09 — Test harness [ABSORBED INTO THE VAULT]

Absorbed at the substrate sweep (`chg-2026-08-01-substrate-sweep`). Its
content now lives, code-verified and current, in:

    vault/system/substrate/sub-substrate-gates.md         (test.sh, the
                                                           multi-boot
                                                           classifier, the
                                                           v8.0 floor guard)
    vault/system/substrate/sub-substrate-interactive.md   (LS-CI: the PTY
                                                           harness and its
                                                           fault taxonomy)
    vault/abis/abi-boot-banner.md                         (the two ABI
                                                           strings both
                                                           halves key on)

The in-kernel runner itself (`kernel/test/`) is guest code and belongs with
the kernel; it lands with that sweep.

**What this file got WRONG by the time it was absorbed** — and the way it
was wrong is a mode the earlier sweeps had not seen.

This was a JANUS document. Its spine was frozen at P1-F while two excellent,
current sections were grafted onto it: the #77/#92 `TEST_YIELD_UNTIL`
treatment and the whole LS-CI half. The document was edited repeatedly, with
care, in two places — and the edits never met.

So it stated the suite size twice, in two of its own sections, as **4** and
as **1233**:

| Where | Claim | Truth at absorption |
|---|---|---|
| "Tests catalog (current)" | four tests, listed by name | `g_tests[]` holds **1237**, across 121 files |
| the banner example | `tests: 4/4 PASS`, `phase: P1-F` | — |
| "Implementation > Runner" | "Single-threaded by design at v1.0 (NCPUS = 1 still)"; "When SMP arrives at Phase 2" | SMP landed long ago; the section grafted directly ABOVE it is entirely about smp4/smp8 peer-thread races |
| "Not yet implemented" | the 10000-iteration leak check, tests for scheduler / territory / handle table / 9P client / syscalls | all exist; `phys.leak_10k` sits four lines below `phys.alloc_smoke` |
| "Error paths" | "Boot timeout (10s by default in tools/test.sh)" | 90 s, or 300 s with the GOROOT baked |
| the #92 section | `1232/1233 FAIL` | the only roughly-right number in the file |

`docs/REFERENCE.md`'s index row was a third, independent count — "6 leaf-API
tests at v1.0" — and listed as still-pending the very leak check that sits
in the registry it indexes. Three published numbers for one quantity, in
three places that cross-reference each other, none of them right.

The self-similar micro-instance, in the newest section: "**Four**
portability facts are load-bearing", followed by **six** numbered items. The
list grew; its header did not.

None of this made the new sections wrong — they are the best writing in the
file, and one of them opens by explicitly RETRACTING an unmeasured claim of
its own (#72's "host timing"). The habit of self-correction was live the
whole time. It simply never looked up.

Binding design (unchanged): `docs/TOOLING.md` (the harness contract),
`docs/LIFE-SUPPORT.md` ("LS-CI").

---

## PENDING ABSORPTION — content added on `main` after this file was stubbed

**Do not delete this section without folding it.** It records material that
landed here post-absorption and has no dossier yet, because the in-kernel
runner is unowned (see the pointer at the top: `kernel/test/` lands with the
kernel sweep). Tracked as a vault task so it cannot be lost by silence.

**From `#130` residue (`d669299c`, 2026-08-03)** — three additions, all about
the in-kernel runner and the console test hooks:

1. **The wait-predicate shapes table gained a precondition, and the reason is
   a correction.** The table listed "assert a flag cleared" as a sound
   observable with no qualification. **A cleared flag means the act STARTED,
   not that it FINISHED** — waiting on `!pending` is sound only where the
   clearer provably completes the act, with no clear-and-bail path and no
   window before the effect the test reads. That is a property of the
   *specific consumer* and must be re-established per site, never inherited
   from the table. Where it does not hold, the wait exits early and the assert
   reads pre-act state — the original race with a guard bolted on, which is
   worse than the bare `sched()` because it now looks handled. Same root as
   `burrow_handle_count() == 0` not meaning "the pages were freed": when
   completion is what you need, make the operation *report* it
   (`burrow_unref_freed`) rather than inferring it from a preceding flag.

2. **Negative asserts need an implication, not a wait.** `test_poll.c`'s #103
   test asserts the IRQ producer did *not* wake the poller, but a peer CPU may
   legitimately dispatch `console_mgr` inside that window. Waiting is
   meaningless; assert an implication with a deliberate read order. Because
   `cons_service_deferred` is the pending flag's sole consumer and always
   walks the hook list, two one-directional forms hold — read the flag first
   for *armed* (`pending || woken`), read the state first for *deferred*
   (`sleeping || !pending`) — while the biconditional is racy in **both**
   orders. The argument rests on sole-consumer-always-completes, not on
   anything general about flags: re-derive it per site, do not port the form.

3. **`cons_test_release_owned_state` — the leaked-global-state backstop
   (#130-R2 F2).** Between the test body and the verdict, `test_run_all`
   releases console/UART state the test left armed and **fails the test that
   left it**. Five states, and the costs are not "one red test":
   `echo-capture` (every later `/dev/cons` write swallowed — silently, since
   kernel diagnostics take `cons_diag_byte` and ignore capture, so the suite
   prints PASS over a dead userspace console), `tx-role` (every later console
   writer parks untimed — the boot hangs), `mgr-hold` (deferred work stops;
   poll wakes strand), `reader-busy` (the single-reader guard refuses every
   later `devcons_read`), and the runner-local `uart-tx-stall`. `TEST_ASSERT`
   is `test_fail(); return;`, so one failing assert inside such a window skips
   the release. Reporting is the point — a silent auto-repair would hide the
   leak it repaired — so the runner prints `LEAKED-STATE(<names>)` and reddens
   the test even when its own assertions passed. **Verified by A/B, because a
   backstop that never fires proves nothing**: with a deliberate failure inside
   the held-role window, *without* the backstop the boot hangs at the very next
   test (`cons.tx_room_wait_and_deadline`, which needs the role) and the suite
   never completes; *with* it the run prints
   `LEAKED-STATE(echo-capture,tx-role)`, finishes 1245/1246, and that next test
   passes.

Note for whoever folds this: (1) and (2) are cross-cutting test *methodology*
and want a home that is not a single file's dossier; (3) splits — the armed
states are `cons.c`'s (already swept, `sub-kernel-cons`) and the backstop is
`kernel/test/test.c`'s (unowned).

---

**If you are here to add something, add it to the dossier, not to this file.**

This stub replaces the whole document, so any edit here becomes a merge
conflict — which is the intended behaviour, and the only reason nothing has
been lost yet. It has now happened TWICE: #125 added a point 7 to the live
document on `main` after absorption, and #130's residue added the three items
above. The #125 content lives in `sub-substrate-interactive.md` (the
guest-suspension mechanism, why `SO_RCVBUF` on the reader is vacuous, the
listener-inheritance fix and its A/B) and in `gate-interactive.md` (the third
thing a silent guest can mean). Nothing from either has been dropped.

That it happened twice is the signal, not the accident: this file is still the
natural place to write about the harness, because the harness has no dossier
yet. The conflict-as-tripwire works, but it fires *after* the writing, and it
only fires for someone merging this branch.
