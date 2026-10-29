#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thylacine/loom_service_io.h>
#include <thylacine/loom_service_decode.h>
#include <thylacine/loom.h>
#include <thylacine/loom_service_fids.h>
#include <thylacine/loom_service_pool_table.h>
#include <thylacine/burrow.h>
#include <thylacine/errno.h>
// Physical-address translation, allocator, Proc snapshot and final backing-drop
// boundaries are host doubles. All exported data structures use real headers.
#define page_to_pa(p) ((paddr_t)(p))
#define pa_to_kva(p) ((void *)(p))
static unsigned checks;
#define CHECK(x,m) do{if(!(x)){fprintf(stderr,"FAIL %s\n",m);exit(1);}++checks;}while(0)
static struct Loom l;
static struct Proc creator;
static struct AddrSpace as,other_as;
static struct loom_service_protocol protocol;
static struct loom_service_request records[4];
static struct loom_slot_table slots;
static struct loom_service_node_table nodes;
static struct loom_service_pool_table pools;
static struct loom_service_ref scope,child,dest;
static struct Burrow backing,ring;
static u8 storage[4096];
static int roots=1,pins,charged,allocations,notifications,wire_effects;
static bool allocation_fault,close_during_alloc;
static struct loom_service_operation *drop_watch;
static void extinction(const char *s){fprintf(stderr,"INVARIANT %s\n",s);exit(86);}
bool loom_is_private(const struct Loom *x){return x==&l;}
void loom_ref(struct Loom *x){CHECK(x==&l,"retained Loom root");++roots;}
void loom_unref(struct Loom *x){CHECK(x==&l && !l.lock.value && roots>1,"Loom drop outside lock");--roots;}
void loom_private_close_changed(struct Loom *x){CHECK(x==&l&&!l.lock.value,"notification outside Ll");++notifications;}
void burrow_ref(struct Burrow *b){CHECK(l.lock.value && b==&backing && b->handle_count>0,"backing pin while table locked");++b->handle_count;}
static void loom_private_drop_buffer(struct AddrSpace *a,struct Burrow *b) {
 CHECK(!l.lock.value && a==&as && b==&backing && b->handle_count>0,"physical backing drop outside Ll");
 if(drop_watch){
  CHECK(drop_watch->fixed_dropping && !drop_watch->fixed.owner && l.service_fixed_settling==1,"unlink is not physical settlement");
  CHECK(loom_admission_cancel_locked(drop_watch,-T_E_CANCELED,true,5)==-T_E_BUSY,"terminal blocked during physical drop");
  CHECK(loom_private_io_drop_fixed(drop_watch)==-T_E_BUSY,"second physical drop refused");
 }
 --b->handle_count;
}
bool proc_service_snapshot_by_stripes(u64 stripes,const struct AddrSpace *a,struct ProcServiceIdentity *out) {
 CHECK(!l.lock.value && stripes==99 && a==&as,"fresh creator snapshot boundary");
 memset(out,0,sizeof(*out));out->access.principal_id=42;out->access.primary_gid=7;return true;
}
int loom_engine_get(struct Loom *x,struct loom_service_ref s,struct loom_service_protocol **out) {
 CHECK(x==&l&&!l.lock.value,"engine get unlocked");
 if(s.slot!=scope.slot || s.incarnation!=scope.incarnation)return -T_E_NOENT;
 ++pins;*out=&protocol;return 0;
}
int loom_engine_put(struct Loom *x,struct loom_service_ref s,struct loom_service_protocol *p) {
 CHECK(x==&l && s.incarnation==scope.incarnation && p==&protocol && pins==1 && !charged && !allocations && !l.lock.value,"entry pin through refund");--pins;return 0;
}
bool addrspace_charge_pages(struct AddrSpace *a,u32 n,bool exempt) {
 CHECK(a==&as && a->lock.value && n==1 && !exempt && pins==1,"charge before fixed resources");++charged;return true;
}
void addrspace_uncharge_pages(struct AddrSpace *a,u32 n) {
 CHECK(a==&as && a->lock.value && n==1 && charged==1 && !allocations,"refund after actual storage free");--charged;
}
static void *kmalloc(size_t n,u32 flags) {
 CHECK(n==PAGE_SIZE && !l.lock.value && charged==1,"charged page allocation");(void)flags;
 if(allocation_fault)return NULL;
 if(close_during_alloc){l.service_closing=true;slots.closing=true;}
 ++allocations;return calloc(1,n);
}
static void kfree(void *ptr) {
 struct loom_service_operation *o=ptr;
 CHECK(allocations==1 && charged==1 && !l.lock.value && !o->fixed.owner && !o->fixed_dropping && !o->source_borrow && !o->fid_claim,"storage free after physical holds");
 --allocations;free(ptr);
}
int p9_client_submit_async(struct p9_client *c,struct p9_rpc *r,p9_session_build_fn build,void *ctx) {
 (void)c;(void)r;(void)build;(void)ctx;++wire_effects;extinction("preparation sent wire");return -P9_E_IO;
}
static int loom_private_extent_locked(struct Loom *l,
                                      const struct loom_service_pool_member *m,
                                      struct loom_pool_extent *out) {
    if (m->reserved || m->buffer_index >= l->n_reg_buf || !m->length) {
        return -T_E_INVAL;
    }
    const struct loom_reg_buffer *b = &l->reg_buf[m->buffer_index];
    if (!b->burrow || b->burrow == l->ring ||
        b->burrow->type != BURROW_TYPE_ANON || !b->burrow->pages ||
        m->offset > b->len || m->length > b->len - m->offset) {
        return -T_E_INVAL;
    }
    // Integer subtraction avoids pointer-difference UB and overflowing
    // user offset+length. Table pins guarantee this ANON direct-map base
    // remains stable even after every original user mapping is detached.
    u64 base = (u64)pa_to_kva(page_to_pa(b->burrow->pages));
    u64 kva = (u64)b->kva;
    if (kva < base || kva - base > b->burrow->size ||
        b->len > b->burrow->size - (kva - base)) {
        return -T_E_INVAL;
    }
    *out = (struct loom_pool_extent){b->burrow,kva - base + m->offset,m->length};
    return 0;
}
int loom_private_fixed_acquire(struct Loom *l, struct Proc *p,
                               const struct loom_service_pool_member *m,
                               struct loom_private_fixed *f) {
    if (!l || l->magic != LOOM_MAGIC || !loom_is_private(l) || !m || !f ||
        !p || p->magic != PROC_MAGIC) return -T_E_INVAL;
    if (p->as != l->service_as || p->stripes != l->service_creator)
        return -T_E_ACCES;
    int rc = 0;
    struct loom_pool_extent e;
    spin_lock(&l->lock);
    if (l->service_closing) rc = -T_E_CANCELED;
    else if (f->owner || f->next || f->extent.backing || f->extent.offset || f->extent.length)
        rc = -T_E_BUSY;
    else if (l->service_fixed_count + l->service_fixed_settling >= l->cq_entries) rc = -T_E_AGAIN;
    else if ((rc = loom_private_extent_locked(l,m,&e)) == 0) {
        if (loom_pool_conflicts(&l->service_pools->bank,e)) rc = -T_E_BUSY;
        else {
            // Existing CQ capacity bounds traversal. Actual request admission
            // still owes its own shared terminal reservation before dispatch.
            if(false)burrow_ref((struct Burrow *)e.backing);
            loom_ref(l);
            *f = (struct loom_private_fixed){l->service_fixed,l,e};
            l->service_fixed = f;
            ++l->service_fixed_count;
        }
    }
    spin_unlock(&l->lock);
    return rc;
}
int loom_private_fixed_release(struct Loom *l, struct loom_private_fixed *f) {
    if (!l || l->magic != LOOM_MAGIC || !loom_is_private(l) || !f) return -T_E_INVAL;
    spin_lock(&l->lock);
    struct loom_private_fixed **link = &l->service_fixed;
    while (*link && *link != f) link = &(*link)->next;
    if (!*link || f->owner != l) { spin_unlock(&l->lock); return -T_E_NOENT; }
    struct Burrow *b = (struct Burrow *)f->extent.backing;
    struct AddrSpace *as = l->service_as;
    *link = f->next;
    --l->service_fixed_count;
    ++l->service_fixed_settling;
    *f = (struct loom_private_fixed){0};
    spin_unlock(&l->lock);
    // A concurrent rebind can drop the last table pin after unlink. This pin
    // still owns the pages; release it and settle before dropping the owner.
    // No reference to f or l is used after the final Loom unref.
    loom_private_drop_buffer(as,b);
    spin_lock(&l->lock);
    --l->service_fixed_settling;
    spin_unlock(&l->lock);
    loom_private_close_changed(l);
    loom_unref(l);
    return 0;
}
u32 p9_client_alloc_fid(struct p9_client *c) {
    if (!c) return P9_NOFID;
    if (c->magic != P9_CLIENT_MAGIC) return P9_NOFID;
    spin_lock(&c->lock);
    if (c->next_fid == P9_NOFID) {
        spin_unlock(&c->lock);
        return P9_NOFID;       // exhausted
    }
    u32 fid = c->next_fid;
    c->next_fid++;
    spin_unlock(&c->lock);
    return fid;
}
// All constructor resources precede any wire effect. Ll never nests an image,
// allocator or client lock. The physical request owner bars reuse even if a
// close/abort driver runs during allocation or destruction outside Ll.

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
        op->phase==LRPC_NEW && !op->next && !op->prev && !op->submitted && !op->fid_claim && !op->source_borrow && !op->fixed.owner && !op->fixed_dropping;
    spin_unlock(&l->lock);
    if (!unused) return -T_E_BUSY;
    kfree(op);
    operation_refund(l);
    operation_unreserve(l,request);
    operation_put(l,scope,p);
    return 0;
}

