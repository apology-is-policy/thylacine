#!/usr/bin/env python3
"""AS-R9 caller migration. Same abort-before-write discipline as apply-asr9.py."""
import sys, pathlib
R = pathlib.Path(__file__).resolve().parents[2]
edits = []
def ed(p, old, new, label): edits.append((R / p, old, new, label))

# ------------------------------------------------------------------- vma.h
ed('kernel/include/thylacine/vma.h',
"""struct Burrow *vma_free_deferred(struct Vma *v, bool *out_freed);
""",
"""struct Burrow *vma_free_deferred(struct Vma *v, bool *out_freed);

// AS-R9: the settled form -- frees the Vma and drops its mapping ref through
// burrow_release_mapping_settled_deferred, so the payer's I-32 charge is settled
// in the SAME lock interval that decides whether the drop freed the region.
// `payer` is the exact AddrSpace incarnation that paid, or NULL to settle
// nothing; *out_refund is the pages to refund OUTSIDE as->lock, nonzero only
// when this drop qualified. vma_free_deferred is this with no payer.
struct Burrow *vma_free_settled_deferred(struct Vma *v, const struct AddrSpace *payer,
                                         bool *out_freed, u32 *out_refund);
""",
'vma.h: declare vma_free_settled_deferred')

# ------------------------------------------------------------------- vma.c
ed('kernel/vma.c',
"""struct Burrow *vma_free_deferred(struct Vma *v, bool *out_freed) {
    if (out_freed) *out_freed = false;
    if (!v)                     extinction("vma_free(NULL)");
    if (v->magic != VMA_MAGIC)  extinction("vma_free of corrupted/already-freed Vma");
    if (v->next || v->prev)     extinction("vma_free of Vma still in a list");

    struct Burrow *to_free = NULL;
    if (v->burrow) {
        to_free = burrow_release_mapping_deferred(v->burrow);
        if (out_freed) *out_freed = (to_free != NULL);
        v->burrow = NULL;
    }
""",
"""struct Burrow *vma_free_settled_deferred(struct Vma *v, const struct AddrSpace *payer,
                                         bool *out_freed, u32 *out_refund) {
    if (out_freed)  *out_freed  = false;
    if (out_refund) *out_refund = 0;
    if (!v)                     extinction("vma_free(NULL)");
    if (v->magic != VMA_MAGIC)  extinction("vma_free of corrupted/already-freed Vma");
    if (v->next || v->prev)     extinction("vma_free of Vma still in a list");

    struct Burrow *to_free = NULL;
    if (v->burrow) {
        to_free = burrow_release_mapping_settled_deferred(v->burrow, payer, out_refund);
        if (out_freed) *out_freed = (to_free != NULL);
        v->burrow = NULL;
    }
""",
'vma.c: rename body to the settled form')

ed('kernel/vma.c',
"""    kmem_cache_free(g_vma_cache, v);
    __atomic_fetch_add(&g_vma_freed, 1u, __ATOMIC_RELAXED);
    return to_free;
}
""",
"""    kmem_cache_free(g_vma_cache, v);
    __atomic_fetch_add(&g_vma_freed, 1u, __ATOMIC_RELAXED);
    return to_free;
}

struct Burrow *vma_free_deferred(struct Vma *v, bool *out_freed) {
    return vma_free_settled_deferred(v, NULL, out_freed, NULL);
}
""",
'vma.c: vma_free_deferred becomes the no-payer wrapper')

