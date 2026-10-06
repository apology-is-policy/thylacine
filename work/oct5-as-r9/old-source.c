bool burrow_unref_freed(struct Burrow *v) {
    if (!v) return false;                      // NULL-safe
    if (v->magic != VMO_MAGIC)
        extinction("burrow_unref of corrupted BURROW (use-after-free?)");
    // #847: decrement + the dual-counter free decision under v->lock; the
    // free runs OUTSIDE the lock (leaf discipline -- see file header).
    spin_lock(&v->lock);
    if (v->handle_count <= 0) {
        spin_unlock(&v->lock);
        extinction("burrow_unref of zero-ref BURROW");
    }
    v->handle_count--;
    // Dual-check: free only when BOTH counts reach 0. Maps to the spec's
    // NoUseAfterFree iff invariant — premature free violates (counts > 0 ∧
    // pages dead); delayed free violates (counts = 0 ∧ pages alive). Exactly
    // one racing unref/release_mapping sees the 0,0 edge.
    bool should_free = (v->handle_count == 0 && v->mapping_count == 0);
    spin_unlock(&v->lock);

    if (should_free)
        burrow_free_internal(v);
    return should_free;
}

struct Burrow *burrow_release_mapping_deferred(struct Burrow *v) {
    if (!v)                       extinction("burrow_release_mapping(NULL)");
    if (v->magic != VMO_MAGIC)
        extinction("burrow_release_mapping of corrupted BURROW (use-after-free?)");
    spin_lock(&v->lock);
    if (v->mapping_count <= 0) {
        spin_unlock(&v->lock);
        extinction("burrow_release_mapping of zero-mapping BURROW");
    }
    v->mapping_count--;
    bool should_free = (v->handle_count == 0 && v->mapping_count == 0);
    spin_unlock(&v->lock);
    return should_free ? v : NULL;
}

u32 burrow_charge_claim_in(struct Burrow *v, const struct AddrSpace *as) {
    if (!v || !as) return 0;
    if (v->magic != VMO_MAGIC)
        extinction("burrow_charge_claim on corrupted BURROW (use-after-free?)");
    u32 pages = 0;
    spin_lock(&v->lock);
    // charge_pages -- not charge_as_id -- is the "held" sentinel: a charge of
    // zero pages is meaningless, so zero pages IS "nothing held". The key is
    // the ADDRESS SPACE that paid (B-1a' audit F4): a pid survives exec, and a
    // handle that outlives the outgoing space (a non-CLOEXEC Loom) would
    // otherwise refund against the successor's space, which never paid.
    // A kernel pin can retain the exact payer after its final Proc exits;
    // callers then use this AddrSpace-keyed form without a dead Proc pointer.
    // A record whose descriptor really died is never claimed: its counter died
    // too, and the physical pages return to the pool when storage is freed.
    if (v->charge_pages != 0 && v->charge_as_id == as->id) {
        pages           = v->charge_pages;
        v->charge_as_id = 0;
        v->charge_pages = 0;
    }
    spin_unlock(&v->lock);
    return pages;
}

void burrow_charge_restore_in(struct Burrow *v, const struct AddrSpace *as, u32 pages) {
    if (!v || !as || pages == 0) return;
    if (v->magic != VMO_MAGIC)
        extinction("burrow_charge_restore on corrupted BURROW (use-after-free?)");
    spin_lock(&v->lock);
    // The record must still be the one WE cleared. A held record here means
    // something re-charged the region between the claim and the restore, which
    // cannot happen: burrow_charge_record runs once, at creation, on a Burrow no
    // other path has a reference to yet. Extinct rather than pick a loser --
    // silently refusing would DROP the caller's `pages` on the floor, and a lost
    // charge is an under-count, the direction that inflates a Proc's budget.
    if (v->charge_pages != 0) {
        spin_unlock(&v->lock);
        extinction("burrow_charge_restore: the region was re-charged mid-settle");
    }
    v->charge_as_id = as->id;
    v->charge_pages = pages;
    spin_unlock(&v->lock);
}