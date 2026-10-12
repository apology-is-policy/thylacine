// Private Loom syscall inners: docs/ASYNC-SERVICE-ABI.md, Qualification bridge.
// Built without LOOM_PRIVATE_QUALIFICATION, every entry refuses -EOPNOTSUPP, so
// no syscall can create a private ring and the rest is unreachable.
#include <thylacine/loom_service_syscall.h>
#include <thylacine/loom.h>
#include <thylacine/loom_service_abi.h>
#include <thylacine/loom_service_control.h>
#include <thylacine/loom_service_frontend.h>
#include <thylacine/burrow.h>
#include <thylacine/errno.h>
#include <thylacine/exec.h>
#include <thylacine/extinction.h>
#include <thylacine/handle.h>
#include <thylacine/proc.h>
#include <thylacine/spinlock.h>
#include <thylacine/vma.h>

_Static_assert(LOOM_REGISTER_SERVICE_TARGET == 2u && LOOM_REGISTER_QUERY_SERVICE_POOL == 9u,
               "the private register ops are the contiguous 2..9");

bool loom_private_routed(void) {
#ifdef LOOM_PRIVATE_QUALIFICATION
    return true;
#else
    return false;
#endif
}

bool loom_private_register_op(u32 op) {
    return op >= LOOM_REGISTER_SERVICE_TARGET && op <= LOOM_REGISTER_QUERY_SERVICE_POOL;
}

u32 loom_private_record_size(u32 op) {
    switch (op) {
    case LOOM_REGISTER_SERVICE_TARGET:        return sizeof(struct loom_service_target);
    case LOOM_REGISTER_SERVICE_SLOT:          return sizeof(struct loom_service_reserve);
    case LOOM_REGISTER_ABORT_SCOPE:           return sizeof(struct loom_service_control);
    case LOOM_REGISTER_QUERY_SERVICE_SLOT:    return sizeof(struct loom_service_snapshot);
    case LOOM_REGISTER_REAP_SERVICE_SLOT:     return sizeof(struct loom_service_control);
    case LOOM_REGISTER_SERVICE_POOL:          return sizeof(struct loom_service_pool_create);
    case LOOM_REGISTER_RETURN_SERVICE_BUFFER: return sizeof(struct loom_service_buffer_return);
    case LOOM_REGISTER_QUERY_SERVICE_POOL:    return sizeof(struct loom_service_pool_snapshot);
    default:                                  return 0;
    }
}

#ifndef LOOM_PRIVATE_QUALIFICATION

int loom_private_setup_for_proc(struct Proc *p, u32 entries, u32 flags,
                                struct loom_params *out, hidx_t *out_fd) {
    (void)p; (void)entries; (void)flags; (void)out; (void)out_fd;
    return -T_E_OPNOTSUPP;
}
int loom_private_register_buffers_routed(struct Loom *l, struct Proc *p,
                                         const struct loom_buf_reg *bufs, u32 n) {
    (void)l; (void)p; (void)bufs; (void)n;
    return -T_E_OPNOTSUPP;
}
int loom_private_enter(struct Loom *l, struct Proc *p, u32 to_submit,
                       u32 min_complete, u32 flags) {
    (void)l; (void)p; (void)to_submit; (void)min_complete; (void)flags;
    return -T_E_OPNOTSUPP;
}
int loom_private_register_record(struct Loom *l, struct Proc *p, u32 op,
                                 const void *record, loom_record_copyout_fn copyout,
                                 void *ctx) {
    (void)l; (void)p; (void)op; (void)record; (void)copyout; (void)ctx;
    return -T_E_OPNOTSUPP;
}

#else

int loom_private_setup_for_proc(struct Proc *p, u32 entries, u32 flags,
                                struct loom_params *out, hidx_t *out_fd) {
    const u32 known = LOOM_SETUP_PRIVATE_SERVICE | LOOM_SETUP_SERVICE_BUFFERS;
    if (!p || p->magic != PROC_MAGIC || !p->as || !out || !out_fd) return -T_E_INVAL;
    if (!(flags & LOOM_SETUP_PRIVATE_SERVICE) || (flags & ~known))  return -T_E_INVAL;
    if (!entries || entries > LOOM_MAX_ENTRIES || (entries & (entries - 1u)))
        return -T_E_INVAL;
    int rc = 0;
    struct Loom *l = loom_create_private_why(p, entries, entries * 2u,
                                             (flags & LOOM_SETUP_SERVICE_BUFFERS) != 0, &rc);
    if (!l) return rc;
    u32 ring_size = l->ring_size;

    // The private ring's pages were charged with its metadata at creation and the
    // charge recorded on the ring, so the mapping adds none: whichever drop ends
    // the ring settles it. The tag is set under the same lock hold as the map, so
    // no clone can observe the mapping untagged.
    u64 va = 0;
    bool mapped = false;
    struct AddrSpace *as = p->as;
    spin_lock(&as->lock);
    if (vma_find_gap(p, ring_size, EXEC_USER_BURROW_BASE, EXEC_USER_BURROW_TOP, &va) != 0 ||
        burrow_map(p, l->ring, va, ring_size, VMA_PROT_RW) != 0) {
        rc = -T_E_NOMEM;
    } else {
        struct Vma *v = vma_lookup_in(as, va);
        if (!v || v->burrow != l->ring || v->vaddr_start != va)
            extinction("private ring mapping vanished under its own lock");
        v->flags |= VMA_FLAG_PRIVATE_RING;
        mapped = true;
    }
    spin_unlock(&as->lock);
    if (!rc) rc = loom_private_start_executor(l);

    // Everything the caller learns is read before the handle exists: once it is
    // installed a sibling thread may close it, and l is then not ours to touch.
    if (!rc) {
        *out = (struct loom_params){
            .flags = flags, .sq_entries = l->sq_entries, .cq_entries = l->cq_entries,
            .ring_size = ring_size, .ring_va = va, .hdr_off = l->hdr_off,
            .sq_array_off = l->sq_array_off, .sqe_off = l->sqe_off, .cqe_off = l->cqe_off,
            .sq_array_size = l->sq_array_size, .sqe_size = l->sqe_size,
            .cqe_size = l->cqe_size,
        };
        if (l->receipt_size) {
            out->_resv1[0] = l->receipt_off;
            out->_resv1[1] = l->receipt_size;
            out->_resv1[2] = sizeof(struct loom_service_buffer_receipt);
        }
        hidx_t fd = handle_alloc(p, KOBJ_LOOM, RIGHT_READ | RIGHT_WRITE, l);
        if (fd >= 0) { *out_fd = fd; return 0; }
        rc = -T_E_MFILE;
    }
    // Both drops settle: the detach refunds nothing while the Loom holds the
    // ring, and the retirer's final drop then claims the charge.
    if (mapped) (void)sys_burrow_detach_for_proc(p, va, ring_size);
    loom_unref(l);
    return rc;
}