ed('kernel/vma.c',
"""            // The eager-ANON refund (#130/#131): the charge RECORD says who
            // paid, never the region's shape. Claimed BEFORE the drop (a
            // freeing drop takes the record with it) and refunded iff the drop
            // actually freed the pages -- a Loom ring, a registered buffer and
            // a Weft share each hold a handle_count ref that can outlive the
            // mapping -- or the region survives only in ANOTHER Proc, which
            // this one can no longer reach and whose eventual last drop has no
            // way to name the payer. Alive on one of this Proc's OWN claims,
            // the claim is put back for that claim's drop to settle.
            bool shared_out = false;
            u32  paid       = 0;
            if (payer && b && !shared_in && b->type == BURROW_TYPE_ANON) {
                shared_out = burrow_is_shared_out(b);
                paid       = burrow_charge_claim(b, payer);
            }
            vma_remove_in(as, v);
            bool freed = false;
            struct Burrow *tf = vma_free_deferred(v, &freed);
            if (paid) {
                if (freed || shared_out) addrspace_uncharge_pages(as, paid);
                else                     burrow_charge_restore(b, payer, paid);
            }
            if (tf) { tf->deferred_free_next = dead; dead = tf; }
""",
"""            // The eager-ANON refund (#130/#131): the charge RECORD says who
            // paid, never the region's shape. Refunded iff this drop actually
            // freed the pages -- a Loom ring, a registered buffer and a Weft
            // share each hold a handle_count ref that can outlive the mapping --
            // or the region survives only in ANOTHER Proc, which this one can no
            // longer reach and whose eventual last drop has no way to name the
            // payer. Alive on one of this Proc's OWN claims, the record is left
            // in place for that claim's drop to settle.
            //
            // AS-R9: the decision is the DROP's, taken under the Burrow's lock
            // (burrow_release_mapping_settled_deferred), not this function's
            // across three separate acquisitions. `b` is therefore never
            // dereferenced after this mapping's ref is gone -- which is what the
            // old claim/restore pair did, and a concurrent final drop by any
            // other holder made that a use-after-free write.
            const struct AddrSpace *settle_as =
                (payer && b && !shared_in && b->type == BURROW_TYPE_ANON) ? payer->as : NULL;
            vma_remove_in(as, v);
            u32 paid = 0;
            struct Burrow *tf = vma_free_settled_deferred(v, settle_as, NULL, &paid);
            // Outside the Burrow leaf lock, as the ledger requires.
            if (paid) addrspace_uncharge_pages(as, paid);
            if (tf) { tf->deferred_free_next = dead; dead = tf; }
""",
'vma.c: detach settles inside the drop')

# ------------------------------------------------------------------ loom.c
ed('kernel/loom.c',
"""// Settle one of the Loom's Burrow pins: claim whatever charge the owner holds
// on it, drop the pin, and refund only if this drop ended the occupancy. The
// claim returns 0 for a region the owner never paid for, which is what keeps
// a shared-in Weft ring from being refunded to its consumer.
static void loom_drop_pin_settling(struct Loom *l, struct Burrow *b) {
    struct Proc *o  = loom_owner_live(l);
    u32          paid = o ? burrow_charge_claim(b, o) : 0;
    if (burrow_unref_freed(b)) {
        if (paid) proc_page_uncharge(o, paid);
    } else if (paid) {
        burrow_charge_restore(b, o, paid);   // !freed => b is still live
    }
}
""",
"""// Settle one of the Loom's Burrow pins: drop the pin and settle the owner's
// charge in the SAME lock interval, refunding only if this drop ended the
// occupancy. The claim returns 0 for a region the owner never paid for, which
// is what keeps a shared-in Weft ring from being refunded to its consumer.
//
// AS-R9: this used to claim, drop, then restore on a nonfinal drop. Another
// holder's final drop could land in that window, so the restore wrote through a
// freed descriptor and the record was missing when the holder that DID free the
// region looked for it. A nonzero refund now comes only from the freeing drop.
static void loom_drop_pin_settling(struct Loom *l, struct Burrow *b) {
    struct Proc *o = loom_owner_live(l);
    u32 refund = 0;
    (void)burrow_unref_settled(b, o, &refund);
    if (refund) proc_page_uncharge(o, refund);
}
""",
'loom.c: loom_drop_pin_settling')

ed('kernel/loom.c',
"""            u32 paid = burrow_charge_claim(old[i], p);
            if (burrow_unref_freed(old[i])) {
                if (paid) proc_page_uncharge(p, paid);
            } else if (paid) {
                burrow_charge_restore(old[i], p, paid);   // !freed => still live
            }
""",
"""            u32 refund = 0;   // AS-R9: settled under the Burrow's own lock
            (void)burrow_unref_settled(old[i], p, &refund);
            if (refund) proc_page_uncharge(p, refund);
""",
'loom.c: displaced registered-buffer pins')

