// Preparation is invisible to the peer. The Loom reference pins the exact
// creator image while this owner holds page charges; native admission separately
// retains its registry/domain and ordinary weighted connection credits.
#include <thylacine/loom_service_protocol.h>
#include <thylacine/loom.h>
#include <thylacine/addrspace.h>
#include <thylacine/burrow.h>
#include <thylacine/9p_srvconn_transport.h>
#include <thylacine/srvconn.h>
#include <thylacine/errno.h>
#include <thylacine/page.h>
#include "../mm/slub.h"

_Static_assert(sizeof(struct loom_service_protocol) > 2048,
               "protocol owner must use buddy allocation for exact charging");

#ifdef KERNEL_TESTS
static u32 protocol_fault;
void loom_private_protocol_fault_arm_for_test(u32 stage) {
    __atomic_store_n(&protocol_fault, stage, __ATOMIC_RELAXED);
}
u32 loom_private_protocol_fault_pending_for_test(void) {
    return __atomic_load_n(&protocol_fault, __ATOMIC_RELAXED);
}
static u32 protocol_fault_take(void) {
    return __atomic_exchange_n(&protocol_fault, 0, __ATOMIC_RELAXED);
}
#else
static u32 protocol_fault_take(void) { return 0; }
#endif

// init requires a complete legacy vtable. It must never be a blocking adapter:
// progress_bind replaces byte delivery with exclusive native try operations.
// Accidental legacy access fails locally, rather than introducing a peer wait.
static int protocol_no_send(void *ctx, const u8 *buf, size_t len) {
    (void)ctx; (void)buf; (void)len; return -1;
}
static int protocol_no_recv(void *ctx, u8 *buf, size_t len) {
    (void)ctx; (void)buf; (void)len; return -1;
}
static int protocol_no_close(void *ctx) { (void)ctx; return -1; }

static void protocol_refund(struct Loom *l, u32 pages) {
    struct AddrSpace *as = l->service_as;
    spin_lock(&as->lock);
    addrspace_uncharge_pages(as, pages);
    spin_unlock(&as->lock);
}

static void protocol_destroy(struct loom_service_protocol *p) {
    struct Loom *l = p->owner;
    u32 pages = p->charged_pages;
    p9_client_destroy(&p->client);
    devsrv_connect_release(&p->admission);
    if (p->send_storage) kfree(p->send_storage);
    if (p->recv_storage) kfree(p->recv_storage);
    kfree(p);
    protocol_refund(l, pages);
    loom_unref(l); // last access to l or its image
}

int loom_private_protocol_release_unused(struct loom_service_protocol *p) {
    if (!p || !p->owner) return -T_E_INVAL;
    if (p->admission.published || p->connect_bound || p->indexed || p->users || p->driving)
        return -T_E_BUSY;
    protocol_destroy(p);
    return 0;
}
int loom_private_protocol_release_retired(struct loom_service_protocol *p) {
    if (!p || !p->owner) return -T_E_INVAL;
    if (!p->locally_retired || p->connect_bound || p->indexed || p->users || p->driving)
        return -T_E_BUSY;
    protocol_destroy(p);
    return 0;
}

int loom_private_protocol_prepare(struct Loom *l, struct loom_service_ref target,
                                  u64 deadline, u64 now,
                                  struct loom_service_protocol **out) {
    u32 fault = protocol_fault_take();
    if (!l || l->magic != LOOM_MAGIC || !loom_is_private(l) || !out)
        return -T_E_INVAL;
    if (deadline && deadline <= now) return -T_E_TIMEDOUT;
    struct ProcServiceIdentity identity;
    if (!proc_service_snapshot_by_stripes(l->service_creator, l->service_as, &identity))
        return -T_E_ACCES;
    const struct SrvServiceTarget *native;
    int rc = loom_private_target_get(l, target, &native);
    if (rc) return rc;
    // Convert the target borrow into an independent protocol owner reference.
    // The native prepare retains everything it needs before dropping the borrow.
    loom_ref(l);
    struct SrvConnectAdmission admission;
    rc = devsrv_service_prepare(native, &identity, &admission);
    (void)loom_private_target_put(l, target);
    if (rc) { loom_unref(l); return rc; }
    u32 msize = srvconn_msize(admission.conn);
    if (msize != SRVCONN_MSIZE && msize != SRVCONN_BULK_MSIZE) {
        devsrv_connect_release(&admission); loom_unref(l); return -T_E_INVAL;
    }
    u32 pages = (u32)burrow_backing_pages(sizeof(struct loom_service_protocol)) +
                2u * (u32)burrow_backing_pages(msize);
    struct AddrSpace *as = l->service_as;
    spin_lock(&as->lock);
    bool charged = addrspace_charge_pages(as, pages,
                                  identity.access.principal_id == PRINCIPAL_SYSTEM);
    spin_unlock(&as->lock);
    if (!charged) { devsrv_connect_release(&admission); loom_unref(l); return -T_E_NOMEM; }
    struct loom_service_protocol *p = fault == 1 ? NULL : kmalloc(sizeof(*p), KP_ZERO);
    if (!p) {
        devsrv_connect_release(&admission);
        protocol_refund(l, pages); loom_unref(l); return -T_E_NOMEM;
    }
    p->owner = l; p->identity = identity; p->admission = admission;
    p->charged_pages = pages;
    p->recv_storage = fault == 2 ? NULL : kmalloc(msize, KP_ZERO);
    if (!p->recv_storage) { rc = -T_E_NOMEM; goto fail; }
    // The preallocated API requires both buffers to be disjoint from *client,
    // including its legacy inline TX array. Charge both classes in full; do not
    // silently weaken that ownership contract to save the default TX pages.
    p->send_storage = fault == 3 ? NULL : kmalloc(msize, KP_ZERO);
    if (!p->send_storage) { rc = -T_E_NOMEM; goto fail; }
    struct p9_transport_ops unused = { .send=protocol_no_send, .recv=protocol_no_recv,
                                      .close=protocol_no_close, .ctx=NULL };
    rc = p9_client_init_preallocated(&p->client, 0, msize, unused,
                                     p->recv_storage, msize, p->send_storage, msize);
    if (rc) { rc = -T_E_INVAL; goto fail; }
    rc = p9_client_progress_bind(&p->client, &p->progress,
             p9_srvconn_progress_ops(p->admission.conn), identity.access.principal_id, deadline);
    if (rc) { rc = -T_E_INVAL; goto fail; }
    // Close during allocation cannot return a new usable prepared owner. A
    // subsequent close still requires the future publisher's locked recheck.
    spin_lock(&l->lock);
    bool closed = l->service_closing;
    spin_unlock(&l->lock);
    if (closed) { rc = -T_E_CANCELED; goto fail; }
    *out = p;
    return 0;
fail:
    (void)loom_private_protocol_release_unused(p);
    return rc;
}
