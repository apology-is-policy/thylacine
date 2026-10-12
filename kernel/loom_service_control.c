// Exact identity, authority and diagnostic snapshots share Ll with admission,
// cancellation and collection. No usercopy, native teardown or final drop under
// Ll. Controls remain independent of SQ/CQ capacity and do not activate a worker.
#include <thylacine/loom_service_control.h>
#include <thylacine/loom_service_engine.h>
#include <thylacine/loom_service_pool_table.h>
#include <thylacine/loom_service_connect.h>
#include <thylacine/loom.h>
#include <thylacine/proc.h>
#include <thylacine/errno.h>

static int control_owner(struct Loom *l,struct Proc *p) {
    if(!l || l->magic!=LOOM_MAGIC || !p || p->magic!=PROC_MAGIC) return -T_E_INVAL;
    if(!loom_is_private(l)) return -T_E_OPNOTSUPP;
    if(p->as!=l->service_as || p->stripes!=l->service_creator) return -T_E_ACCES;
    return 0;
}
static bool control_zero_ref(struct loom_service_ref r) {
    return !r.slot && !r.reserved && !r.incarnation;
}
static bool control_same(struct loom_service_ref a,struct loom_service_ref b) {
    return a.slot==b.slot && a.incarnation==b.incarnation && a.reserved==b.reserved;
}
static struct loom_service_slot *control_exact(struct Loom *l,struct loom_service_ref ref) {
    if(ref.reserved || !ref.incarnation || ref.slot>=LOOM_SERVICE_SLOT_COUNT)return NULL;
    struct loom_service_slot *s=&l->service_slots->slots[ref.slot];
    return s->phase==LSLOT_LIVE && s->incarnation==ref.incarnation ? s : NULL;
}
static int control_input(struct Loom *l,struct Proc *p,const struct loom_service_control *in) {
    int rc=control_owner(l,p);if(rc)return rc;
    if(!in || in->size!=sizeof(*in) || in->version!=LOOM_SERVICE_ABI_VERSION ||
       in->flags || in->reserved || in->object.reserved)return -T_E_INVAL;
    return 0;
}
int loom_private_service_query(struct Loom *l,struct Proc *p,
        const struct loom_service_snapshot *in,struct loom_service_snapshot *out) {
    int rc=control_owner(l,p);if(rc)return rc;
    if(!in || !out || in->size!=sizeof(*in) || in->version!=LOOM_SERVICE_ABI_VERSION ||
       in->flags || in->object.reserved || in->kind || in->state || !control_zero_ref(in->scope) ||
       in->active_ops || in->pending_terminals || in->reason || in->status_flags)return -T_E_INVAL;
    struct loom_service_snapshot result={.size=sizeof(result),.version=LOOM_SERVICE_ABI_VERSION,.object=in->object};
    spin_lock(&l->lock);
    struct loom_service_slot *s=control_exact(l,in->object);
    if(!s)rc=-T_E_NOENT;
    else if(s->kind==LOOM_SERVICE_TARGET) {result.kind=s->kind;result.state=LOOM_SERVICE_READY;}
    else if(s->kind==LOOM_SERVICE_SCOPE || s->kind==LOOM_SERVICE_FID) {
        struct loom_service_node *n=&l->service_nodes->nodes[in->object.slot];
        result.kind=s->kind;result.state=n->state;
        result.scope=s->kind==LOOM_SERVICE_SCOPE ? in->object : n->parent;
        result.reason=n->reason;result.status_flags=n->status_flags;
        if(s->kind==LOOM_SERVICE_SCOPE) {
            struct loom_engine_entry *e=&l->service_engine->entries[in->object.slot];
            // Progress is written outside Ll. A live driver may have sent bytes
            // before reacquiring Ll; never race a raw cursor read here.
            if(e->phase!=LENGINE_EMPTY && e->incarnation==in->object.incarnation &&
               e->protocol->driving)result.status_flags|=LOOM_SERVICE_BYTES_SENT;
            for(u32 i=0;i<l->service_requests.capacity;++i) {
                struct loom_service_request *r=&l->service_requests.records[i];
                if(r->phase==LREQ_FREE || !control_same(r->scope,in->object))continue;
                if(r->phase==LREQ_PREPARED || r->phase==LREQ_ACTIVE)++result.active_ops;
                if(r->phase==LREQ_TERMINAL)++result.pending_terminals;
                if(r->borrows)result.status_flags|=LOOM_SERVICE_BYTES_SENT;
            }
        }
    } else rc=-T_E_INVAL; // POOL uses its dedicated snapshot per buffers ABI.
    if(!rc)*out=result;
    spin_unlock(&l->lock);
    return rc;
}
int loom_private_service_abort(struct Loom *l,struct Proc *p,const struct loom_service_control *in) {
    int rc=control_input(l,p,in);if(rc)return rc;
    bool indexed=false;
    spin_lock(&l->lock);
    struct loom_service_slot *s=control_exact(l,in->object);
    if(!s)rc=-T_E_NOENT;
    else if(s->kind!=LOOM_SERVICE_SCOPE)rc=-T_E_INVAL;
    else {
        struct loom_service_node *n=&l->service_nodes->nodes[in->object.slot];
        if(n->state==LOOM_SERVICE_ABORTING || n->state==LOOM_SERVICE_RETIRED)rc=1;
        else {
            struct loom_engine_entry *e=&l->service_engine->entries[in->object.slot];
            indexed=e->phase!=LENGINE_EMPTY && e->incarnation==in->object.incarnation;
            if(!indexed) {
                if(!n->reason)n->reason=-T_E_CANCELED;
                n->state=LOOM_SERVICE_ABORTING;
                // A constructing raw owner already holds scope/request pins.
                // Its publish/progress must observe this latch, and rollback
                // alone may settle unused abort once those pins are gone.
                if(!s->pins) {n->state=LOOM_SERVICE_RETIRED;n->status_flags|=LOOM_SERVICE_LOCAL_RETIRED;}
            }
        }
    }
    spin_unlock(&l->lock);
    if(rc || !indexed)return rc;
    rc=loom_engine_abort(l,in->object,-T_E_CANCELED); // owns entry through teardown
    if(rc!=-T_E_NOENT)return rc;
    // A concurrent driver may retire+collect before index pinning. Preserve
    // idempotence for the same unreaped identity, never for a reused slot.
    spin_lock(&l->lock);s=control_exact(l,in->object);
    if(s && s->kind==LOOM_SERVICE_SCOPE &&
       l->service_nodes->nodes[in->object.slot].state>=LOOM_SERVICE_ABORTING)rc=1;
    spin_unlock(&l->lock);
    return rc;
}
int loom_private_service_reap(struct Loom *l,struct Proc *p,const struct loom_service_control *in) {
    int rc=control_input(l,p,in);if(rc)return rc;
    u32 kind=0,state=0;
    spin_lock(&l->lock);
    struct loom_service_slot *s=control_exact(l,in->object);
    if(!s)rc=-T_E_NOENT;
    else {kind=s->kind;if(kind==LOOM_SERVICE_SCOPE || kind==LOOM_SERVICE_FID)state=l->service_nodes->nodes[in->object.slot].state;}
    spin_unlock(&l->lock);
    if(rc)return rc;
    // Each consuming primitive rechecks the SAME exact identity and its pins
    // under Ll; no naked pointer crosses this dispatch interval. Final drops
    // occur outside Ll while the caller's retained Loom keeps its image alive.
    if(kind==LOOM_SERVICE_TARGET)return loom_private_target_reap(l,in->object);
    if(kind==LOOM_SERVICE_POOL)return l->receipt_size ? loom_private_pool_reap(l,in->object) : -T_E_OPNOTSUPP;
    if(kind==LOOM_SERVICE_SCOPE && state==LOOM_SERVICE_RETIRED)return loom_private_connect_reap(l,in->object);
    if(kind==LOOM_SERVICE_SCOPE || kind==LOOM_SERVICE_FID)return loom_private_node_reap_unused(l,in->object);
    return -T_E_INVAL;
}