# ------------------------------------------------------------------ weft.c
ed('kernel/weft.c',
"""    // maps the region and its own detach will settle instead (that detach sees
    // freed == true once this pin is gone), so put the claim back.
    // Removal-before-free (R2-F5) is unchanged: the entry is already unlinked.
    u32 paid = burrow_charge_claim(victim, owner);
    if (burrow_unref_freed(victim)) {
        if (paid) proc_page_uncharge(owner, paid);
    } else if (paid) {
        burrow_charge_restore(victim, owner, paid);
    }
    return 0;
""",
"""    // maps the region and its own detach will settle instead (that detach sees
    // freed == true once this pin is gone), so the record is left in place for it.
    // Removal-before-free (R2-F5) is unchanged: the entry is already unlinked.
    // AS-R9: the drop decides and settles under one hold of the Burrow's lock,
    // so a concurrent final drop by the sharer's own detach cannot strand the
    // record or leave this path writing through freed storage.
    u32 refund = 0;
    (void)burrow_unref_settled(victim, owner, &refund);
    if (refund) proc_page_uncharge(owner, refund);
    return 0;
""",
'weft.c: explicit share unregister')

ed('kernel/weft.c',
"""    for (u32 i = 0; i < n; i++) {
        u32 paid = burrow_charge_claim(orphans[i], owner);
        if (burrow_unref_freed(orphans[i])) {
            if (paid) proc_page_uncharge(owner, paid);
        } else if (paid) {
            burrow_charge_restore(orphans[i], owner, paid);
        }
    }
""",
"""    for (u32 i = 0; i < n; i++) {
        u32 refund = 0;   // AS-R9: settled under the Burrow's own lock
        (void)burrow_unref_settled(orphans[i], owner, &refund);
        if (refund) proc_page_uncharge(owner, refund);
    }
""",
'weft.c: owner orphan sweep')

# --------------------------------------------------------------- syscall.c
ed('kernel/syscall.c',
"""    } else if (paid) {
        // Neither alias was fully torn down, so the region -- and the charge
        // that belongs to it -- survives. Put the claim back for the retry or
        // for exit to settle. `wb` is still live: a partial teardown by
        // definition left a mapping holding it.
        burrow_charge_restore(wb, p, paid);
    }
""",
"""    } else if (paid) {
        // Neither alias was fully torn down, so the region -- and the charge
        // that belongs to it -- survives. Put the claim back for the retry or
        // for exit to settle.
        //
        // AS-R9: claim/restore is a use-after-free write for any caller that can
        // lose its last reference inside the window, and the five other callers
        // were migrated to the settled drops for exactly that reason. THIS one
        // is sound, and the premise is worth naming rather than inheriting:
        //   1. every failure return in burrow_unmap_reporting precedes that
        //      function's first mutation, so a nonzero rc leaves its alias
        //      attached -- it is not a partial teardown but no teardown;
        //   2. this arm runs only when rc_x or rc_w is nonzero, so at least one
        //      of the two aliases still holds a mapping ref on `wb`;
        //   3. both aliases live in p->as, whose lock is held across the whole
        //      claim/unmap/restore interval, so no concurrent unmap of either
        //      can run; refs held by any other address space only ADD to the
        //      counts and can never drive them to {0,0} while (2) holds.
        // Premise 1 is the fragile one: a failure return added BELOW the
        // mutation point in burrow_unmap_reporting would silently make this a
        // UAF. burrow.unmap_failure_leaves_mapping_attached pins it.
        burrow_charge_restore(wb, p, paid);
    }
""",
'syscall.c: name the JIT remaining-reference premise')

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
    print('NOTHING WRITTEN -- fix the anchors'); sys.exit(1)
for path, t in texts.items():
    path.write_text(t); print(f'wrote {path.relative_to(R)}')