int loom_private_register_buffers_routed(struct Loom *l, struct Proc *p,
                                         const struct loom_buf_reg *bufs, u32 n) {
    return loom_private_register_buffers(l, p, bufs, n);
}

int loom_private_enter(struct Loom *l, struct Proc *p, u32 to_submit,
                       u32 min_complete, u32 flags) {
    if (!l || l->magic != LOOM_MAGIC || !loom_is_private(l)) return -T_E_INVAL;
    // Never sleeps: a client parks in poll(2) on the ring, with its deadline.
    if (min_complete || (flags & ~LOOM_ENTER_NONBLOCK)) return -T_E_INVAL;
    return loom_private_submit(l, p, to_submit);
}

// Publish a reservation only after the caller holds its result; otherwise burn
// it. The reservation is this call's alone until published, so its rollback
// cannot meet a competitor.
static int finish_reservation(struct Loom *l, struct loom_service_ref ref, u32 at,
                              loom_record_copyout_fn copyout, void *ctx,
                              int (*publish)(struct Loom *, struct loom_service_ref),
                              int (*rollback)(struct Loom *, struct loom_service_ref)) {
    int rc = copyout(ctx, at, &ref, sizeof(ref)) ? -T_E_FAULT : publish(l, ref);
    if (rc && rollback(l, ref)) extinction("private register lost its reservation rollback");
    return rc;
}

int loom_private_register_record(struct Loom *l, struct Proc *p, u32 op,
                                 const void *record, loom_record_copyout_fn copyout,
                                 void *ctx) {
    if (!l || l->magic != LOOM_MAGIC || !p || !record || !copyout) return -T_E_INVAL;
    if (!loom_is_private(l)) return -T_E_OPNOTSUPP;
    struct loom_service_ref ref = {0};
    int rc;
    switch (op) {
    case LOOM_REGISTER_SERVICE_TARGET:
        rc = loom_private_target_prepare(l, p, record, &ref);
        return rc ? rc : finish_reservation(l, ref,
            __builtin_offsetof(struct loom_service_target, result), copyout, ctx,
            loom_private_target_publish, loom_private_target_rollback);
    case LOOM_REGISTER_SERVICE_SLOT:
        rc = loom_private_node_prepare(l, p, record, &ref);
        return rc ? rc : finish_reservation(l, ref,
            __builtin_offsetof(struct loom_service_reserve, result), copyout, ctx,
            loom_private_node_publish, loom_private_node_rollback);
    case LOOM_REGISTER_SERVICE_POOL:
        rc = loom_private_pool_prepare(l, p, record, &ref);
        return rc ? rc : finish_reservation(l, ref,
            __builtin_offsetof(struct loom_service_pool_create, result), copyout, ctx,
            loom_private_pool_publish, loom_private_pool_rollback);
    case LOOM_REGISTER_ABORT_SCOPE:
        return loom_private_service_abort(l, p, record);
    case LOOM_REGISTER_REAP_SERVICE_SLOT:
        return loom_private_service_reap(l, p, record);
    case LOOM_REGISTER_RETURN_SERVICE_BUFFER:
        return loom_private_service_return(l, p, record);
    case LOOM_REGISTER_QUERY_SERVICE_SLOT: {
        struct loom_service_snapshot snap;
        rc = loom_private_service_query(l, p, record, &snap);
        if (rc) return rc;
        return copyout(ctx, 0, &snap, sizeof(snap)) ? -T_E_FAULT : 0;
    }
    case LOOM_REGISTER_QUERY_SERVICE_POOL: {
        struct loom_service_pool_snapshot snap;
        rc = loom_private_service_pool_query(l, p, record, &snap);
        if (rc) return rc;
        return copyout(ctx, 0, &snap, sizeof(snap)) ? -T_E_FAULT : 0;
    }
    default:
        return -T_E_INVAL;
    }
}

#endif