_Static_assert(sizeof(struct loom_service_operation)<=PAGE_SIZE,"RPC owner buddy page");
static bool rpc_same(struct loom_service_ref a,struct loom_service_ref b) {
    return a.slot==b.slot && a.incarnation==b.incarnation && a.reserved==b.reserved;
}
static struct loom_service_request *rpc_record(struct loom_service_operation *o) {
    if(!o || !o->owner || !o->protocol || o->protocol->owner!=o->owner ||
       !rpc_same(o->scope,o->protocol->scope) || o->request.reserved ||
       !o->request.identity)return NULL;
    struct loom_request_table *t=&o->owner->service_requests;
    if(!t->records || o->request.index>=t->capacity)return NULL;
    struct loom_service_request *r=&t->records[o->request.index];
    return r->identity==o->request.identity && rpc_same(r->scope,o->scope) &&
           r->owners==1 ? r : NULL;
}
static int rpc_ready(struct loom_service_operation *o) {
    struct Loom *l=o->owner;struct loom_service_protocol *p=o->protocol;
    struct loom_service_ref ref=o->scope;
    if(ref.reserved || !ref.incarnation || ref.slot>=LOOM_SERVICE_SLOT_COUNT)return -T_E_NOENT;
    struct loom_service_slot *s=&l->service_slots->slots[ref.slot];
    struct loom_service_node *n=&l->service_nodes->nodes[ref.slot];
    if(s->incarnation!=ref.incarnation || s->kind!=LOOM_SERVICE_SCOPE ||
       s->phase!=LSLOT_LIVE)return -T_E_NOENT;
    if(l->service_closing || l->service_slots->closing || l->service_requests.detached ||
       n->state!=LOOM_SERVICE_READY || n->reason || p->abort_reason ||
       p->locally_retired || !p->connect_bound || !p->indexed)return -T_E_CANCELED;
    return 0;
}
int loom_rpc_check_enqueue_locked(struct loom_service_operation *o,u64 now) {
    struct loom_service_request *r=rpc_record(o);
    if(!r)return -T_E_NOENT;
    struct Loom *l=o->owner;struct loom_service_protocol *p=o->protocol;
    if(o->phase!=LRPC_NEW || o->next || o->prev || o->callback_seen ||
       o->capture_error || o->rpc.on_complete || o->rpc.sending || r->phase!=LREQ_PREPARED || r->borrows)
        return -T_E_BUSY;
    if(p->op_count>=l->service_requests.capacity)return -T_E_NOSPC;
    int rc=rpc_ready(o);
    if(rc)return rc;
    return loom_request_check_admit(&l->service_requests,l->service_slots,l->service_nodes,o->request,now);
}
int loom_rpc_enqueue_locked(struct loom_service_operation *o,u64 now) {
    int rc=loom_rpc_check_enqueue_locked(o,now);if(rc)return rc;
    struct Loom *l=o->owner;struct loom_service_protocol *p=o->protocol;
    rc=loom_request_admit(&l->service_requests,l->service_slots,l->service_nodes,o->request,now);
    if(rc)return rc;
    if(loom_request_borrow(&l->service_requests,l->service_slots,l->service_nodes,o->request))
        extinction("RPC lost fresh request borrow");
    o->prev=p->op_tail;
    if(p->op_tail)p->op_tail->next=o;else p->op_head=o;
    p->op_tail=o;++p->op_count;o->phase=LRPC_QUEUED;
    return 0;
}
int loom_rpc_begin_locked(struct loom_service_operation *o,u64 now) {
    struct loom_service_request *r=rpc_record(o);
    if(!r)return -T_E_NOENT;
    if(!o->protocol->driving || o->phase!=LRPC_QUEUED ||
       r->phase!=LREQ_ACTIVE || r->borrows!=1 || o->rpc.sending)return -T_E_BUSY;
    int rc=rpc_ready(o);if(rc)return rc;
    if(o->capture_error || o->callback_seen)return -T_E_IO;
    if(r->deadline && r->deadline<=now)return -T_E_TIMEDOUT;
    o->callback_seen=false;o->capture_error=0;
    o->submitted=false;o->advanced=false;
    o->rpc.on_complete=loom_rpc_capture;o->phase=LRPC_INFLIGHT;
    return 0;
}
void loom_rpc_capture(struct p9_rpc *rpc,int status,struct p9_dispatch_result *reply) {
    // Actual retained owner; callback runs only inside the sole driver. Never
    // touch Ll, CQ, allocator, client APIs or the list from this boundary.
    struct loom_service_operation *o=(void *)((u8 *)rpc-
                                  __builtin_offsetof(struct loom_service_operation,rpc));
    if(o->phase!=LRPC_INFLIGHT || o->callback_seen) {
        o->capture_error=-T_E_IO;return;
    }
    o->capture_error=loom_private_reply_capture(&o->program.wire.expect,status,reply,
                                      o->program.wire.payload,o->program.wire.payload_capacity,&o->reply);
    o->callback_seen=true;
}
int loom_rpc_requeue_locked(struct loom_service_operation *o,u64 now) {
    struct loom_service_request *r=rpc_record(o);
    if(!r)return -T_E_NOENT;
    if(!o->protocol->driving || o->phase!=LRPC_INFLIGHT || !o->callback_seen ||
       o->capture_error || o->rpc.sending || !o->advanced || o->action!=LPROGRAM_NEXT ||
       r->phase!=LREQ_ACTIVE || r->borrows!=1)
        return -T_E_BUSY;
    int rc=rpc_ready(o);if(rc)return rc;
    if(r->deadline && r->deadline<=now)return -T_E_TIMEDOUT;
    o->rpc.on_complete=NULL;o->callback_seen=false;o->phase=LRPC_QUEUED;
    return 0;
}
int loom_rpc_settle_locked(struct loom_service_operation *o) {
    struct loom_service_request *r=rpc_record(o);
    if(!r)return -T_E_NOENT;
    if(!o->protocol->driving || r->phase!=LREQ_ACTIVE || r->borrows!=1 ||
       o->rpc.sending || (o->phase!=LRPC_QUEUED &&
         (o->phase!=LRPC_INFLIGHT || !o->callback_seen)))return -T_E_BUSY;
    // Sole driver has returned from client code. There can be no later callback
    // before either requeue or destruction; payload/FID release may now settle.
    o->rpc.on_complete=NULL;o->phase=LRPC_SETTLING;
    return 0;
}
int loom_rpc_check_finish_locked(struct loom_service_operation *o,s32 result,u64 now) {
    struct loom_service_request *r=rpc_record(o);
    if(!r)return -T_E_NOENT;
    if(!o->protocol->driving || o->phase!=LRPC_SETTLING ||
       r->phase!=LREQ_ACTIVE || r->borrows!=1 || o->rpc.on_complete || o->rpc.sending)
        return -T_E_BUSY;
    // Malformed or duplicate completion poisons the private scope. Do not
    // turn it into an isolated operation error while leaving the scope alive.
    if(result<0 && (o->capture_error || (o->advanced && o->action==LPROGRAM_ABORT)) &&
       !o->protocol->abort_reason)return -T_E_BUSY;
    if(result>=0) {
        if(!o->callback_seen || o->capture_error || o->reply.result<0 ||
           !o->advanced || o->action!=LPROGRAM_TERMINAL ||
           o->program.phase!=LPROGRAM_DONE || o->program.result<0)return -T_E_IO;
        int rc=rpc_ready(o);if(rc)return rc;
        if(r->deadline && r->deadline<=now)return -T_E_TIMEDOUT;
    }
    return 0;
}
int loom_rpc_finish_locked(struct loom_service_operation *o,s32 result,u64 now) {
    int rc=loom_rpc_check_finish_locked(o,result,now);if(rc)return rc;
    struct Loom *l=o->owner;
    if(loom_request_put(&l->service_requests,o->request) ||
       loom_request_finish(&l->service_requests,l->service_slots,l->service_nodes,o->request,result))
        extinction("RPC lost settled terminal obligation");
    o->phase=LRPC_TERMINAL;
    return 0;
}
int loom_rpc_unlink_locked(struct loom_service_operation *o) {
    struct loom_service_request *r=rpc_record(o);
    if(!r)return -T_E_NOENT;
    struct loom_service_protocol *p=o->protocol;
    if(o->phase!=LRPC_TERMINAL || r->phase!=LREQ_DELIVERED || r->borrows ||
       o->rpc.on_complete || o->rpc.sending || !p->op_count)return -T_E_BUSY;
    if((o->prev ? o->prev->next!=o : p->op_head!=o) ||
       (o->next ? o->next->prev!=o : p->op_tail!=o))return -T_E_INVAL;
    if(o->prev)o->prev->next=o->next;else p->op_head=o->next;
    if(o->next)o->next->prev=o->prev;else p->op_tail=o->prev;
    --p->op_count;o->next=NULL;o->prev=NULL;o->phase=LRPC_RETIRED;
    return 0;
}

