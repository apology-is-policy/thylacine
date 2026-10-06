#!/usr/bin/env python3
"""AS-R9 repair: fold charge settlement into the drop's lock interval.

Exact-byte anchored. Builds every edit in memory and asserts each anchor occurs
EXACTLY once BEFORE any file is written -- a partial application would leave the
tree in a state neither reviewable nor revertable.
"""
import sys, pathlib
R = pathlib.Path(__file__).resolve().parents[2]
edits = []   # (path, old, new, label)

def ed(p, old, new, label):
    edits.append((R / p, old, new, label))

# ---------------------------------------------------------------- burrow.h
ed('kernel/include/thylacine/burrow.h',
"""// burrow_charge_restore: put back a claim the caller decided NOT to settle.
// Callers claim BEFORE the drop (the record dies with the Burrow, so it cannot
// be read after) and restore when the drop turns out not to end the payer's
// involvement. A concurrent settler that saw the momentarily-cleared record
// simply skips -- so the failure mode of that window is a charge that outlives
// its region until the payer's next release point (benign: an over-charge on
// the payer, never a refund to a Proc that did not pay).
""",
"""// burrow_charge_restore: put back a claim the caller decided NOT to settle.
// LEGAL ONLY for a caller that provably holds an INDEPENDENT reference across
// the whole claim-drop-restore interval -- one the drop it just made cannot have
// been the last of. SYS_JIT_DESTROY is the only such caller: it holds as->lock
// for the interval, both of the region's aliases live in that one address space,
// and every failure return in burrow_unmap_reporting precedes that function's
// first mutation, so a nonzero rc leaves its alias attached. Any other caller
// settles through the burrow_*_settled drops below.
//
// AS-R9: the claim-drop-restore sequence is NOT safe in general, and an earlier
// version of this comment wrongly called its window benign. Between a nonfinal
// drop and the restore, another holder can make the final drop and free the
// descriptor; the restore then writes charge_as_id/charge_pages through a
// pointer whose last reference is gone. That is a use-after-free write, and if
// the storage is recycled into a live Burrow the charge is planted on an
// UNRELATED region -- a later claim then refunds pages the payer never bought
// for it, an UNDER-count, the direction that inflates a budget and breaks I-32.
// The converse loss is just as real: the racing final holder reads the
// momentarily-cleared record, claims nothing, and refunds nothing at all.
""",
'burrow.h: correct the false "benign window" contract')

ed('kernel/include/thylacine/burrow.h',
"""u32 burrow_charge_claim_in(struct Burrow *v, const struct AddrSpace *as);
void burrow_charge_restore_in(struct Burrow *v, const struct AddrSpace *as, u32 pages);
""",
"""u32 burrow_charge_claim_in(struct Burrow *v, const struct AddrSpace *as);
void burrow_charge_restore_in(struct Burrow *v, const struct AddrSpace *as, u32 pages);

// AS-R9: the settled drops -- a drop whose charge decision happens INSIDE the
// same v->lock interval that decides finality, which is what makes the two
// atomic with respect to each other. There is no window to lose: a drop that
// does not qualify leaves the record untouched (so the holder that does qualify
// still finds it), and a drop that does qualify takes the record with it under
// the lock (so it is settled exactly once). Neither form touches `v` after the
// reference it dropped is gone, so no caller needs a surviving reference.
//
// `payer` is the EXACT AddrSpace incarnation that paid; NULL means "settle
// nothing" and is how a caller whose policy predicate fails opts out. The
// returned refund is nonzero only on the drop that qualified, so a caller can
// refund unconditionally on a nonzero scalar. Refund OUTSIDE the leaf lock --
// these return a scalar precisely so the caller can.
//
// burrow_unref_settled*: handle drop. Qualifies iff this drop frees the region.
// burrow_release_mapping_settled_deferred: mapping drop, DEFERRED like its
// unsettled twin -- returns the now-dead Burrow without freeing it, for a
// caller holding as->lock to free after the unlock. Qualifies iff this drop
// frees the region OR the region is shared out (the sharer's own detach must
// settle then, because the surviving foreign mapping cannot name the payer);
// shared_out is observed under that same lock, and being monotonic false->true
// a later observation can only ADD a reason to settle.
bool burrow_unref_settled_in(struct Burrow *v, const struct AddrSpace *payer,
                             u32 *out_refund);
bool burrow_unref_settled(struct Burrow *v, const struct Proc *payer,
                          u32 *out_refund);
struct Burrow *burrow_release_mapping_settled_deferred(struct Burrow *v,
                                                       const struct AddrSpace *payer,
                                                       u32 *out_refund);
""",
'burrow.h: declare the settled drops')

# ---------------------------------------------------------------- burrow.c
ed('kernel/burrow.c',
"""u32 burrow_charge_claim_in(struct Burrow *v, const struct AddrSpace *as) {
    if (!v || !as) return 0;
    if (v->magic != VMO_MAGIC)
        extinction("burrow_charge_claim on corrupted BURROW (use-after-free?)");
    u32 pages = 0;
    spin_lock(&v->lock);
    // charge_pages""",
"""// The claim itself, with v->lock ALREADY held -- the settled drops below need
// it inside the critical section that decides finality, which is the whole
// point of AS-R9; burrow_charge_claim_in is the same operation taking the lock
// for callers that only want the claim.
static u32 burrow_charge_claim_locked(struct Burrow *v, const struct AddrSpace *as) {
    if (!as) return 0;
    // charge_pages""",
'burrow.c: hoist the claim into a lock-held helper (part 1)')

