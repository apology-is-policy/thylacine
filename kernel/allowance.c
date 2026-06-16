// Per-Proc hardware allowance -- the Menagerie driver-authority lift
// (ARCH section 28 invariant I-34; docs/MENAGERIE.md section 4;
// specs/allowance.tla). See <thylacine/allowance.h> for the model.
//
// The kernel enforces three of the four I-34 legs structurally:
//   HandlesWithinAllowance   -- allowance_permits (CreateBegin) + allowance_
//                               handle_alloc (CreateCommit's under-lock
//                               re-check) gate every hw-handle create.
//   AllowanceWithinConferred -- the conferred set is IMMUTABLE after confer
//                               (no path mutates mmio[]/irq[]/dma_max).
//   RevokedFullyCleared      -- proc_revoke_allowance + the caller's
//                               proc_group_terminate (handle sweep at reap).
// The fourth leg, ConferredWithinNode (the grant is a subset of the bound
// node), is the warden's grant policy (MENAGERIE section 11) -- the kernel
// copies whatever the warden confers; the warden computes node INTERSECT
// manifest.

#include <thylacine/allowance.h>
#include <thylacine/proc.h>
#include <thylacine/handle.h>
#include "../mm/slub.h"

// CreateBegin (specs/allowance.tla): the allowance gate. Lock-free fast read
// of the immutable conferred set + the revoked flag (ACQUIRE). A NULL
// allowance is BROAD -- true, subject to the caller's CAP_HW_CREATE gate + the
// kobj_*_create I-5 reservation (the as-built v1.0 path).
bool allowance_permits(struct Proc *p, enum hw_res_kind kind, u64 a, u64 b) {
    if (!p) return false;
    // ACQUIRE-load the allowance pointer (audit F4): pairs with the RELEASE
    // publish in proc_confer_allowance so the conferred-set writes are visible
    // before any gate read, independent of the warden's spawn-ordering.
    struct Allowance *al = __atomic_load_n(&p->allowance, __ATOMIC_ACQUIRE);
    if (!al) return true;   // BROAD

    // A revoked allowance permits nothing (the spec's allowance[d] = {}).
    if (__atomic_load_n(&al->revoked, __ATOMIC_ACQUIRE)) return false;

    switch (kind) {
    case HW_RES_MMIO: {
        u64 base = a, size = b;
        if (size == 0) return false;
        if (base > ~(u64)0 - size) return false;        // [base, base+size) overflow
        u64 end = base + size;
        for (u32 i = 0; i < al->mmio_count && i < ALLOWANCE_MMIO_MAX; i++) {
            u64 wb = al->mmio[i].base, ws = al->mmio[i].size;
            if (ws == 0) continue;
            if (wb > ~(u64)0 - ws) continue;            // a corrupt window can't widen
            if (base >= wb && end <= wb + ws) return true;
        }
        return false;
    }
    case HW_RES_IRQ: {
        u32 intid = (u32)a;
        for (u32 i = 0; i < al->irq_count && i < ALLOWANCE_IRQ_MAX; i++)
            if (al->irq[i] == intid) return true;
        return false;
    }
    case HW_RES_DMA: {
        u64 size = a;
        return (size > 0 && size <= al->dma_max);
    }
    case HW_RES_PCI: {
        u32 bdf = (u32)a;
        for (u32 i = 0; i < al->pci_count && i < ALLOWANCE_PCI_MAX; i++)
            if (al->pci[i] == bdf) return true;
        return false;
    }
    }
    return false;   // unknown kind -> fail closed
}

// The "drivers are leaves" gate (MENAGERIE section 13.2): rfork_internal denies
// a NARROWED driver a child Proc, so no hw-capable grandchild can inherit a
// clone of the allowance that survives the per-Proc revoke + thread-group-scoped
// DeviceRemoved terminate (5e-4 F2). True iff p carries a narrowed allowance.
// SYS_PCI_CLAIM no longer uses this -- since build-arc step 6 it gates on the
// per-(bus,dev,fn) PCI axis (HW_RES_PCI). The allowance pointer is read ACQUIRE
// (uniform with every other p->allowance reader).
bool allowance_is_narrowed(struct Proc *p) {
    return p && __atomic_load_n(&p->allowance, __ATOMIC_ACQUIRE) != NULL;
}