// These two edges run outside Ll, under the exclusive p->driving claim. They
// may not release storage. A callback may occur synchronously inside submit.
int loom_rpc_submit(struct loom_service_operation *o) {
    if(!o || !o->protocol || !o->protocol->driving || o->phase!=LRPC_INFLIGHT ||
       o->rpc.on_complete!=loom_rpc_capture || o->submitted || o->callback_seen ||
       o->rpc.sending)return -T_E_BUSY;
    o->submitted=true;
    int rc=loom_private_wire_submit(&o->protocol->client,&o->rpc,&o->program.wire);
    // Invalid client arguments are the one refusal that does not callback.
    // Accepted negative completions already captured: never duplicate them.
    if(rc<0 && !o->callback_seen)loom_rpc_capture(&o->rpc,rc,NULL);
    return rc;
}
int loom_rpc_advance(struct loom_service_operation *o,enum loom_program_action *out) {
    if(!o || !out || !o->protocol || !o->protocol->driving ||
       o->phase!=LRPC_INFLIGHT || !o->submitted || !o->callback_seen ||
       o->rpc.sending || o->advanced)return -T_E_BUSY;
    o->action=loom_private_program_reply(&o->program,&o->reply,o->capture_error);
    o->advanced=true;*out=o->action;return 0;
}

