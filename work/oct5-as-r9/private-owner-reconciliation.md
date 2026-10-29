# Private-owner draft: reconciliation against the repaired base

Astra closed the scoped AS-R9 review at ba0c8f60f and with it the prerequisite to
INSPECT AND RECONCILE the preserved private-owner draft (yip 0161 turn 27). This
is the concrete delta she asked for, before anything is applied and before any
activation. Nothing here is applied to source; the draft was read read-only from
her checkout and her tree was not modified.

Draft: `thylacine-astra/work/oct4-async-service/owner-integration/paused-owner/`
Its base: `c822021a2`, which is an ancestor of my base `5ff62b788` by one commit.

## 1. The pin is intact -- and it does not pin what I first assumed

All 9 paths in `pin.json` match sha256 **the draft's own copies**, 9/9. They do
NOT match the same paths at `c822021a2`, and my first control assumed they would
-- which would have reported "the draft's pin is broken" on 8 mismatches and one
file missing at base. The missing file (`kernel/test/loom_private_fixture.h`) is
simply NEW, which is the tell I should have read first. `pin.json` is a
self-integrity manifest for the draft, recording `head` as the base it was
authored against. Establish what a manifest pins before calling a mismatch a
defect: a fabricated defect outranks a missed one.

## 2. The reconciliation surface is ONE file

Of the 9 paths, exactly one moved between `c822021a2` and my HEAD:

    kernel/loom.c   +16 -17
    the other 8     unchanged

And it moved from MY AS-R9 work alone -- Astra's intervening commit does not
touch `loom.c` at all (empty numstat). So no third party is in this seam.

## 3. No line-level collision

The draft's loom.c hunks sit at base lines 16, 191-273 (including a +131-line
insertion), 563, 656, 1806, 2388, 2593. Mine sit at 316-327 and 706-708. They are
disjoint, so a rebase does not conflict textually. That is NOT the same as
semantic independence, which is why the next section exists.

## 4. THE FINDING: the draft's retirement path writes the AS-R9 defect shape anew

`loom_private_destroy` in the draft contains:

    u32 paid = burrow_charge_claim_in(l->ring, as);
    bool freed = burrow_unref_freed(l->ring);
    if (!freed) burrow_charge_restore_in(l->ring, as, paid);
    ...
    addrspace_uncharge_pages(as, metadata + (freed ? paid : 0));

That is claim / drop / restore -- precisely the shape AS-R9 removed, in which the
Burrow's lock covers each of the three operations and none of the gaps, so
another holder's FINAL drop landing between the non-final drop and the restore
makes the restore write through a freed descriptor.

It is NEW, not inherited: the site is absent from `c822021a2:kernel/loom.c` AND
absent from my repaired HEAD. The draft's other two occurrences (its lines
458-462 and 845-849) ARE the inherited pre-repair forms of the two sites my
branch already migrated to `burrow_unref_settled` at loom.c:328 and :709 -- a
rebase onto the repaired base resolves those two and leaves THIS one defective.
So the naive reconciliation produces a tree where AS-R9 is fixed everywhere
except the newest path, which is the worst of the available outcomes: the repair
present, its lesson not.

**Reachability is a separate question from shape, and I am not claiming the
stronger one.** The AS-R9 window needs two holders of one Burrow. This path is a
private ring's retirement, and the draft guards it with `extinction` on
`sqpoll`, `n_reg_buf`, `inflight_ops` and any retained fid, which may exclude a
second holder by construction. That argument has NOT been verified and it is
exactly the "premise true for a reason nobody wrote down" class this project has
been bitten by. The settled form costs nothing here, so the obligation below does
not depend on resolving the reachability question.

## 5. Obligations before activation

1. Convert `loom_private_destroy`'s charge settlement to the settled form, so the
   decision is folded into the same `v->lock` interval that decides finality --
   the same migration the five original callers got. Do not rely on the
   exclusivity guards to make claim/restore safe.
2. Rebase the draft's other two loom.c sites onto the repaired forms rather than
   re-applying the draft's inherited versions (a textual rebase will not do this
   for you: the hunks are disjoint, so git will happily keep both).
3. The 8 unchanged paths can be taken as authored, subject to their own review.
4. `addrspace_uncharge_pages(as, metadata + (freed ? paid : 0))` reads `paid`
   after the drop; re-derive it from the settled refund instead, so one value is
   not sourced from a pre-drop claim and consumed post-drop.

## 6. Qualification plan

This is a lifetime-and-charge path, so the host double is not sufficient and a
single boot is not either:
- the four burrow witnesses plus whatever the draft adds, on the gate image;
- `tools/ci-smp-gate.sh` -- it is an I-32 settlement path reached at retirement,
  so it needs the multi-boot matrix for the same reason AS-R9 did, not one green
  boot;
- a settled-drop witness for the retirement path specifically, red before the
  conversion and green after, since a regression here would otherwise look
  exactly like a clean retirement;
- the Pi A72/KVM leg remains the named unrun residual and this does not change
  that: qualifying a second charge-settlement path on one memory model inherits
  the same one-axis caveat.

Activation stays gated: 128 MiB protection retained, private async, replacement
memory accounting and clipboard all non-default until their own gates pass.