// CreateCommit (specs/allowance.tla): install the hw handle, re-checking the
// allowance UNDER the lock proc_revoke_allowance takes so a concurrent
// DeviceRemoved revocation is observed. handle_alloc is spinlock-only (never
// sleeps), so holding al->lock across it is sound; lock order al->lock ->
// handle-table lock (nothing acquires the handle-table lock then this -> the
// order is acyclic). A NULL allowance bypasses the re-check (broad).
hidx_t allowance_handle_alloc(struct Proc *p, enum kobj_kind kind,
                              rights_t rights, void *obj) {
    if (!p) return -1;
    struct Allowance *al = __atomic_load_n(&p->allowance, __ATOMIC_ACQUIRE);
    if (!al) return handle_alloc(p, kind, rights, obj);   // BROAD

    spin_lock(&al->lock);
    if (__atomic_load_n(&al->revoked, __ATOMIC_ACQUIRE)) {
        spin_unlock(&al->lock);
        return -1;                                        // revoke won the race -> abort
    }
    hidx_t h = handle_alloc(p, kind, rights, obj);
    spin_unlock(&al->lock);
    return h;
}

// Confer (specs/allowance.tla): the warden narrows a freshly-spawned driver.
// SET-ONCE at spawn -- the caller (the warden's spawn path) guarantees no
// concurrent reader (the driver has not yet entered EL0), so the swap +
// free-old is race-free. mmio[]/irq[]/dma_max become immutable here.
int proc_confer_allowance(struct Proc *p,
                          const struct hw_window *mmio, u32 mmio_count,
                          const u32 *irq, u32 irq_count, u64 dma_max,
                          const u32 *pci, u32 pci_count) {
    if (!p) return -1;
    if (mmio_count > ALLOWANCE_MMIO_MAX) return -1;
    if (irq_count > ALLOWANCE_IRQ_MAX)   return -1;
    if (pci_count > ALLOWANCE_PCI_MAX)   return -1;
    if (mmio_count > 0 && !mmio) return -1;
    if (irq_count > 0 && !irq)   return -1;
    if (pci_count > 0 && !pci)   return -1;

    struct Allowance *al = kmalloc(sizeof(*al), KP_ZERO);
    if (!al) return -1;
    spin_lock_init(&al->lock);
    al->mmio_count = mmio_count;
    for (u32 i = 0; i < mmio_count; i++) al->mmio[i] = mmio[i];
    al->irq_count = irq_count;
    for (u32 i = 0; i < irq_count; i++) al->irq[i] = irq[i];
    al->dma_max = dma_max;
    al->pci_count = pci_count;
    for (u32 i = 0; i < pci_count; i++) al->pci[i] = pci[i];
    al->revoked = 0;

    // Install the new allowance UNDER g_proc_table_lock (audit F1): the confer
    // runs in the child's spawn thunk, AFTER the child is proc-tree-linked + thus
    // reachable by a concurrent proc_group_terminate -> proc_revoke_allowance
    // (which locks the OLD allowance). The lockless swap+free here raced that
    // revoke's spin_lock(&old->lock) -> UAF on the narrowed-parent-spawns-child
    // path (where `old` is the inherited clone). proc_allowance_install_locked
    // serializes the swap on g_proc_table_lock -- the lock the revoke runs under;
    // post-swap, `old` is unreferenced, so the kfree OUTSIDE the lock is safe.
    // The RELEASE store inside the helper preserves the gate-read ACQUIRE pairing
    // (the conferred-set writes above happen-before it).
    struct Allowance *old = proc_allowance_install_locked(p, al);
    if (old) kfree(old);
    return 0;
}

// CONFER GATE (specs/allowance.tla, the monotonic-reduction property): may
// `parent` confer the described allowance? True iff every requested resource
// is within the parent's OWN allowance, so a confer is a NARROWING and never a
// widening (I-2's hardware-axis analog). Reuses allowance_permits per resource:
// a BROAD parent (allowance == NULL) permits everything -> any narrowed set is
// conferrable (the warden); a NARROWED parent permits only its conferred set ->
// only a subset is conferrable (a bus driver spawning a sub-driver). An empty
// axis (count 0, a size-0 window, or dma_max 0) confers nothing and is within.
// A revoked parent confers nothing (allowance_permits is false while revoked).
bool allowance_confer_within_parent(struct Proc *parent,
                                    const struct hw_window *mmio, u32 mmio_count,
                                    const u32 *irq, u32 irq_count, u64 dma_max,
                                    const u32 *pci, u32 pci_count) {
    if (!parent)                          return false;
    if (mmio_count > ALLOWANCE_MMIO_MAX)  return false;
    if (irq_count > ALLOWANCE_IRQ_MAX)    return false;
    if (pci_count > ALLOWANCE_PCI_MAX)    return false;
    if (mmio_count > 0 && !mmio)          return false;
    if (irq_count > 0 && !irq)            return false;
    if (pci_count > 0 && !pci)            return false;

    for (u32 i = 0; i < mmio_count; i++) {
        if (mmio[i].size == 0) continue;   // empty window confers nothing
        if (!allowance_permits(parent, HW_RES_MMIO, mmio[i].base, mmio[i].size))
            return false;
    }
    for (u32 i = 0; i < irq_count; i++) {
        if (!allowance_permits(parent, HW_RES_IRQ, irq[i], 0))
            return false;
    }
    for (u32 i = 0; i < pci_count; i++) {
        if (!allowance_permits(parent, HW_RES_PCI, pci[i], 0))
            return false;
    }
    // dma_max == 0 confers no DMA (within trivially); allowance_permits rejects
    // size 0, so the empty grant must be special-cased rather than queried.
    if (dma_max > 0 && !allowance_permits(parent, HW_RES_DMA, dma_max, 0))
        return false;
    return true;
}