static bool admission_same(struct loom_service_ref a,struct loom_service_ref b) {
    return a.slot==b.slot && a.incarnation==b.incarnation && a.reserved==b.reserved;
}
int loom_admission_scope_locked(struct Loom *l,struct loom_service_ref source,
                                struct loom_service_ref *out) {
    if(!l || !out || !l->service_slots || !l->service_nodes || source.reserved ||
       !source.incarnation || source.slot>=LOOM_SERVICE_SLOT_COUNT)return -T_E_INVAL;
    struct loom_service_slot *s=&l->service_slots->slots[source.slot];
    struct loom_service_node *n=&l->service_nodes->nodes[source.slot];
    if(s->incarnation!=source.incarnation || s->phase!=LSLOT_LIVE ||
       (s->kind!=LOOM_SERVICE_SCOPE && s->kind!=LOOM_SERVICE_FID))return -T_E_NOENT;
    struct loom_service_ref scope=s->kind==LOOM_SERVICE_SCOPE?source:n->parent;
    if(scope.reserved || !scope.incarnation || scope.slot>=LOOM_SERVICE_SLOT_COUNT)return -T_E_NOENT;
    s=&l->service_slots->slots[scope.slot];n=&l->service_nodes->nodes[scope.slot];
    if(s->kind!=LOOM_SERVICE_SCOPE || s->incarnation!=scope.incarnation ||
       s->phase!=LSLOT_LIVE)return -T_E_NOENT;
    if(l->service_closing || l->service_slots->closing ||
       n->state!=LOOM_SERVICE_READY || n->reason)return -T_E_CANCELED;
    *out=scope;return 0;
}
static void admission_put(struct loom_service_operation *o) {
    struct Loom *l=o->owner;
    if(o->source_borrow) {
        if(loom_fid_put(l->service_slots,l->service_nodes,o->source))
            extinction("admission lost source borrow");
        o->source_borrow=false;
    }
    if(o->fid_claim) {
        struct loom_service_ref ref=o->fid_claim==P9_TWALK?o->destination:o->source;
        if(loom_fid_release_claim(l->service_slots,l->service_nodes,ref,o->request.identity))
            extinction("admission lost exact FID claim");
        o->fid_claim=0;
    }
}
int loom_admission_enqueue_locked(struct loom_service_operation *o,
    struct loom_service_ref source,struct loom_service_ref destination,u64 now) {
    int rc=loom_rpc_check_enqueue_locked(o,now);if(rc)return rc;
    if(o->fid_claim || o->source_borrow ||
       (o->program.phase!=LPROGRAM_STAT && o->program.phase!=LPROGRAM_EFFECT))return -T_E_BUSY;
    struct Loom *l=o->owner;struct loom_service_ref scope;
    rc=loom_admission_scope_locked(l,source,&scope);if(rc)return rc;
    if(!admission_same(scope,o->scope))return -T_E_INVAL;
    u8 kind=o->program.operation;u32 wire=0;
    if(kind!=P9_TWALK && kind!=P9_TLOPEN && kind!=P9_TREAD &&
       kind!=P9_TWRITE && kind!=P9_TCLUNK)return -T_E_OPNOTSUPP;
    if(kind==P9_TWALK && (o->program.wire.new_fid==P9_NOFID ||
       admission_same(source,destination)))return -T_E_INVAL;
    o->source=source;o->destination=destination;
    if(kind==P9_TREAD || kind==P9_TWRITE || kind==P9_TWALK) {
        u32 need=kind==P9_TREAD?LFID_READ:kind==P9_TWRITE?LFID_WRITE:0;
        rc=loom_fid_borrow(l->service_slots,l->service_nodes,source,scope,need,&wire);
        if(rc)return rc;
        o->source_borrow=true;
        if(kind==P9_TWALK) {
            rc=loom_fid_claim_walk(l->service_slots,l->service_nodes,destination,scope,o->request.identity);
            if(rc){admission_put(o);return rc;}
            o->fid_claim=kind;
        }
    } else if(kind==P9_TLOPEN) {
        rc=loom_fid_claim_open(l->service_slots,l->service_nodes,source,scope,o->request.identity);
        if(rc)return rc;
        o->fid_claim=kind;wire=l->service_nodes->nodes[source.slot].wire_fid;
    } else {
        // Pure queue/request checks above remain true under this same Ll hold.
        // No fallible work may follow this irreversible local retirement.
        rc=loom_fid_begin_clunk(l->service_slots,l->service_nodes,source,scope,o->request.identity);
        if(rc)return rc;
        o->fid_claim=kind;wire=l->service_nodes->nodes[source.slot].wire_fid;
    }
    o->program.wire.fid=wire;
    if(loom_rpc_enqueue_locked(o,now))extinction("admission changed inside Ll");
    return 0;
}
int loom_admission_begin_locked(struct loom_service_operation *o,u64 now) {
    if(!o)return -T_E_INVAL;
    if(o->fid_claim==P9_TCLUNK) {
        u32 wire;
        int rc=loom_fid_clunk_wire(o->owner->service_slots,o->owner->service_nodes,
                                 o->source,o->request.identity,&wire);
        if(rc)return rc;
        o->program.wire.fid=wire;
    }
    return loom_rpc_begin_locked(o,now);
}
int loom_admission_commit_locked(struct loom_service_operation *o,u64 now) {
    if(!o)return -T_E_INVAL;
    if(o->fixed.owner || o->fixed_dropping)return -T_E_BUSY;
    s32 result=o->program.operation==P9_TWALK?(s32)o->destination.slot:o->program.result;
    // A negative program result belongs to cancel, including open DAC refusal.
    if(o->program.result<0)return -T_E_INVAL;
    int rc=loom_rpc_check_finish_locked(o,result,now);if(rc)return rc;
    struct Loom *l=o->owner;
    switch(o->program.operation) {
    case P9_TWALK:
        if(o->fid_claim!=P9_TWALK || !o->source_borrow)return -T_E_BUSY;
        rc=loom_fid_commit_walk(l->service_slots,l->service_nodes,o->destination,o->scope,
                               o->request.identity,o->program.wire.new_fid);break;
    case P9_TLOPEN:
        if(o->fid_claim!=P9_TLOPEN)return -T_E_BUSY;
        rc=loom_fid_commit_open(l->service_slots,l->service_nodes,o->source,o->scope,
                               o->request.identity,o->program.wire.mode);break;
    case P9_TCLUNK:
        if(o->fid_claim!=P9_TCLUNK)return -T_E_BUSY;
        rc=loom_fid_finish_clunk(l->service_slots,l->service_nodes,o->source,o->request.identity);break;
    case P9_TREAD:case P9_TWRITE:
        if(!o->source_borrow)return -T_E_BUSY;
        break;
    default:return -T_E_OPNOTSUPP;
    }
    if(rc)return rc;
    admission_put(o);
    if(loom_rpc_finish_locked(o,result,now))extinction("admission commit lost terminal");
    return 0;
}
int loom_admission_cancel_locked(struct loom_service_operation *o,s32 reason,
                                 bool abort_quiesced,u64 now) {
    if(!o || reason>=0)return -T_E_INVAL;
    if(o->fixed.owner || o->fixed_dropping)return -T_E_BUSY;
    int rc=loom_rpc_check_finish_locked(o,reason,now);if(rc)return rc;
    struct Loom *l=o->owner;
    if(o->fid_claim==P9_TCLUNK) {
        if(!abort_quiesced || !o->protocol->abort_reason)return -T_E_BUSY;
        rc=loom_fid_finish_clunk(l->service_slots,l->service_nodes,o->source,o->request.identity);
        if(rc)return rc; // earlier source borrowers still retain this FID
    }
    admission_put(o);
    if(loom_rpc_finish_locked(o,reason,now))extinction("admission cancellation lost terminal");
    return 0;
}


