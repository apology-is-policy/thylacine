// All constructor resources precede any wire effect. Ll never nests an image,
// allocator or client lock. The physical request owner bars reuse even if a
// close/abort driver runs during allocation or destruction outside Ll.
#include <thylacine/loom_service_operation.h>
#include <thylacine/loom_service_engine.h>
#include <thylacine/loom.h>
#include <thylacine/addrspace.h>
#include <thylacine/page.h>
#include <thylacine/errno.h>
#include <thylacine/extinction.h>
#include "mm/slub.h"

_Static_assert(sizeof(struct loom_service_operation)<=PAGE_SIZE,
               "operation metadata must fit its charged buddy page");

#ifdef KERNEL_TESTS
static u32 operation_fault;
void loom_private_operation_fault_arm_for_test(void) {
    __atomic_store_n(&operation_fault,1,__ATOMIC_RELAXED);
}
bool loom_private_operation_fault_pending_for_test(void) {
    return __atomic_load_n(&operation_fault,__ATOMIC_RELAXED)!=0;
}
static bool operation_fault_take(void) {
    return __atomic_exchange_n(&operation_fault,0,__ATOMIC_RELAXED)!=0;
}
#else
static bool operation_fault_take(void) { return false; }
#endif

static int operation_scope_ready(struct Loom *l,struct loom_service_ref s,
                                  struct loom_service_protocol *p) {
    if (l->service_closing || l->service_slots->closing) return -T_E_CANCELED;
    if (s.reserved || !s.incarnation || s.slot>=LOOM_SERVICE_SLOT_COUNT) return -T_E_NOENT;
    struct loom_service_slot *slot=&l->service_slots->slots[s.slot];
    struct loom_service_node *node=&l->service_nodes->nodes[s.slot];
    if (slot->incarnation!=s.incarnation || slot->kind!=LOOM_SERVICE_SCOPE ||
        slot->phase!=LSLOT_LIVE) return -T_E_NOENT;
    if (node->reason<0) return node->reason;
    if (node->state!=LOOM_SERVICE_READY || !p->connect_bound ||
        p->locally_retired || p->abort_reason) return -T_E_CANCELED;
    return 0;
}
static void operation_refund(struct Loom *l) {
    spin_lock(&l->service_as->lock);
    addrspace_uncharge_pages(l->service_as,1);
    spin_unlock(&l->service_as->lock);
}
static void operation_unreserve(struct Loom *l,struct loom_request_ref r) {
    spin_lock(&l->lock);
    if (loom_request_drop_owner(&l->service_requests,r) ||
        loom_request_rollback(&l->service_requests,l->service_slots,r))
        extinction("operation lost prepared physical owner");
    spin_unlock(&l->lock);
}
static void operation_put(struct Loom *l,struct loom_service_ref s,
                           struct loom_service_protocol *p) {
    if (loom_engine_put(l,s,p)) extinction("operation lost protocol entry pin");
    loom_private_close_changed(l);
    loom_unref(l); // retained creator image and Loom are last
}
int loom_private_operation_prepare(struct Loom *l,struct loom_service_ref scope,
    u64 user_data,u64 deadline,u64 now,struct loom_service_operation **out) {
    bool fault=operation_fault_take();
    if (!l || l->magic!=LOOM_MAGIC || !loom_is_private(l) || !out) return -T_E_INVAL;
    if (deadline && deadline<=now) return -T_E_TIMEDOUT;
    struct ProcServiceIdentity identity;
    if (!proc_service_snapshot_by_stripes(l->service_creator,l->service_as,&identity))
        return -T_E_ACCES;
    struct loom_service_protocol *p;
    int rc=loom_engine_get(l,scope,&p);
    if (rc) return rc;
    loom_ref(l);
    struct loom_request_ref request;
    spin_lock(&l->lock);
    rc=operation_scope_ready(l,scope,p);
    if (!rc) rc=loom_request_prepare(&l->service_requests,l->service_slots,
              l->service_nodes,scope,user_data,deadline,now,&request);
    if (!rc && loom_request_hold_owner(&l->service_requests,request))
        extinction("operation cannot hold fresh prepared owner");
    spin_unlock(&l->lock);
    if (rc) { operation_put(l,scope,p); return rc; }
    spin_lock(&l->service_as->lock);
    bool charged=addrspace_charge_pages(l->service_as,1,
                       identity.access.principal_id==PRINCIPAL_SYSTEM);
    spin_unlock(&l->service_as->lock);
    if (!charged) { rc=-T_E_NOMEM; goto fail_reservation; }
    // Request a full page, not sizeof(*op): exact buddy storage charge even
    // when operation metadata would otherwise fit a shared slab class.
    struct loom_service_operation *op=fault ? NULL : kmalloc(PAGE_SIZE,KP_ZERO);
    if (!op) { rc=-T_E_NOMEM; goto fail_charge; }
    op->owner=l; op->protocol=p; op->request=request; op->scope=scope;
    // Keep the fresh creator/image snapshot, never CONNECT or worker authority.
    const u8 *src=(const u8 *)&identity.access;
    u8 *dst=(u8 *)&op->program.identity;
    for(size_t i=0;i<sizeof(identity.access);++i)dst[i]=src[i];
    spin_lock(&l->lock);
    rc=operation_scope_ready(l,scope,p);
    spin_unlock(&l->lock);
    if (rc) { kfree(op); goto fail_charge; }
    *out=op;
    return 0;
fail_charge:
    operation_refund(l);
fail_reservation:
    operation_unreserve(l,request);
    operation_put(l,scope,p);
    return rc;
}
int loom_private_operation_release_unused(struct loom_service_operation *op) {
    if (!op || !op->owner || !op->protocol) return -T_E_INVAL;
    struct Loom *l=op->owner;
    struct loom_service_protocol *p=op->protocol;
    struct loom_service_ref scope=op->scope;
    struct loom_request_ref request=op->request;
    spin_lock(&l->lock);
    struct loom_service_request *r=request.index<l->service_requests.capacity ?
                              &l->service_requests.records[request.index] : NULL;
    bool unused=r && !request.reserved && request.identity &&
        r->identity==request.identity && r->phase==LREQ_PREPARED &&
        r->owners==1 && !r->borrows && !op->rpc.on_complete && !op->rpc.sending &&
        op->phase==LRPC_NEW && !op->next && !op->prev && !op->submitted && !op->fid_claim && !op->source_borrow;
    spin_unlock(&l->lock);
    if (!unused) return -T_E_BUSY;
    kfree(op);
    operation_refund(l);
    operation_unreserve(l,request);
    operation_put(l,scope,p);
    return 0;
}
