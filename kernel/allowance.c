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
    struct Allowance *al = p->allowance;
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
    }
    return false;   // unknown kind -> fail closed
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
    struct Allowance *al = p->allowance;
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
                          const u32 *irq, u32 irq_count, u64 dma_max) {
    if (!p) return -1;
    if (mmio_count > ALLOWANCE_MMIO_MAX) return -1;
    if (irq_count > ALLOWANCE_IRQ_MAX)   return -1;
    if (mmio_count > 0 && !mmio) return -1;
    if (irq_count > 0 && !irq)   return -1;

    struct Allowance *al = kmalloc(sizeof(*al), KP_ZERO);
    if (!al) return -1;
    spin_lock_init(&al->lock);
    al->mmio_count = mmio_count;
    for (u32 i = 0; i < mmio_count; i++) al->mmio[i] = mmio[i];
    al->irq_count = irq_count;
    for (u32 i = 0; i < irq_count; i++) al->irq[i] = irq[i];
    al->dma_max = dma_max;
    al->revoked = 0;

    struct Allowance *old = p->allowance;
    p->allowance = al;
    if (old) kfree(old);
    return 0;
}

// Revoke (specs/allowance.tla): on DeviceRemoved, close the gate. The CALLER
// then proc_group_terminate's the driver -- the #809/#811 cascade drops the
// live handles at reap. NULL allowance -> no-op.
void proc_revoke_allowance(struct Proc *p) {
    if (!p) return;
    struct Allowance *al = p->allowance;
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
    if (!parent || !parent->allowance) return 0;   // broad parent -> child NULL
    struct Allowance *src = parent->allowance;
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
    dst->revoked = __atomic_load_n(&src->revoked, __ATOMIC_ACQUIRE);
    child->allowance = dst;
    return 0;
}

// proc_free: release the allowance. NULL-tolerant.
void allowance_free(struct Proc *p) {
    if (!p || !p->allowance) return;
    kfree(p->allowance);
    p->allowance = NULL;
}