static u8 io_kind(u8 opcode) {
    switch(opcode) {
    case LOOM_OP_WALK:return P9_TWALK;
    case LOOM_OP_LOPEN:return P9_TLOPEN;
    case LOOM_OP_READ:return P9_TREAD;
    case LOOM_OP_WRITE:return P9_TWRITE;
    case LOOM_OP_CLUNK:return P9_TCLUNK;
    default:return 0;
    }
}
// Empty fixed I/O has no byte users or backing hold, but still validates its
// registered slot and offset. Never fabricate a zero-length pool extent.
static int io_empty(struct Loom *l,u32 index,u64 offset) {
    int rc=0;
    spin_lock(&l->lock);
    if(l->service_closing)rc=-T_E_CANCELED;
    else if(index>=l->n_reg_buf)rc=-T_E_INVAL;
    else {
        struct loom_reg_buffer *b=&l->reg_buf[index];
        if(!b->burrow || b->burrow==l->ring || b->burrow->type!=BURROW_TYPE_ANON ||
           !b->burrow->pages || offset>b->len)rc=-T_E_INVAL;
    }
    spin_unlock(&l->lock);return rc;
}
static int io_view(struct loom_service_operation *o,u8 **out) {
    struct loom_pool_extent e=o->fixed.extent;
    struct Burrow *b=(struct Burrow *)e.backing;
    if(o->fixed.owner!=o->owner || !b || b->type!=BURROW_TYPE_ANON || !b->pages ||
       !e.length || e.offset>b->size || e.length>b->size-e.offset)return -T_E_IO;
    *out=(u8 *)pa_to_kva(page_to_pa(b->pages))+e.offset;
    return 0;
}
int loom_private_io_drop_fixed(struct loom_service_operation *o) {
    if(!o || !o->owner || o->fixed_dropping || o->rpc.sending ||
       (o->phase!=LRPC_NEW && o->phase!=LRPC_SETTLING) || o->rpc.on_complete)
        return -T_E_BUSY;
    o->fixed_dropping=true;
    int rc=o->fixed.owner?loom_private_fixed_release(o->owner,&o->fixed):0;
    o->fixed_dropping=false;
    if(!rc){o->program.wire.payload=NULL;o->program.wire.payload_capacity=0;}
    return rc;
}
int loom_private_io_release_prepared(struct loom_service_operation *o) {
    if(!o || o->phase!=LRPC_NEW || o->source_borrow || o->fid_claim ||
       o->submitted || o->rpc.on_complete || o->rpc.sending)return -T_E_BUSY;
    int rc=loom_private_io_drop_fixed(o);if(rc)return rc;
    return loom_private_operation_release_unused(o);
}
int loom_private_io_prepare(struct Loom *l,struct Proc *p,const struct loom_sqe *s,
                            u64 now,struct loom_service_operation **out) {
    if(!l || l->magic!=LOOM_MAGIC || !loom_is_private(l) || !p ||
       p->magic!=PROC_MAGIC || !s || !out)return -T_E_INVAL;
    if(p->as!=l->service_as || p->stripes!=l->service_creator)return -T_E_ACCES;
    // Caller owns this immutable copy for the whole call; no shared-ring reads.
    int rc=loom_private_sqe_shape(s,false);if(rc)return rc;
    u8 kind=io_kind(s->opcode);if(!kind)return -T_E_OPNOTSUPP;
    struct loom_service_ref source={.slot=s->handle_idx,.incarnation=s->_resv1[1]};
    struct loom_service_ref destination={0},scope;
    if(kind==P9_TWALK)destination=(struct loom_service_ref){.slot=(u32)s->offset,.incarnation=s->_resv1[2]};
    spin_lock(&l->lock);
    rc=loom_admission_scope_locked(l,source,&scope);
    spin_unlock(&l->lock);if(rc)return rc;
    struct loom_service_operation *o;
    rc=loom_private_operation_prepare(l,scope,s->user_data,s->_resv1[3],now,&o);
    if(rc)return rc;
    o->source=source;o->destination=destination;
    if(kind==P9_TREAD || kind==P9_TWRITE || (kind==P9_TWALK && s->len)) {
        if(!s->len)rc=io_empty(l,s->buf_idx_or_off,s->_resv1[0]);
        else {
            struct loom_service_pool_member m={.buffer_index=s->buf_idx_or_off,
                                               .offset=s->_resv1[0],.length=s->len};
            rc=loom_private_fixed_acquire(l,p,&m,&o->fixed);
            if(!rc) {
                u8 *view;
                rc=io_view(o,&view);
                if(!rc && kind==P9_TWALK) {
                    for(u32 i=0;i<s->len;++i)o->program.wire.component[i]=view[i];
                    o->program.wire.component_len=(u16)s->len;
                    rc=loom_private_walk_component(o->program.wire.component,s->len);
                    if(!rc)rc=loom_private_io_drop_fixed(o); // copied name owns no remaining byte users
                } else if(!rc) {
                    o->program.wire.payload=view;o->program.wire.payload_capacity=s->len;
                }
            }
        }
        if(rc)goto fail;
    }
    if(kind==P9_TREAD || kind==P9_TWRITE) {
        o->program.wire.count=s->len;o->program.wire.offset=s->offset;
    } else if(kind==P9_TLOPEN)o->program.wire.mode=(u32)s->offset;
    else if(kind==P9_TWALK) {
        o->program.wire.new_fid=p9_client_alloc_fid(&o->protocol->client);
        if(o->program.wire.new_fid==P9_NOFID){rc=-T_E_NOSPC;goto fail;}
    }
    rc=loom_private_program_start(&o->program,&o->program.identity,kind);
    if(rc)goto fail;
    *out=o;return 0;
fail:
    if(loom_private_io_release_prepared(o))extinction("I/O prepare lost unpublished holds");
    return rc;
}
int loom_private_io_publish(struct loom_service_operation *o,u64 now) {
    if(!o || !o->owner || o->fixed_dropping)return -T_E_INVAL;
    if((o->program.operation==P9_TREAD || o->program.operation==P9_TWRITE) &&
       o->program.wire.count && o->fixed.owner!=o->owner)return -T_E_BUSY;
    struct Loom *l=o->owner;
    // Publishing can let the worker consume/free o. Retain a separate wake root
    // across the unlock; no field of o is read after a successful transfer.
    loom_ref(l);
    spin_lock(&l->lock);
    int rc=loom_admission_enqueue_locked(o,o->source,o->destination,now);
    spin_unlock(&l->lock);
    if(!rc)loom_private_close_changed(l);
    loom_unref(l);return rc;
}

