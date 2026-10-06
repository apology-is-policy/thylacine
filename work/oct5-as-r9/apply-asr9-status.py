#!/usr/bin/env python3
import sys, pathlib
R = pathlib.Path(__file__).resolve().parents[2]
edits = []
def ed(p, old, new, label): edits.append((R / p, old, new, label))

S = 'docs/ASYNC-SERVICE-STATUS.md'

ed(S,
"""## AS-R9: Burrow settlement races the final reference (open)
""",
"""## AS-R9: Burrow settlement races the final reference (repair written, UNRUN)
""",
'status: AS-R9 heading')

ed(S,
"""Continue the approved modal visuals/status work without enabling private async
services or the clipboard endpoint.
""",
"""Continue the approved modal visuals/status work without enabling private async
services or the clipboard endpoint.

**SUPERSEDED October 5.** The operator authorised Corona to implement the
resumed async service lifecycle and production memory accounting, beginning with
AS-R9, and Astra gave the implementation handoff (Yip call 0161). The pause above
is recorded for history; it no longer governs. The preserved owner draft is
still unapplied, and private async, the replacement accounting and the clipboard
all remain gated on their own qualification.

## Corona AS-R9: charge settlement inside the drop (October 5 -- SOURCE COMPLETE, UNRUN)

Corona checkout `/Users/northkillpd/projects/thylacine-corona`, branch
`corona/async-memory`, base `5ff62b78809846af4780ec41f82d1676e7584e80` (verified
equal to Astra's tip; all six of the handoff `base.json` source hashes match).

**Nothing in this section has been built or run.** No gate, no model, no boot, no
host leg. The Mac lease is held by Main for its signal7 landing gates and Corona
is queued behind it. Every claim below is a source-level claim; the verification
row is deliberately empty until the runs happen.

**The defect, widened from the original write-up.** AS-R9 was recorded above as a
pattern "in legacy Loom". It is not confined there: the claim/drop/restore
sequence has **six** instances, five of them in live production paths
independent of the dormant private owner --
`kernel/loom.c:322` (`loom_drop_pin_settling`), `kernel/loom.c:706` (displaced
registered-buffer pins), `kernel/weft.c:388` (share unregister),
`kernel/weft.c:445` (owner orphan sweep), `kernel/vma.c:386` (eager-ANON detach
in `vma_detach_range_in`) -- plus `kernel/syscall.c:7320` (JIT destroy), which is
sound and is treated separately below.

**Severity, measured rather than asserted.** `burrow_free_internal` clobbers
`magic` and returns the slot to SLUB, which its own comment notes does not zero
the slot. So a stale restore lands on a slot that is either still free
(`magic == 0`, so `burrow_charge_restore_in` extincts -- a whole-system kill
reachable from an ordinary pair of concurrent closes, and the dominant outcome),
reissued with no charge recorded (the payer's charge is planted on an unrelated
region; it becomes a wrong refund -- an I-32 under-count -- only if that region
is never `burrow_charge_record`'d, since that call overwrites unconditionally,
and is later settled against the same AddrSpace id: reachable for backings that
take no record, but **not** the likely case), or reissued with a charge (the
`charge_pages != 0` arm extincts with "re-charged mid-settle", a **fabricated**
fault whose own comment asserts the case cannot happen). The write can also race
a concurrent `burrow_create` initialising that slot. Independently, the holder
that actually frees the region reads the momentarily-cleared record and refunds
nothing, so the payer stays charged for pages that no longer exist. `g_vmo_cache`
being Burrow-specific bounds a reissued slot to being a Burrow.

**The repair.** `kernel/burrow.c` + `burrow.h`:
`burrow_charge_claim_locked` (the claim with `v->lock` already held, so there is
one claim implementation rather than two); `burrow_unref_settled_in` /
`burrow_unref_settled`; `burrow_release_mapping_settled_deferred`. The
decrement, the `{0,0}` dual-counter decision and the charge claim run in one hold
of the lock. A non-qualifying drop leaves the record alone, so the holder that
does qualify still finds it; a qualifying drop takes it under the lock, so
settlement is exactly-once. Neither form touches the Burrow after the reference
it dropped is gone, so no caller needs a surviving reference. The refund returns
as a scalar so the caller applies it outside the leaf lock. `payer` is the exact
AddrSpace incarnation; `NULL` settles nothing. The mapping form keeps the
deferred contract and qualifies on `freed || shared_out`, reading `shared_out`
under the same lock -- monotonic false -> true, so a later observation can only
add a reason to settle. `kernel/vma.c` gained `vma_free_settled_deferred`, of
which `vma_free_deferred` is now the no-payer wrapper, so the Vma validation is
not duplicated. All five unsafe callers migrated.

**JIT destroy: proven sound, kept, premise named.** Its restore-path reference is
guaranteed by three facts, now written at the site instead of inherited from a
comment that asserted the conclusion: every failure return in
`burrow_unmap_reporting` precedes that function's first mutation, so a nonzero rc
is no teardown rather than a partial one and leaves its alias attached; the
restore arm runs only when one of the two unmaps failed, so at least one alias
still holds a mapping ref; and both aliases live in `p->as`, whose lock is held
across the whole interval, while refs from any other address space only add to
the counts. Premise one is the fragile half -- a failure return added below the
mutation point would silently make the site a use-after-free write -- so
`burrow.unmap_failure_leaves_mapping_attached` pins it rather than a comment.

**Three false claims deleted, not softened.** `burrow.h`, the Burrow dossier and
the Loom dossier each described this window as benign over-charging, the Loom
dossier calling its failure mode "deliberately chosen". It was neither benign nor
chosen: the descriptions omitted the use-after-free write entirely, and
"never a refund to a Proc that did not pay" is false in the reissued-slot case.
`burrow.h`'s claim/restore contract now states that the API is legal only for a
caller holding an independent reference across the interval, and names JIT as the
only such caller.

**Tests written, none executed.** Native: `burrow.settled_drop_retains_nonfinal_charge`,
`burrow.settled_drop_exact_payer`, `burrow.settled_mapping_drop_defers_free`,
`burrow.unmap_failure_leaves_mapping_attached` (each with a positive control one
variable away where a negative assertion would otherwise be satisfiable by a
broken fixture). `kernel/test/test_addrspace.c`'s async-owner settle -- the case
the exact-payer form exists for, where only an AddrSpace pin names the payer --
now settles through the drop. Host double `work/oct5-as-r9/asr9-fixture.py`, 12
legs: three pre-fix schedules (handle/handle, mapping/handle, handle/mapping),
the reissued-clean and reissued-charged outcomes, a **positive control** that
runs the identical sequence with no racer and requires the restore to complete
(without it an `extinction()` miswired to always exit 42 would "reproduce" the
bug on any input), four repaired schedules, and a shared_out discrimination pair
one variable apart. The pre-fix functions are extracted with
`git show 5ff62b788:kernel/burrow.c` rather than from the working tree, so the
repair cannot launder the premise; `work/oct5-as-r9/verify-verbatim.py` separately
proves the handoff fixture's four functions are byte-identical to shipped source
and prints its denominator so a zero-block run cannot pass as agreement.

**Owed before any of this counts as qualified:** run the host legs; CI build; the
new `burrow.*` tests plus the burrow/vma/weft/loom/capacity/resource/addrspace
suites; intended source mutants with named failing assertions; the burrow and
capacity models; and the audit round. The `kernel/burrow.c` + `burrow.h` row in
`docs/AUDIT-TRIGGERS.md` (VMO / BURROW) is triggered by this change.

**Dossiers co-staged:** `sub-kernel-burrow` (the AS-R9 section and the corrected
contract), `sub-kernel-vma`, `sub-kernel-loom`, `sub-kernel-weft`. `quaestor lint`
reports 0 failures; its 2 warnings (`sub-kernel-loom-pools` section order, 47
stale dossiers) are pre-existing and not introduced here.
""",
'status: supersede the park, add the Corona AS-R9 section')

texts, fail = {}, False
for path, old, new, label in edits:
    t = texts.get(path)
    if t is None: t = texts[path] = path.read_text()
    n = t.count(old)
    if n != 1:
        print(f'ABORT [{label}]: anchor occurs {n} times in {path.name}, expected 1'); fail = True
    else:
        texts[path] = t.replace(old, new, 1); print(f'  ok  [{label}]')
if fail:
    print('NOTHING WRITTEN'); sys.exit(1)
for path, t in texts.items():
    path.write_text(t); print(f'wrote {path.relative_to(R)}')