// Revoke (specs/allowance.tla): on DeviceRemoved, close the gate. Folded into
// proc_group_terminate (Menagerie build-arc step 5 / #160) so the warden's
// killgrp of a removed driver IS revoke-then-terminate atomically -- closing
// the in-flight-create race universally (allowance.tla revoke_race): a
// SYS_*_CREATE racing the removal observes `revoked` at the CreateCommit
// re-check and aborts. The #809/#811 cascade then drops the live handles at
// reap (the handle-axis teardown). Lock note: proc_group_terminate holds
// g_proc_table_lock when it calls this, so the live lock order gains the edge
// g_proc_table_lock -> al->lock. al->lock is a near-leaf (allowance_handle_alloc
// nests only the handle-table lock under it; nothing nests g_proc_table_lock
// under al->lock), so the order stays acyclic. NULL allowance -> no-op.
void proc_revoke_allowance(struct Proc *p) {
    if (!p) return;
    // ACQUIRE-load (audit F3): uniform with every other p->allowance reader
    // (allowance_permits / handle_alloc / is_narrowed / clone_into), pairing
    // with the RELEASE publish. Sound today (set-once, read under
    // g_proc_table_lock via proc_group_terminate), but the asymmetry was a trap.
    struct Allowance *al = __atomic_load_n(&p->allowance, __ATOMIC_ACQUIRE);
    if (!al) return;
    spin_lock(&al->lock);
    __atomic_store_n(&al->revoked, 1u, __ATOMIC_RELEASE);
    spin_unlock(&al->lock);
}

// rfork inherit: a narrowed parent's child is equally narrowed (the hardware-
// axis analog of caps' monotonic reduction, I-2). A broad parent -> child
// stays NULL. The conferred set is copied (immutable, a coherent snapshot);
// the revoked flag is copied (ACQUIRE) so a child forked after the parent's
// revocation is born revoked. The counts are clamped defensively (cf. the
// A-1a supp_gid_count clamp) so a corrupt source count can never leave a
// garbage tail.
int allowance_clone_into(struct Proc *child, struct Proc *parent) {
    if (!parent) return 0;
    // ACQUIRE the parent's allowance (audit F4): a coherent snapshot of the
    // pointer + (via the release pairing) the immutable conferred set.
    struct Allowance *src = __atomic_load_n(&parent->allowance, __ATOMIC_ACQUIRE);
    if (!src) return 0;   // broad parent -> child stays NULL
    struct Allowance *dst = kmalloc(sizeof(*dst), KP_ZERO);
    if (!dst) return -1;
    spin_lock_init(&dst->lock);
    dst->mmio_count = src->mmio_count > ALLOWANCE_MMIO_MAX
                    ? ALLOWANCE_MMIO_MAX : src->mmio_count;
    for (u32 i = 0; i < dst->mmio_count; i++) dst->mmio[i] = src->mmio[i];
    dst->irq_count = src->irq_count > ALLOWANCE_IRQ_MAX
                   ? ALLOWANCE_IRQ_MAX : src->irq_count;
    for (u32 i = 0; i < dst->irq_count; i++) dst->irq[i] = src->irq[i];
    dst->dma_max = src->dma_max;
    dst->pci_count = src->pci_count > ALLOWANCE_PCI_MAX
                   ? ALLOWANCE_PCI_MAX : src->pci_count;
    for (u32 i = 0; i < dst->pci_count; i++) dst->pci[i] = src->pci[i];
    dst->revoked = __atomic_load_n(&src->revoked, __ATOMIC_ACQUIRE);
    // RELEASE-publish into the (not-yet-running) child for consistency with the
    // confer publish; the child has no concurrent reader yet, but the edge is
    // recorded in code, not only in the spawn-ordering contract (audit F4).
    __atomic_store_n(&child->allowance, dst, __ATOMIC_RELEASE);
    return 0;
}

// proc_free: release the allowance. NULL-tolerant.
void allowance_free(struct Proc *p) {
    if (!p || !p->allowance) return;
    kfree(p->allowance);
    p->allowance = NULL;
}