ed('kernel/burrow.c',
"""    if (v->charge_pages != 0 && v->charge_as_id == as->id) {
        pages           = v->charge_pages;
        v->charge_as_id = 0;
        v->charge_pages = 0;
    }
    spin_unlock(&v->lock);
    return pages;
}
""",
"""    if (v->charge_pages != 0 && v->charge_as_id == as->id) {
        u32 pages       = v->charge_pages;
        v->charge_as_id = 0;
        v->charge_pages = 0;
        return pages;
    }
    return 0;
}

u32 burrow_charge_claim_in(struct Burrow *v, const struct AddrSpace *as) {
    if (!v || !as) return 0;
    if (v->magic != VMO_MAGIC)
        extinction("burrow_charge_claim on corrupted BURROW (use-after-free?)");
    spin_lock(&v->lock);
    u32 pages = burrow_charge_claim_locked(v, as);
    spin_unlock(&v->lock);
    return pages;
}

// AS-R9: the settled handle drop. The decrement, the dual-counter free decision
// and the charge claim all run in ONE hold of v->lock, so no other holder can
// interleave between "this drop was not the last" and the settlement of the
// record -- the window that made the old claim-drop-restore sequence a
// use-after-free write (see the contract in burrow.h). The free runs outside
// the lock, leaf discipline as everywhere else, and `v` is not touched after it.
bool burrow_unref_settled_in(struct Burrow *v, const struct AddrSpace *payer,
                             u32 *out_refund) {
    if (out_refund) *out_refund = 0;
    if (!v) return false;                      // NULL-safe, mirroring burrow_unref_freed
    if (v->magic != VMO_MAGIC)
        extinction("burrow_unref_settled of corrupted BURROW (use-after-free?)");
    spin_lock(&v->lock);
    if (v->handle_count <= 0) {
        spin_unlock(&v->lock);
        extinction("burrow_unref_settled of zero-ref BURROW");
    }
    v->handle_count--;
    bool should_free = (v->handle_count == 0 && v->mapping_count == 0);
    // Claim ONLY on the drop that ends the occupancy. A nonfinal drop leaves the
    // record in place for whoever does end it -- the old code cleared it here
    // and put it back afterwards, which is exactly how a racing final holder
    // came to read an empty record and refund nothing.
    u32 refund = should_free ? burrow_charge_claim_locked(v, payer) : 0;
    spin_unlock(&v->lock);

    if (should_free)
        burrow_free_internal(v);
    if (out_refund) *out_refund = refund;
    return should_free;
}

bool burrow_unref_settled(struct Burrow *v, const struct Proc *payer, u32 *out_refund) {
    return burrow_unref_settled_in(v, payer ? payer->as : NULL, out_refund);
}

// AS-R9: the settled mapping drop, deferred. The twin of
// burrow_release_mapping_deferred -- same {0,0} decision, same "return the dead
// Burrow rather than free it under the caller's as->lock" contract -- with the
// charge decision folded into the same critical section.
struct Burrow *burrow_release_mapping_settled_deferred(struct Burrow *v,
                                                       const struct AddrSpace *payer,
                                                       u32 *out_refund) {
    if (out_refund) *out_refund = 0;
    if (!v)                       extinction("burrow_release_mapping_settled(NULL)");
    if (v->magic != VMO_MAGIC)
        extinction("burrow_release_mapping_settled of corrupted BURROW (use-after-free?)");
    spin_lock(&v->lock);
    if (v->mapping_count <= 0) {
        spin_unlock(&v->lock);
        extinction("burrow_release_mapping_settled of zero-mapping BURROW");
    }
    v->mapping_count--;
    bool should_free = (v->handle_count == 0 && v->mapping_count == 0);
    // A shared-out region settles on the sharer's detach even when the drop does
    // not free it: the mapping that survives is in a Proc that cannot name the
    // payer, so there is no later settler. Read under this same lock -- monotonic
    // false -> true, so observing it here rather than before the drop can only
    // ADD a reason to settle, never miss one that mattered.
    bool shared_out = v->shared_out;
    u32  refund = (should_free || shared_out) ? burrow_charge_claim_locked(v, payer) : 0;
    spin_unlock(&v->lock);

    if (out_refund) *out_refund = refund;
    return should_free ? v : NULL;
}
""",
'burrow.c: lock-held claim + the two settled drops')

for p, old, new, label in edits:
    pass

# ----------------------------------------------------------- validate, then write
texts, fail = {}, False
for path, old, new, label in edits:
    t = texts.get(path)
    if t is None:
        t = texts[path] = path.read_text()
    n = t.count(old)
    if n != 1:
        print(f'ABORT [{label}]: anchor occurs {n} times in {path.name}, expected exactly 1')
        fail = True
    else:
        texts[path] = t.replace(old, new, 1)
        print(f'  ok  [{label}]')
if fail:
    print('NOTHING WRITTEN -- fix the anchors')
    sys.exit(1)
for path, t in texts.items():
    path.write_text(t)
    print(f'wrote {path.relative_to(R)}')