// RETURN uses the pool's retained backing and exact lease, never its originating
// scope. Scope retirement (or a full CQ) must not strand a consumer-held payload.
// Caller owns a copied record, stable creator image and Loom through the wake.
int loom_private_service_return(struct Loom *l,struct Proc *p,
        const struct loom_service_buffer_return *in) {
    int rc=control_owner(l,p);if(rc)return rc;
    if(!in || in->size!=sizeof(*in) || in->version!=LOOM_SERVICE_ABI_VERSION ||
       in->flags || in->receipt.reserved || in->receipt.pool.reserved ||
       !in->receipt.pool.incarnation || in->receipt.pool.slot>=LOOM_SERVICE_SLOT_COUNT ||
       in->receipt.member>=LOOM_SERVICE_POOL_MEMBERS)return -T_E_INVAL;
    if(!l->receipt_size)return -T_E_OPNOTSUPP;
    spin_lock(&l->lock);
    struct loom_service_slot *s=control_exact(l,in->receipt.pool);
    struct loom_pool *pool=&l->service_pools->pools[in->receipt.pool.slot];
    if(!s || s->kind!=LOOM_SERVICE_POOL)rc=-T_E_NOENT;
    else rc=loom_pool_return(&l->service_pools->bank,pool,&in->receipt);
    spin_unlock(&l->lock);
    // Ll serializes return with the next claim; wake only after releasing it.
    // No SQ/CQ lookup, allocation, native progress or scope pointer is needed.
    if(!rc)loom_private_close_changed(l);
    return rc;
}
int loom_private_service_pool_query(struct Loom *l,struct Proc *p,
        const struct loom_service_pool_snapshot *in,struct loom_service_pool_snapshot *out) {
    int rc=control_owner(l,p);if(rc)return rc;
    if(!in || !out || in->size!=sizeof(*in) || in->version!=LOOM_SERVICE_ABI_VERSION ||
       in->flags || in->pool.reserved || in->members || in->available || in->busy ||
       in->pending || in->leased || in->streams || in->reserved[0] || in->reserved[1])
        return -T_E_INVAL;
    if(!l->receipt_size)return -T_E_OPNOTSUPP;
    struct loom_service_pool_snapshot result;
    spin_lock(&l->lock);
    struct loom_service_slot *s=control_exact(l,in->pool);
    if(!s || s->kind!=LOOM_SERVICE_POOL)rc=-T_E_NOENT;
    else rc=loom_pool_snapshot(&l->service_pools->bank,&l->service_pools->pools[in->pool.slot],&result);
    spin_unlock(&l->lock);
    if(!rc)*out=result;
    return rc;
}