static struct loom_sqe sqe(unsigned opcode) {
 struct loom_sqe s={.opcode=opcode,.handle_idx=child.slot,.user_data=77};
 s._resv1[1]=child.incarnation;s._resv1[3]=100;
 if(opcode==LOOM_OP_READ || opcode==LOOM_OP_WRITE){s.len=4;s.offset=512;s._resv1[0]=32;}
 if(opcode==LOOM_OP_WALK){s.offset=dest.slot;s._resv1[2]=dest.incarnation;}
 return s;
}
static void reset(void) {
 CHECK(roots==1 && !pins && !charged && !allocations,"previous roots balanced");
 memset(&l,0,sizeof(l));memset(&creator,0,sizeof(creator));memset(&protocol,0,sizeof(protocol));
 memset(&slots,0,sizeof(slots));memset(&nodes,0,sizeof(nodes));memset(&pools,0,sizeof(pools));memset(records,0,sizeof(records));
 memset(&backing,0,sizeof(backing));memset(&ring,0,sizeof(ring));memset(storage,0,sizeof(storage));
 backing.type=BURROW_TYPE_ANON;backing.pages=(struct page *)storage;backing.size=sizeof(storage);backing.handle_count=1;
 l.magic=LOOM_MAGIC;l.service_as=&as;l.service_creator=99;l.cq_entries=4;
 l.service_slots=&slots;l.service_nodes=&nodes;l.service_pools=&pools;l.ring=&ring;
 l.service_requests=(struct loom_request_table){.records=records,.capacity=4};
 l.n_reg_buf=1;l.reg_buf[0]=(struct loom_reg_buffer){.burrow=&backing,.kva=storage+128,.len=256};
 creator.magic=PROC_MAGIC;creator.as=&as;creator.stripes=99;
 CHECK(!loom_service_node_prepare(&slots,&nodes,LOOM_SERVICE_SCOPE,(struct loom_service_ref){0},&scope) && !loom_service_node_publish(&slots,&nodes,scope),"scope setup");
 nodes.nodes[scope.slot].state=LOOM_SERVICE_READY;CHECK(!loom_slot_pin(&slots,scope,LOOM_SERVICE_SCOPE),"engine scope pin");
 CHECK(!loom_fid_bind_root(&slots,&nodes,scope,41),"root map");
 CHECK(!loom_service_node_prepare(&slots,&nodes,LOOM_SERVICE_FID,scope,&child) && !loom_service_node_publish(&slots,&nodes,child),"child setup");
 CHECK(!loom_fid_claim_walk(&slots,&nodes,child,scope,987) && !loom_fid_commit_walk(&slots,&nodes,child,scope,987,73) && !loom_fid_release_claim(&slots,&nodes,child,987),"child map");
 CHECK(!loom_fid_claim_open(&slots,&nodes,child,scope,988) && !loom_fid_commit_open(&slots,&nodes,child,scope,988,2) && !loom_fid_release_claim(&slots,&nodes,child,988),"child read/write");
 CHECK(!loom_service_node_prepare(&slots,&nodes,LOOM_SERVICE_FID,scope,&dest) && !loom_service_node_publish(&slots,&nodes,dest),"destination setup");
 protocol.owner=&l;protocol.scope=scope;protocol.indexed=protocol.connect_bound=true;protocol.client.magic=P9_CLIENT_MAGIC;protocol.client.next_fid=100;
 allocation_fault=close_during_alloc=false;drop_watch=NULL;notifications=wire_effects=0;
}
// Admitted destruction is an explicit fixture boundary; the real READY/close
// destructor remains pending. Its prescribed physical-free-before-refund order
// is checked by the allocator/image/entry doubles above.
static void destroy_terminal(struct loom_service_operation *o) {
 struct loom_request_ref r=o->request;
 CHECK(!loom_request_deliver(&l.service_requests,r) && !loom_rpc_unlink_locked(o),"terminal delivery/unlink");
 kfree(o);spin_lock(&as.lock);addrspace_uncharge_pages(&as,1);spin_unlock(&as.lock);
 CHECK(!loom_request_drop_owner(&l.service_requests,r) && !loom_request_reap(&l.service_requests,&slots,r),"physical owner ends");
 CHECK(!loom_engine_put(&l,scope,&protocol),"last entry pin");loom_unref(&l);
}
int main(void) {
 reset();struct loom_sqe s=sqe(LOOM_OP_LOPEN);struct loom_service_operation *o=(void *)1;
 creator.as=&other_as;CHECK(loom_private_io_prepare(&l,&creator,&s,1,&o)==-T_E_ACCES && o==(void *)1 && !pins,"exact creator image refusal");creator.as=&as;
 creator.stripes++;CHECK(loom_private_io_prepare(&l,&creator,&s,1,&o)==-T_E_ACCES,"exact creator incarnation refusal");creator.stripes--;s=sqe(LOOM_OP_READ);
 CHECK(!loom_private_io_prepare(&l,&creator,&s,1,&o) && o->program.wire.payload==storage+160 && o->fixed.extent.offset==160 && backing.handle_count==2 && l.service_fixed_count==1 && roots==3,"prepared canonical slice owns independent pin");
 CHECK(!wire_effects && !protocol.op_count && records[0].phase==LREQ_PREPARED,"prepare has no wire or queue effect");
 s.len=200;s.offset=900;s._resv1[0]=0;
 CHECK(o->program.wire.count==4 && o->program.wire.offset==512 && o->program.wire.payload_capacity==4,"program detached from later SQE edits");
 CHECK(loom_private_operation_release_unused(o)==-T_E_BUSY,"raw unused free refuses fixed hold");
 CHECK(loom_private_io_publish(o,100)==-T_E_TIMEDOUT && o->fixed.owner==&l && !protocol.op_count,"fresh publication deadline retains provisional hold");
 CHECK(!loom_private_io_release_prepared(o) && roots==1 && backing.handle_count==1 && !l.service_fixed_count,"unpublished cleanup frees both roots");

 reset();s=sqe(LOOM_OP_WRITE);CHECK(!loom_private_io_prepare(&l,&creator,&s,1,&o),"write preparation");
 // Simulate replacement dropping the old registered table pin. The accepted
 // extent owns its own pin and never consults the new table for its pointer.
 --backing.handle_count;l.reg_buf[0].burrow=NULL;l.n_reg_buf=0;
 CHECK(o->program.wire.payload==storage+160 && backing.handle_count==1,"table replacement preserves accepted extent");
 CHECK(!loom_private_io_publish(o,2) && protocol.op_head==o && roots==3,"publish transfers owner and balances wake root");
 CHECK(loom_private_io_release_prepared(o)==-T_E_BUSY && loom_private_io_drop_fixed(o)==-T_E_BUSY,"queued ownership cannot release payload");
 protocol.driving=true;CHECK(!loom_rpc_settle_locked(o),"queued cancellation settles");
 CHECK(loom_admission_cancel_locked(o,-T_E_CANCELED,true,4)==-T_E_BUSY,"terminal waits for physical fixed release");
 drop_watch=o;CHECK(!loom_private_io_drop_fixed(o) && !o->program.wire.payload && !o->program.wire.payload_capacity && backing.handle_count==0 && roots==2,"physical payload release clears pointer");drop_watch=NULL;
 CHECK(!loom_admission_cancel_locked(o,-T_E_CANCELED,true,4),"terminal follows actual buffer drop");destroy_terminal(o);

 reset();s=sqe(LOOM_OP_WALK);s.len=3;s._resv1[0]=32;memcpy(storage+160,"abc",3);
 CHECK(!loom_private_io_prepare(&l,&creator,&s,1,&o) && !memcmp(o->program.wire.component,"abc",3) && !o->fixed.owner && backing.handle_count==1 && protocol.client.next_fid==101,"walk owns copied component and burns wire fid");
 memset(storage+160,'z',3);CHECK(!memcmp(o->program.wire.component,"abc",3),"walk never rereads mutable backing");
 CHECK(!loom_private_io_release_prepared(o) && protocol.client.next_fid==101,"rollback never rewinds wire fid");
 reset();s=sqe(LOOM_OP_WALK);s.len=2;memcpy(storage+128,"..",2);o=(void *)1;
 CHECK(loom_private_io_prepare(&l,&creator,&s,1,&o)==-T_E_INVAL && o==(void *)1 && backing.handle_count==1 && roots==1 && !l.service_fixed_count && protocol.client.next_fid==100,"invalid component unwinds before fid allocation");
 reset();s=sqe(LOOM_OP_WALK);protocol.client.next_fid=P9_NOFID;
 CHECK(loom_private_io_prepare(&l,&creator,&s,1,&o)==-T_E_NOSPC && roots==1 && !charged,"exhausted monotone allocator unwinds");
 reset();s=sqe(LOOM_OP_READ);s.len=0;s._resv1[0]=256;
 CHECK(!loom_private_io_prepare(&l,&creator,&s,1,&o) && !o->fixed.owner && !o->program.wire.payload && !o->program.wire.payload_capacity && backing.handle_count==1,"empty extent validates end offset without pin");
 CHECK(!loom_private_io_release_prepared(o),"empty prepared cleanup");s._resv1[0]=257;
 CHECK(loom_private_io_prepare(&l,&creator,&s,1,&o)==-T_E_INVAL && roots==1,"empty extent still bounds offset");
 s._resv1[0]=0;s.buf_idx_or_off=1;l.reg_buf[1]=l.reg_buf[0]; // stale metadata must not authorize an unregistered slot
 CHECK(loom_private_io_prepare(&l,&creator,&s,1,&o)==-T_E_INVAL,"empty extent still validates registration");
 reset();s=sqe(LOOM_OP_READ);allocation_fault=true;
 CHECK(loom_private_io_prepare(&l,&creator,&s,1,&o)==-T_E_NOMEM && roots==1 && !pins && !charged,"allocation failure no fixed side effect");
 reset();s=sqe(LOOM_OP_READ);close_during_alloc=true;
 CHECK(loom_private_io_prepare(&l,&creator,&s,1,&o)==-T_E_CANCELED && roots==1 && !pins && !charged && backing.handle_count==1,"close during allocation unwinds before fixed pin");
 reset();s=sqe(LOOM_OP_READ);s.len=300;
 CHECK(loom_private_io_prepare(&l,&creator,&s,1,&o)==-T_E_INVAL && !l.service_requests.occupied && roots==1,"invalid fixed slice unwinds storage");
 reset();s=sqe(LOOM_OP_READ);s.flags=LOOM_SQE_BUFFER_SELECT;s._resv1[0]=0;s._resv1[2]=1;
 CHECK(loom_private_io_prepare(&l,&creator,&s,1,&o)==-T_E_OPNOTSUPP && !pins,"pooled frontend not silently fixed");
 reset();s=sqe(LOOM_OP_LOPEN);s.offset=2;
 CHECK(!loom_private_io_prepare(&l,&creator,&s,1,&o) && o->program.phase==LPROGRAM_STAT && o->program.wire.mode==2,"open preserves mode and staged DAC");
 CHECK(!loom_private_io_release_prepared(o),"open prepared cleanup");
 printf("PASS fixed SQE preparation: %u checks\n",checks);return 0;
}
