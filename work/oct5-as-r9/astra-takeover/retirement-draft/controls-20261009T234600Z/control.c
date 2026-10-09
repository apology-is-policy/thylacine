#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thylacine/loom_service_retirement.h>
#include <thylacine/loom_service_decode.h>
#include <thylacine/loom.h>
#include <thylacine/loom_service_fids.h>
#include <thylacine/loom_service_engine.h>
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
static struct loom_engine_table engine;
static struct loom_service_ref scope,child,dest;
static struct Burrow backing,ring;
static u8 storage[4096],cq_storage[1024];
void poll_waiter_list_wake(struct poll_waiter_list *w){CHECK(!l.lock.value && w==&l.cq_waiters,"CQ wake outside Ll");}
static int roots=1,pins,charged,allocations,notifications,wire_effects;
static bool allocation_fault,close_during_alloc;
static bool watch_free;
static struct loom_request_ref freeing_request;
static struct loom_service_operation *drop_watch;
int wakeup(struct Rendez *r){(void)r;return 0;}
void srvconn_teardown(struct SrvConn *c){CHECK(c==(struct SrvConn *)&protocol && !l.lock.value,"native teardown outside Ll");}
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
 CHECK(a==&as && a->lock.value && n==1 && charged==1 && !allocations,"refund after actual storage free");
 if(watch_free)CHECK(records[freeing_request.index].owners==1 && records[freeing_request.index].identity==freeing_request.identity,"physical owner retained through refund");
 --charged;
}
static void *kmalloc(size_t n,u32 flags) {
 CHECK(n==PAGE_SIZE && !l.lock.value && charged==1,"charged page allocation");(void)flags;
 if(allocation_fault)return NULL;
 if(close_during_alloc){l.service_closing=true;slots.closing=true;}
 ++allocations;return calloc(1,n);
}
static void kfree(void *ptr) {
 struct loom_service_operation *o=ptr;
 CHECK(allocations==1 && charged==1 && !l.lock.value && !o->fixed.owner && !o->fixed_dropping && !o->pool_stream && !o->source_borrow && !o->fid_claim,"storage free after physical holds");
 if(watch_free){
  freeing_request=o->request;
  CHECK(o->phase==LRPC_RETIRED && records[o->request.index].identity==o->request.identity && records[o->request.index].owners==1 && records[o->request.index].phase==LREQ_DELIVERED,"physical owner retained through kfree");
 }
 --allocations;free(ptr);
}
int p9_client_submit_async(struct p9_client *c,struct p9_rpc *r,p9_session_build_fn build,void *ctx) {
 (void)c;(void)r;(void)build;(void)ctx;++wire_effects;extinction("preparation sent wire");return -P9_E_IO;
}
// Bounded payload-lease state machine. See loom_service_pool.h for lock/pin
// obligations. Private syscall dispatch is not enabled by this module.

static bool same_ref(struct loom_service_ref a, struct loom_service_ref b) {
    return a.slot == b.slot && a.incarnation == b.incarnation;
}
static bool valid_ref(struct loom_service_ref r) {
    return !r.reserved && r.slot < LOOM_SERVICE_SLOT_COUNT && r.incarnation;
}
static bool valid_extent(struct loom_pool_extent e) {
    return e.backing && e.length && e.offset <= UINT64_MAX - e.length;
}
static bool overlaps(struct loom_pool_extent a, struct loom_pool_extent b) {
    // Both extents are validated, so their exclusive ends cannot overflow.
    return a.backing == b.backing && a.offset < b.offset + b.length &&
           b.offset < a.offset + a.length;
}
bool loom_pool_extents_overlap(struct loom_pool_extent a, struct loom_pool_extent b) {
    return !valid_extent(a) || !valid_extent(b) || overlaps(a,b);
}

static bool owns(const struct loom_pool_cell *c, const struct loom_pool *p) {
    return c->phase != LMEMBER_FREE && same_ref(c->pool, p->ref);
}
static const struct loom_pool_cell *member(const struct loom_pool_bank *b,
                                           const struct loom_pool *p, u32 n) {
    for (u32 i = 0; i < LOOM_SERVICE_POOL_MEMBERS; ++i)
        if (owns(&b->cells[i], p) && b->cells[i].ordinal == n)
            return &b->cells[i];
    return NULL;
}
static int receipt_cell(struct loom_pool_bank *b, const struct loom_pool *p,
                        const struct loom_service_buffer_receipt *r,
                        u32 phase, struct loom_pool_cell **out) {
    if (r->reserved || !valid_ref(r->pool) || r->member >= p->count)
        return -T_E_INVAL;
    if (p->phase != LPOOL_LIVE || !same_ref(r->pool, p->ref) || !r->lease)
        return -T_E_NOENT;
    struct loom_pool_cell *c = (struct loom_pool_cell *)member(b, p, r->member);
    if (!c || c->phase != phase || c->nonce != r->lease) return -T_E_NOENT;
    *out = c;
    return 0;
}
bool loom_pool_conflicts(const struct loom_pool_bank *b, struct loom_pool_extent e) {
    // Invalid intervals must never be used as an exclusion bypass.
    if (!valid_extent(e)) return true;
    for (u32 i = 0; i < LOOM_SERVICE_POOL_MEMBERS; ++i)
        if (b->cells[i].phase != LMEMBER_FREE && overlaps(e, b->cells[i].extent))
            return true;
    return false;
}
int loom_pool_prepare(struct loom_pool_bank *b, struct loom_pool *p,
                      struct loom_service_ref ref, const struct loom_pool_extent *e,
                      u32 count) {
    if (p->phase != LPOOL_EMPTY) return -T_E_BUSY;
    if (!valid_ref(ref) || !e || !count || count > LOOM_SERVICE_POOL_MEMBERS)
        return -T_E_INVAL;
    u32 free = 0;
    for (u32 i = 0; i < LOOM_SERVICE_POOL_MEMBERS; ++i) {
        if (b->cells[i].phase == LMEMBER_FREE) ++free;
        else if (same_ref(b->cells[i].pool, ref)) return -T_E_INVAL;
    }
    if (free < count) return -T_E_NOSPC;
    for (u32 i = 0; i < count; ++i) {
        if (!valid_extent(e[i])) return -T_E_INVAL;
        if (loom_pool_conflicts(b, e[i])) return -T_E_BUSY;
        for (u32 j = 0; j < i; ++j)
            if (overlaps(e[i], e[j])) return -T_E_INVAL;
    }
    // All validation precedes mutation. Provisional cells immediately exclude
    // both competing registrations and ordinary fixed-buffer admissions.
    u32 n = 0;
    for (u32 i = 0; i < LOOM_SERVICE_POOL_MEMBERS && n < count; ++i) {
        struct loom_pool_cell *c = &b->cells[i];
        if (c->phase != LMEMBER_FREE) continue;
        *c = (struct loom_pool_cell){ .pool = ref, .extent = e[n],
                     .ordinal = n, .phase = LMEMBER_AVAILABLE };
        ++n;
    }
    *p = (struct loom_pool){ .ref = ref, .phase = LPOOL_RESERVED, .count = count };
    return 0;
}
int loom_pool_publish(struct loom_pool *p) {
    if (p->phase != LPOOL_RESERVED) return -T_E_INVAL;
    p->phase = LPOOL_LIVE;
    return 0;
}
static void remove_pool(struct loom_pool_bank *b, struct loom_pool *p) {
    for (u32 i = 0; i < LOOM_SERVICE_POOL_MEMBERS; ++i)
        if (owns(&b->cells[i], p)) b->cells[i] = (struct loom_pool_cell){0};
    // Pins are the caller's responsibility: drop them outside the ring lock
    // only after this exclusion removal and after all local writers ended.
    *p = (struct loom_pool){0};
}
int loom_pool_rollback(struct loom_pool_bank *b, struct loom_pool *p) {
    if (p->phase != LPOOL_RESERVED) return -T_E_INVAL;
    remove_pool(b, p);
    return 0;
}
int loom_pool_stream_get(struct loom_pool *p) {
    if (p->phase != LPOOL_LIVE) return -T_E_NOENT;
    if (p->streams == UINT32_MAX) return -T_E_NOSPC;
    ++p->streams;
    return 0;
}
int loom_pool_stream_put(struct loom_pool *p) {
    if (p->phase != LPOOL_LIVE || !p->streams) return -T_E_INVAL;
    --p->streams;
    return 0;
}
int loom_pool_claim(struct loom_pool_bank *b, struct loom_pool *p, u32 maximum,
                    struct loom_service_buffer_receipt *out) {
    if (p->phase != LPOOL_LIVE) return -T_E_NOENT;
    if (!p->streams || !maximum || maximum > INT32_MAX) return -T_E_INVAL;
    for (u32 i = 0; i < LOOM_SERVICE_POOL_MEMBERS; ++i)
        if (owns(&b->cells[i], p) && maximum > b->cells[i].extent.length)
            return -T_E_INVAL;
    // Exhaustion is terminal even with an empty pool; do not wait for a return
    // that can never make another nonce available.
    if (b->next_nonce == UINT64_MAX) return -T_E_NOSPC;
    for (u32 n = 0; n < p->count; ++n) {
        u32 ordinal = (p->cursor + n) % p->count;
        struct loom_pool_cell *c = (struct loom_pool_cell *)member(b, p, ordinal);
        if (!c || c->phase != LMEMBER_AVAILABLE) continue;
        c->nonce = ++b->next_nonce;
        c->phase = LMEMBER_BUSY;
        c->requested = maximum;
        c->result = 0; c->user_data = 0; c->more = false;
        p->cursor = (ordinal + 1) % p->count;
        *out = (struct loom_service_buffer_receipt){ .pool = p->ref,
                                     .member = ordinal, .lease = c->nonce };
        return 0;
    }
    return -T_E_AGAIN; // wait locally; caller must not emit a Tread
}
int loom_pool_release_busy(struct loom_pool_bank *b, const struct loom_pool *p,
                           const struct loom_service_buffer_receipt *r) {
    struct loom_pool_cell *c;
    int err = receipt_cell(b, p, r, LMEMBER_BUSY, &c);
    if (err) return err;
    c->phase = LMEMBER_AVAILABLE; // nonce remains burned
    return 0;
}
int loom_pool_commit(struct loom_pool_bank *b, const struct loom_pool *p,
                     const struct loom_service_buffer_receipt *r, u32 length,
                     u64 user_data, bool more) {
    struct loom_pool_cell *c;
    int err = receipt_cell(b, p, r, LMEMBER_BUSY, &c);
    if (err) return err;
    if (!length || length > c->requested) return -T_E_INVAL;
    c->result = length; c->user_data = user_data; c->more = more;
    c->phase = LMEMBER_PENDING;
    return 0;
}
int loom_pool_peek(const struct loom_pool_bank *b, const struct loom_pool *p,
                   u32 ordinal, struct loom_pool_result *out) {
    if (p->phase != LPOOL_LIVE) return -T_E_NOENT;
    if (ordinal >= p->count) return -T_E_INVAL;
    const struct loom_pool_cell *c = member(b, p, ordinal);
    if (!c || c->phase != LMEMBER_PENDING) return -T_E_NOENT;
    *out = (struct loom_pool_result){
        .receipt = { .pool = p->ref, .member = ordinal, .lease = c->nonce },
        .user_data = c->user_data, .length = c->result, .more = c->more };
    return 0;
}
int loom_pool_deliver(struct loom_pool_bank *b, const struct loom_pool *p,
                      const struct loom_service_buffer_receipt *r) {
    struct loom_pool_cell *c;
    int err = receipt_cell(b, p, r, LMEMBER_PENDING, &c);
    if (err) return err;
    c->phase = LMEMBER_LEASED;
    return 0;
}
int loom_pool_return(struct loom_pool_bank *b, const struct loom_pool *p,
                     const struct loom_service_buffer_receipt *r) {
    struct loom_pool_cell *c;
    int err = receipt_cell(b, p, r, LMEMBER_LEASED, &c);
    if (err) return err;
    c->phase = LMEMBER_AVAILABLE;
    return 0;
}
int loom_pool_snapshot(const struct loom_pool_bank *b, const struct loom_pool *p,
                       struct loom_service_pool_snapshot *out) {
    if (p->phase != LPOOL_LIVE) return -T_E_NOENT;
    *out = (struct loom_service_pool_snapshot){ .size = sizeof(*out),
         .version = LOOM_SERVICE_ABI_VERSION, .pool = p->ref,
         .members = p->count, .streams = p->streams };
    for (u32 i = 0; i < LOOM_SERVICE_POOL_MEMBERS; ++i) {
        const struct loom_pool_cell *c = &b->cells[i];
        if (!owns(c, p)) continue;
        switch (c->phase) {
        case LMEMBER_AVAILABLE: ++out->available; break;
        case LMEMBER_BUSY: ++out->busy; break;
        case LMEMBER_PENDING: ++out->pending; break;
        case LMEMBER_LEASED: ++out->leased; break;
        default: return -T_E_IO;
        }
    }
    return 0;
}
int loom_pool_reap(struct loom_pool_bank *b, struct loom_pool *p) {
    if (p->phase != LPOOL_LIVE) return -T_E_NOENT;
    if (p->streams) return -T_E_BUSY;
    for (u32 i = 0; i < LOOM_SERVICE_POOL_MEMBERS; ++i)
        if (owns(&b->cells[i], p) && b->cells[i].phase != LMEMBER_AVAILABLE)
            return -T_E_BUSY;
    remove_pool(b, p);
    return 0;
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
            burrow_ref((struct Burrow *)e.backing);
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
        op->phase==LRPC_NEW && !op->next && !op->prev && !op->submitted && !op->fid_claim && !op->source_borrow && !op->fixed.owner && !op->pool_stream && !op->fixed_dropping;
    spin_unlock(&l->lock);
    if (!unused) return -T_E_BUSY;
    kfree(op);
    operation_refund(l);
    operation_unreserve(l,request);
    operation_put(l,scope,p);
    return 0;
}

int loom_private_operation_release_retired(struct loom_service_operation *op) {
    if(!op || !op->owner || !op->protocol)return -T_E_INVAL;
    struct Loom *l=op->owner;struct loom_service_protocol *p=op->protocol;
    struct loom_request_ref request=op->request;struct loom_service_ref scope=op->scope;
    spin_lock(&l->lock);
    if(!p->driving || op->phase!=LRPC_TERMINAL || op->fid_claim ||
       op->source_borrow || op->fixed.owner || op->fixed_dropping ||
       op->pool_busy || op->pool_pending || op->rpc.sending || op->rpc.on_complete) {
        spin_unlock(&l->lock);return -T_E_BUSY;
    }
    int rc=loom_rpc_unlink_locked(op);
    if(!rc && op->pool_stream && loom_provided_detach_locked(op))
        extinction("retired operation lost pool stream");
    spin_unlock(&l->lock);if(rc)return rc;
    kfree(op);
    operation_refund(l);
    spin_lock(&l->lock);
    if(loom_request_drop_owner(&l->service_requests,request) ||
       loom_request_reap(&l->service_requests,l->service_slots,request))
        extinction("operation lost retired physical owner");
    spin_unlock(&l->lock);
    operation_put(l,scope,p);return 0;
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
int loom_rpc_check_begin_locked(struct loom_service_operation *o,u64 now) {
    struct loom_service_request *r=rpc_record(o);
    if(!r)return -T_E_NOENT;
    if(!o->protocol->driving || o->phase!=LRPC_QUEUED ||
       r->phase!=LREQ_ACTIVE || r->borrows!=1 || o->rpc.sending)return -T_E_BUSY;
    int rc=rpc_ready(o);if(rc)return rc;
    if(o->capture_error || o->callback_seen)return -T_E_IO;
    if(r->deadline && r->deadline<=now)return -T_E_TIMEDOUT;
    return 0;
}
int loom_rpc_begin_locked(struct loom_service_operation *o,u64 now) {
    int rc=loom_rpc_check_begin_locked(o,now);if(rc)return rc;
    if(o->pool_stream && !o->pool_busy)return -T_E_BUSY;
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

int loom_rpc_rearm_read_locked(struct loom_service_operation *o,u64 now) {
    if(!o || !o->pool_stream || !o->multishot || !o->pool_more_delivered || o->pool_busy ||
       o->pool_pending || o->pool_terminal || o->program.operation!=P9_TREAD ||
       o->program.result<=0)return -T_E_BUSY;
    int rc=loom_rpc_check_finish_locked(o,o->program.result,now);if(rc)return rc;
    // One continuous request/source borrow across shots. Offset and identity
    // are unchanged; only the completed attempt is reset after CQ delivery.
    o->pool_more_delivered=false;
    o->program.phase=LPROGRAM_EFFECT;o->program.result=0;
    o->program.wire.built=false;o->program.wire.local_again=false;
    o->callback_seen=false;o->capture_error=0;o->submitted=false;o->advanced=false;
    o->phase=LRPC_QUEUED;return 0;
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
    if(o->fixed.owner || o->fixed_dropping || o->pool_busy)return -T_E_BUSY;
    if(o->pool_stream && o->program.result>0 &&
       (!o->pool_terminal || !o->pool_pending))return -T_E_BUSY;
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
    if(o->fixed.owner || o->fixed_dropping || o->pool_busy)return -T_E_BUSY;
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
    if(!o || !o->owner || o->pool_stream || o->fixed_dropping || o->rpc.sending ||
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

static bool provided_same(struct loom_service_ref a,struct loom_service_ref b) {
    return a.slot==b.slot && a.incarnation==b.incarnation && !a.reserved && !b.reserved;
}
static struct loom_pool *provided_pool(struct Loom *l,struct loom_service_ref ref) {
    if(!l || !l->service_pools || !l->service_slots || ref.reserved ||
       !ref.incarnation || ref.slot>=LOOM_SERVICE_SLOT_COUNT)return NULL;
    struct loom_service_slot *s=&l->service_slots->slots[ref.slot];
    struct loom_pool *p=&l->service_pools->pools[ref.slot];
    if(s->kind!=LOOM_SERVICE_POOL || s->phase!=LSLOT_LIVE ||
       s->incarnation!=ref.incarnation || p->phase!=LPOOL_LIVE ||
       !provided_same(p->ref,ref))return NULL;
    return p;
}
static struct loom_pool_cell *provided_cell(struct loom_service_operation *o) {
    if(!o->owner->service_pools || !provided_same(o->receipt.pool,o->pool) ||
       o->receipt.reserved || !o->receipt.lease)return NULL;
    struct loom_pool_bank *b=&o->owner->service_pools->bank;
    for(u32 i=0;i<LOOM_SERVICE_POOL_MEMBERS;++i) {
        struct loom_pool_cell *c=&b->cells[i];
        if(c->phase!=LMEMBER_FREE && provided_same(c->pool,o->pool) &&
           c->ordinal==o->receipt.member && c->nonce==o->receipt.lease)return c;
    }
    return NULL;
}
static int provided_bounds(struct loom_pool_extent e,u32 maximum,u8 **view) {
    struct Burrow *b=(void *)e.backing;
    if(!b || b->type!=BURROW_TYPE_ANON || !b->pages || !maximum ||
       maximum>e.length || e.offset>b->size || e.length>b->size-e.offset)
        return -T_E_INVAL;
    if(view)*view=(u8 *)pa_to_kva(page_to_pa(b->pages))+e.offset;
    return 0;
}
static void provided_clear(struct loom_service_operation *o) {
    o->program.wire.payload=NULL;o->program.wire.payload_capacity=0;
}
int loom_provided_detach_locked(struct loom_service_operation *o) {
    if(!o || !o->pool_stream || o->pool_busy || o->pool_pending ||
       (o->phase!=LRPC_NEW && o->phase!=LRPC_RETIRED) || o->rpc.sending ||
       o->rpc.on_complete)return -T_E_BUSY;
    struct loom_pool *p=provided_pool(o->owner,o->pool);
    if(!p)return -T_E_NOENT;
    int rc=loom_pool_stream_put(p);if(rc)return rc;
    o->pool_stream=false;provided_clear(o);return 0;
}
int loom_provided_release_prepared(struct loom_service_operation *o) {
    if(!o || o->phase!=LRPC_NEW || o->source_borrow || o->fid_claim)return -T_E_BUSY;
    spin_lock(&o->owner->lock);int rc=loom_provided_detach_locked(o);
    spin_unlock(&o->owner->lock);if(rc)return rc;
    return loom_private_operation_release_unused(o);
}
int loom_provided_prepare(struct Loom *l,struct Proc *p,const struct loom_sqe *s,
                          u64 now,struct loom_service_operation **out) {
    if(!l || l->magic!=LOOM_MAGIC || !loom_is_private(l) || !p ||
       p->magic!=PROC_MAGIC || !s || !out)return -T_E_INVAL;
    if(p->as!=l->service_as || p->stripes!=l->service_creator)return -T_E_ACCES;
    int rc=loom_private_sqe_shape(s,l->receipt_size!=0);if(rc)return rc;
    if(s->opcode!=LOOM_OP_READ || !(s->flags&LOOM_SQE_BUFFER_SELECT) ||
       s->len>INT32_MAX)return -T_E_INVAL;
    struct loom_service_ref source={.slot=s->handle_idx,.incarnation=s->_resv1[1]},scope;
    struct loom_service_ref ref={.slot=s->buf_idx_or_off,.incarnation=s->_resv1[2]};
    spin_lock(&l->lock);rc=loom_admission_scope_locked(l,source,&scope);
    spin_unlock(&l->lock);if(rc)return rc;
    struct loom_service_operation *o;
    rc=loom_private_operation_prepare(l,scope,s->user_data,s->_resv1[3],now,&o);
    if(rc)return rc;
    o->source=source;o->pool=ref;o->multishot=(s->flags&LOOM_SQE_MULTISHOT)!=0;
    o->program.wire.count=s->len;o->program.wire.offset=s->offset;
    rc=loom_private_program_start(&o->program,&o->program.identity,P9_TREAD);
    spin_lock(&l->lock);
    struct loom_pool *pool=provided_pool(l,ref);
    if(!rc && (l->service_closing || l->service_slots->closing))rc=-T_E_CANCELED;
    if(!rc && !pool)rc=-T_E_NOENT;
    u32 count=0;
    for(u32 i=0;!rc && i<LOOM_SERVICE_POOL_MEMBERS;++i) {
        struct loom_pool_cell *c=&l->service_pools->bank.cells[i];
        if(c->phase==LMEMBER_FREE || !provided_same(c->pool,ref))continue;
        ++count;rc=provided_bounds(c->extent,s->len,NULL);
    }
    if(!rc && count!=pool->count)rc=-T_E_IO;
    if(!rc){rc=loom_pool_stream_get(pool);if(!rc)o->pool_stream=true;}
    spin_unlock(&l->lock);
    if(rc){if(loom_private_operation_release_unused(o))extinction("provided prepare unwind");return rc;}
    *out=o;return 0;
}
int loom_provided_publish(struct loom_service_operation *o,u64 now) {
    if(!o || !o->pool_stream || o->pool_busy || o->pool_pending)return -T_E_INVAL;
    struct Loom *l=o->owner;loom_ref(l);spin_lock(&l->lock);
    int rc=loom_admission_enqueue_locked(o,o->source,o->destination,now);
    spin_unlock(&l->lock);if(!rc)loom_private_close_changed(l);loom_unref(l);return rc;
}
int loom_provided_begin_locked(struct loom_service_operation *o,u64 now) {
    if(!o || !o->pool_stream || o->pool_pending || o->pool_terminal ||
       o->program.operation!=P9_TREAD)return -T_E_BUSY;
    int rc=loom_rpc_check_begin_locked(o,now);if(rc)return rc;
    struct loom_pool *p=provided_pool(o->owner,o->pool);if(!p)return -T_E_NOENT;
    struct loom_pool_bank *b=&o->owner->service_pools->bank;
    bool fresh=!o->pool_busy;
    if(fresh){rc=loom_pool_claim(b,p,o->program.wire.count,&o->receipt);if(rc)return rc;o->pool_busy=true;}
    struct loom_pool_cell *c=provided_cell(o);u8 *view=NULL;
    rc=(!c || c->phase!=LMEMBER_BUSY)?-T_E_IO:provided_bounds(c->extent,o->program.wire.count,&view);
    if(rc) {
        if(fresh){if(loom_pool_release_busy(b,p,&o->receipt))extinction("provided invalid view cleanup");o->pool_busy=false;}
        return rc;
    }
    o->program.wire.payload=view;o->program.wire.payload_capacity=o->program.wire.count;
    if(loom_rpc_begin_locked(o,now))extinction("provided begin changed inside Ll");
    return 0;
}
int loom_provided_release_busy_locked(struct loom_service_operation *o) {
    if(!o || !o->pool_stream || !o->pool_busy || o->pool_pending ||
       o->phase!=LRPC_SETTLING || !o->protocol->driving ||
       o->rpc.sending || o->rpc.on_complete)return -T_E_BUSY;
    struct loom_pool *p=provided_pool(o->owner,o->pool);if(!p)return -T_E_NOENT;
    int rc=loom_pool_release_busy(&o->owner->service_pools->bank,p,&o->receipt);
    if(!rc){o->pool_busy=false;provided_clear(o);}return rc;
}
int loom_provided_commit_locked(struct loom_service_operation *o,u64 now) {
    if(!o || !o->pool_stream || !o->pool_busy || o->pool_pending ||
       !o->source_borrow || o->program.operation!=P9_TREAD || o->program.result<0)return -T_E_BUSY;
    int rc=loom_rpc_check_finish_locked(o,o->program.result,now);if(rc)return rc;
    if(!o->program.result) {
        rc=loom_provided_release_busy_locked(o);if(rc)return rc;
        return loom_admission_commit_locked(o,now);
    }
    struct loom_pool *p=provided_pool(o->owner,o->pool);if(!p)return -T_E_NOENT;
    struct loom_service_request *r=&o->owner->service_requests.records[o->request.index];
    rc=loom_pool_commit(&o->owner->service_pools->bank,p,&o->receipt,
                       (u32)o->program.result,r->user_data,o->multishot);
    if(rc)return rc;
    o->pool_busy=false;o->pool_pending=true;provided_clear(o);
    if(!o->multishot) {
        o->pool_terminal=true;
        if(loom_admission_commit_locked(o,now))extinction("provided single terminal lost");
    }
    return 0;
}
int loom_provided_deliver(struct loom_service_operation *o) {
    if(!o || !o->owner)return -T_E_INVAL;
    struct Loom *l=o->owner;spin_lock(&l->lock);int rc=0;
    struct loom_pool *p=provided_pool(l,o->pool);
    struct loom_pool_cell *c=provided_cell(o);
    if(!o->pool_stream || !o->pool_pending || !p || !c ||
       c->phase!=LMEMBER_PENDING){rc=-T_E_NOENT;goto done;}
    if(l->service_requests.detached){rc=-T_E_CANCELED;goto done;}
    // Single-shot terminal and payload delivery share one CQ entry. No second
    // non-leased terminal can be posted, even after source abort/retirement.
    if(o->pool_terminal) {
        u64 user;s32 result;
        rc=loom_request_peek(&l->service_requests,o->request,&user,&result);
        if(rc || user!=c->user_data || result!=(s32)c->result){if(!rc)rc=-T_E_IO;goto done;}
    } else if(!o->multishot || (o->phase!=LRPC_SETTLING && o->phase!=LRPC_TERMINAL)) {
        rc=-T_E_BUSY;goto done;
    }
    struct loom_ring_hdr *h=(void *)(l->ring_kva+l->hdr_off);
    u32 tail=l->cq_tail,head=__atomic_load_n(&h->cq_head,__ATOMIC_ACQUIRE);
    if((u32)(tail-head)>=l->cq_entries){rc=-T_E_AGAIN;goto done;}
    u32 idx=tail&(l->cq_entries-1u);
    struct loom_cqe *cq=(void *)(l->ring_kva+l->cqe_off);
    struct loom_service_buffer_receipt *receipts=(void *)(l->ring_kva+l->receipt_off);
    cq[idx]=(struct loom_cqe){.user_data=c->user_data,.result=(s32)c->result,
        .flags=LOOM_CQE_SERVICE_BUFFER|(c->more?LOOM_CQE_MORE:0)};
    receipts[idx]=o->receipt;
    if(loom_pool_deliver(&l->service_pools->bank,p,&o->receipt))extinction("provided delivery changed inside Ll");
    if(o->pool_terminal && loom_request_deliver(&l->service_requests,o->request))extinction("provided terminal delivery lost");
    o->pool_pending=false;o->pool_more_delivered=!o->pool_terminal;
    l->cq_tail=tail+1u;
    __atomic_store_n(&h->cq_tail,l->cq_tail,__ATOMIC_RELEASE);
done:
    spin_unlock(&l->lock);if(!rc)poll_waiter_list_wake(&l->cq_waiters);return rc;
}

int loom_provided_discard_closed_locked(struct loom_service_operation *o) {
    if(!o || !o->owner || !o->protocol)return -T_E_INVAL;
    struct Loom *l=o->owner;struct loom_service_protocol *p=o->protocol;
    if(!l->service_closing || !l->service_slots->closing || !p->driving ||
       !p->rpc_quiesced || !p->abort_reason || o->pool_busy ||
       o->rpc.sending || o->rpc.on_complete ||
       (o->phase!=LRPC_SETTLING && o->phase!=LRPC_TERMINAL))return -T_E_BUSY;
    if(!o->pool_pending)return 0;
    struct loom_pool_cell *c=provided_cell(o);
    if(!o->pool_stream || !c || c->phase!=LMEMBER_PENDING)return -T_E_NOENT;
    // Actual close has detached this consumer. All writer users ended; this
    // exact PENDING record was never leased. Do not touch any LEASED member.
    c->phase=LMEMBER_AVAILABLE;o->pool_pending=false;o->pool_more_delivered=false;
    provided_clear(o);return 0;
}

int loom_retirement_quiesce(struct loom_service_protocol *p) {
    if(!p || !p->owner)return -T_E_INVAL;
    struct Loom *l=p->owner;spin_lock(&l->lock);
    if(!p->driving || !p->indexed || !p->connect_bound || p->abort_reason>=0 ||
       p->client.magic!=P9_CLIENT_MAGIC || p->client.progress!=&p->progress ||
       !p->admission.conn) {spin_unlock(&l->lock);return -T_E_BUSY;}
    bool done=p->rpc_quiesced;spin_unlock(&l->lock);if(done)return 0;
    srvconn_teardown(p->admission.conn);
    p9_client_progress_abort(&p->client);
    // Sole driver has returned from all callbacks. An abort flag by itself
    // does not certify these cursors or RPC container users have ended.
    spin_lock(&l->lock);
    if(p->progress.sending || p->progress.io.tx)extinction("abort retained TX cursor");
    for(u32 i=0;i<P9_SESSION_MAX_OUTSTANDING;++i)
        if(p->client.inflight[i])extinction("abort retained RPC callback");
    p->rpc_quiesced=true;spin_unlock(&l->lock);return 0;
}
int loom_retirement_close_operation(struct loom_service_operation *o,u64 now) {
    if(!o || !o->owner || !o->protocol)return -T_E_INVAL;
    struct Loom *l=o->owner;struct loom_service_protocol *p=o->protocol;
    spin_lock(&l->lock);
    if(!l->service_closing || !l->service_slots->closing || !p->driving ||
       !p->rpc_quiesced || p->abort_reason>=0 || o->phase==LRPC_NEW) {
        spin_unlock(&l->lock);return -T_E_BUSY;
    }
    int rc=0;
    if(o->phase==LRPC_QUEUED || o->phase==LRPC_INFLIGHT)rc=loom_rpc_settle_locked(o);
    if(!rc && o->pool_pending)rc=loom_provided_discard_closed_locked(o);
    if(!rc && o->pool_busy)rc=loom_provided_release_busy_locked(o);
    spin_unlock(&l->lock);if(rc)return rc;
    if(o->fixed.owner || o->fixed_dropping){rc=loom_private_io_drop_fixed(o);if(rc)return rc;}
    spin_lock(&l->lock);
    if(o->phase==LRPC_SETTLING)rc=loom_admission_cancel_locked(o,p->abort_reason,p->rpc_quiesced,now);
    if(!rc && o->phase!=LRPC_TERMINAL)rc=-T_E_BUSY;
    if(!rc) {
        struct loom_service_request *r=&l->service_requests.records[o->request.index];
        if(r->phase!=LREQ_DELIVERED)
            rc=loom_request_discard_closed_owner(&l->service_requests,l->service_slots,o->request);
    }
    spin_unlock(&l->lock);if(rc)return rc;
    return loom_private_operation_release_retired(o);
}

int loom_retirement_post_terminal(struct loom_service_operation *o) {
    if(!o || !o->owner || !o->protocol)return -T_E_INVAL;
    struct Loom *l=o->owner;spin_lock(&l->lock);
    if(!o->protocol->driving || o->phase!=LRPC_TERMINAL || o->pool_pending ||
       o->pool_busy || o->fixed.owner || o->fixed_dropping) {
        spin_unlock(&l->lock);return -T_E_BUSY;
    }
    // The actual combined publisher already delivered a single-shot lease.
    bool combined=o->pool_terminal;spin_unlock(&l->lock);
    return combined?0:loom_private_post_terminal(l,o->request);
}

int loom_private_post_terminal(struct Loom *l, struct loom_request_ref ref) {
    if (!l || l->magic != LOOM_MAGIC || !loom_is_private(l)) return -T_E_INVAL;
    spin_lock(&l->lock);
    struct loom_request_table *t = &l->service_requests;
    u64 user_data; s32 result;
    int rc = loom_request_peek(t,ref,&user_data,&result);
    if (rc) { spin_unlock(&l->lock); return rc; }
    if (t->detached) { spin_unlock(&l->lock); return -T_E_CANCELED; }
    struct loom_ring_hdr *h = (void *)(l->ring_kva+l->hdr_off);
    u32 tail = l->cq_tail;
    u32 head = __atomic_load_n(&h->cq_head,__ATOMIC_ACQUIRE);
    if ((u32)(tail-head) >= l->cq_entries) {
        spin_unlock(&l->lock);
        return -T_E_AGAIN;
    }
    u32 idx = tail & (l->cq_entries-1u);
    struct loom_cqe *cq = (void *)(l->ring_kva+l->cqe_off);
    cq[idx] = (struct loom_cqe){ .user_data=user_data, .result=result };
    if (l->receipt_size) {
        struct loom_service_buffer_receipt *receipts = (void *)(l->ring_kva+l->receipt_off);
        receipts[idx] = (struct loom_service_buffer_receipt){0};
    }
    // No abort check here: a terminal committed before abort stays truthful.
    // Copy, mark delivered, and publish the tail under one lock interval.
    rc = loom_request_deliver(t,ref);
    if (rc) { spin_unlock(&l->lock); return rc; }
    l->cq_tail = tail+1u;
    __atomic_store_n(&h->cq_tail,l->cq_tail,__ATOMIC_RELEASE);
    spin_unlock(&l->lock);
    poll_waiter_list_wake(&l->cq_waiters);
    return 0;
}
static void client_mark_dead_locked(struct p9_client *c, bool devgone) {
    int async_status = devgone ? -P9_E_NODEV : -P9_E_IO;
    c->dead = true;
    if (c->progress) {
        // Completion may free an RPC. Drop the TX pointer before callbacks;
        // terminal teardown never waits for a frame, Tflush or Tclunk.
        struct p9_rpc *sending = c->progress->sending;
        c->progress->sending = NULL;
        p9_handshake_progress_abort(&c->progress->handshake);
        p9_transport_progress_abort(&c->progress->io);
        // The progress cursor no longer borrows its frame. End the RPC's TX
        // hold before callbacks can settle/free it, including an early-replied
        // RPC already removed from inflight[]. Legacy blocking senders retain
        // their existing ownership; this edge is progress-mode only.
        if (sending) sending->sending = false;
    }
    for (u32 tag = 0; tag < P9_SESSION_MAX_OUTSTANDING; tag++) {
        struct p9_rpc *r = c->inflight[tag];
        if (!r) continue;
        r->dead = true;
        if (r->on_complete) {
            // Async (POST_CQE, Loom): there is no submitter to wake. Clear the
            // slot + complete the op with an error CQE carrying the reason. The
            // callback runs under c->lock and MUST NOT sleep (seam contract).
            c->inflight[tag] = NULL;
            r->on_complete(r, async_status, NULL);
        } else {
            wakeup(&r->rendez);
        }
    }
    // #349 SA-1: also wake every sender parked on c2s back-pressure
    // (client_send_flow). They sleep on their OWN rendez, NOT their rpc->rendez,
    // so the per-rpc wake above does not reach them; their cond reads c->dead (now
    // true) -> each returns -EIO. A death from a path with no subsequent
    // reader-clear signal (e.g. the c2s send-break in client_run) would otherwise
    // leave them parked indefinitely. (mark_dead is the SOLE c->dead setter.)
    if (c->send_waiters) poll_waiter_list_wake(&c->send_waiters_list);
}
void p9_client_progress_abort(struct p9_client *c) {
    if (!c || c->magic != P9_CLIENT_MAGIC) return;
    spin_lock(&c->lock);
    if (c->progress) client_mark_dead_locked(c, false);
    spin_unlock(&c->lock);
}
bool loom_engine_empty(const struct loom_service_engine *t) {
    if (!t || t->count || t->wait_active || t->wait_mask) return false;
    for (u32 i=0;i<LOOM_SERVICE_SLOT_COUNT;++i) {
        const struct loom_engine_entry *e=&t->entries[i];
        if (e->phase || e->protocol || e->pins || e->incarnation) return false;
    }
    return true;
}
static int loom_private_pools_destroy(struct Loom *l) {
    if(!l || !l->service_pools || !l->service_slots || !l->service_engine ||
       __atomic_load_n(&l->refcount,__ATOMIC_ACQUIRE) || !l->service_closing ||
       !l->service_slots->closing || !loom_engine_empty(l->service_engine) ||
       !loom_requests_empty(&l->service_requests) || l->service_fixed ||
       l->service_fixed_count || l->service_fixed_settling)return -T_E_BUSY;
    struct loom_service_pool_table *t=l->service_pools;
    struct Burrow *pins[LOOM_SERVICE_POOL_MEMBERS];u32 count=0;
    // Validate all records before changing any; no partially reclaimed bank.
    for(u32 i=0;i<LOOM_SERVICE_SLOT_COUNT;++i) {
        struct loom_pool *p=&t->pools[i];struct loom_service_slot *s=&l->service_slots->slots[i];
        if(p->phase==LPOOL_EMPTY) {
            if(s->phase!=LSLOT_FREE && s->kind==LOOM_SERVICE_POOL)return -T_E_INVAL;
            continue;
        }
        if(p->streams || s->pins)return -T_E_BUSY;
        if(p->ref.slot!=i || p->ref.reserved || !p->ref.incarnation ||
           s->kind!=LOOM_SERVICE_POOL || s->incarnation!=p->ref.incarnation ||
           !p->count || p->count>LOOM_SERVICE_POOL_MEMBERS ||
           !((p->phase==LPOOL_RESERVED && s->phase==LSLOT_PROVISIONAL) ||
             (p->phase==LPOOL_LIVE && s->phase==LSLOT_LIVE)))return -T_E_INVAL;
        u32 found=0;u64 ordinals=0;
        for(u32 j=0;j<LOOM_SERVICE_POOL_MEMBERS;++j) {
            struct loom_pool_cell *c=&t->bank.cells[j];
            if(c->phase==LMEMBER_FREE || c->pool.slot!=i)continue;
            if(c->pool.reserved || c->pool.incarnation!=p->ref.incarnation ||
               c->ordinal>=p->count || (ordinals&(1ull<<c->ordinal)) || !c->extent.backing)
                return -T_E_INVAL;
            if(c->phase!=LMEMBER_AVAILABLE && c->phase!=LMEMBER_LEASED)return -T_E_BUSY;
            if(p->phase==LPOOL_RESERVED && c->phase!=LMEMBER_AVAILABLE)return -T_E_INVAL;
            ordinals|=1ull<<c->ordinal;++found;
        }
        if(found!=p->count)return -T_E_INVAL;
    }
    for(u32 i=0;i<LOOM_SERVICE_POOL_MEMBERS;++i) {
        struct loom_pool_cell *c=&t->bank.cells[i];if(c->phase==LMEMBER_FREE)continue;
        if(c->pool.slot>=LOOM_SERVICE_SLOT_COUNT ||
           t->pools[c->pool.slot].phase==LPOOL_EMPTY)return -T_E_INVAL;
        pins[count++]=(struct Burrow *)c->extent.backing;
    }
    for(u32 i=0;i<LOOM_SERVICE_POOL_MEMBERS;++i)
        if(t->bank.cells[i].phase==LMEMBER_LEASED)t->bank.cells[i].phase=LMEMBER_AVAILABLE;
    for(u32 i=0;i<LOOM_SERVICE_SLOT_COUNT;++i) {
        struct loom_pool *p=&t->pools[i];if(p->phase==LPOOL_EMPTY)continue;
        int rc=p->phase==LPOOL_RESERVED?loom_service_pool_rollback(l->service_slots,t,p->ref)
                                      :loom_service_pool_reap(l->service_slots,t,p->ref);
        if(rc)extinction("exclusive pool destruction changed ledger");
    }
    // Ledger exclusions are gone but saved pins keep every backing alive until
    // its physical settled drop. Never turn this into a metadata-only memset.
    for(u32 i=0;i<count;++i)loom_private_drop_buffer(l->service_as,pins[i]);
    return 0;
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
 l.magic=LOOM_MAGIC;l.refcount=1;memset(&engine,0,sizeof(engine));l.service_engine=&engine;l.service_as=&as;l.service_creator=99;l.cq_entries=4;
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
static int fixed_cases(void) {
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
static struct loom_service_ref pool_ref;
static void make_pool(u32 count) {
 struct loom_pool_extent ext[2]={{&backing,512,64},{&backing,640,64}};
 CHECK(!loom_service_pool_prepare(&slots,&pools,ext,count,&pool_ref) &&
       !loom_service_pool_publish(&slots,&pools,pool_ref),"pool setup ledger");
 // Registration already pinned canonical backing; this fixture's physical
 // registration boundary holds those pins independently of operation storage.
 backing.handle_count+=count;l.receipt_size=32;
 memset(cq_storage,0,sizeof(cq_storage));l.ring_kva=cq_storage;l.hdr_off=0;l.cqe_off=128;l.receipt_off=256;
}
static struct loom_sqe pool_sqe(bool multi) {
 struct loom_sqe s=sqe(LOOM_OP_READ);s.flags=LOOM_SQE_BUFFER_SELECT|(multi?LOOM_SQE_MULTISHOT:0);
 s._resv1[0]=0;s.buf_idx_or_off=pool_ref.slot;s._resv1[2]=pool_ref.incarnation;return s;
}
static void completed_read(struct loom_service_operation *o,u32 count) {
 // Parsed completion boundary: synthetic valid dispatch record, actual owner
 // capture + program consumption. No real wire/progress proof in this fixture.
 o->program.wire.built=true;o->program.wire.expect=(struct loom_service_reply_expect){.kind=P9_TREAD,.fid=73,.new_fid=73,.count=4};
 o->submitted=true;struct p9_dispatch_result d={.kind=P9_TREAD,.fid=73,.new_fid=73,.read_count=count,.read_data=(const u8 *)"abc"};
 loom_rpc_capture(&o->rpc,0,&d);enum loom_program_action action;
 CHECK(!loom_rpc_advance(o,&action) && action==LPROGRAM_TERMINAL && !loom_rpc_settle_locked(o),"parsed read capture settles actual program");
}
static void free_provided(struct loom_service_operation *o) {
 struct loom_request_ref r=o->request;
 if(records[r.index].phase==LREQ_TERMINAL)CHECK(!loom_request_deliver(&l.service_requests,r),"fixture plain terminal delivery");
 CHECK(!loom_rpc_unlink_locked(o) && !loom_provided_detach_locked(o),"retired owner returns only stream ref");
 kfree(o);spin_lock(&as.lock);addrspace_uncharge_pages(&as,1);spin_unlock(&as.lock);
 CHECK(!loom_request_drop_owner(&l.service_requests,r) && !loom_request_reap(&l.service_requests,&slots,r),"provided physical owner ends");
 CHECK(!loom_engine_put(&l,scope,&protocol),"provided final engine pin");loom_unref(&l);
}
static int provided_cases(void) {
 CHECK(!fixed_cases(),"fixed regressions");
 struct loom_service_operation *o;struct loom_sqe s;
 reset();make_pool(2);s=pool_sqe(false);o=(void *)1;
 creator.as=&other_as;CHECK(loom_provided_prepare(&l,&creator,&s,1,&o)==-T_E_ACCES && o==(void *)1 && !pins,"provided creator image exact");creator.as=&as;
 creator.stripes++;CHECK(loom_provided_prepare(&l,&creator,&s,1,&o)==-T_E_ACCES && !pins,"provided creator incarnation exact");creator.stripes--;
 s.len=65;
 CHECK(loom_provided_prepare(&l,&creator,&s,1,&o)==-T_E_INVAL && o==(void *)1 && !pools.pools[pool_ref.slot].streams && roots==1,"all member bounds precede stream ownership");
 s.len=4;pools.bank.cells[1].extent.length=2;
 CHECK(loom_provided_prepare(&l,&creator,&s,1,&o)==-T_E_INVAL && roots==1,"later short member also rejects whole request");pools.bank.cells[1].extent.length=64;
 CHECK(!loom_provided_prepare(&l,&creator,&s,1,&o) && o->pool_stream && !o->pool_busy && !pools.bank.next_nonce && !wire_effects,"prepare retains stream without consuming member");
 CHECK(loom_private_operation_release_unused(o)==-T_E_BUSY,"raw free refuses pool stream");
 CHECK(loom_private_io_drop_fixed(o)==-T_E_BUSY,"fixed drop cannot clear pooled ownership");
 CHECK(loom_service_pool_reap(&slots,&pools,pool_ref)==-T_E_BUSY,"prepared stream prevents pool reap");
 CHECK(!loom_provided_release_prepared(o) && !pools.pools[pool_ref.slot].streams,"prepared rollback returns stream");

 reset();make_pool(2);s=pool_sqe(false);CHECK(!loom_provided_prepare(&l,&creator,&s,1,&o) && !loom_provided_publish(o,2),"single published");protocol.driving=true;
 CHECK(loom_rpc_begin_locked(o,3)==-T_E_BUSY && o->phase==LRPC_QUEUED,"raw begin refuses unclaimed pool");
 CHECK(!loom_provided_begin_locked(o,3) && o->pool_busy && o->receipt.lease==1 && o->program.wire.payload==storage+512,"selected canonical member before read");
 CHECK(loom_provided_release_busy_locked(o)==-T_E_BUSY,"inflight member cannot return");
 completed_read(o,3);CHECK(!memcmp(storage+512,"abc",3),"captured reply in selected member");
 CHECK(loom_admission_commit_locked(o,4)==-T_E_BUSY,"ordinary finish cannot bypass pool commit");
 CHECK(!loom_provided_commit_locked(o,4) && o->pool_pending && o->pool_terminal && records[o->request.index].phase==LREQ_TERMINAL && !o->program.wire.payload,"single positive commits terminal and pending payload");
 CHECK(loom_provided_detach_locked(o)==-T_E_BUSY,"terminal bookkeeping retains stream");
 // Isolate the pending guard from the independent phase guard, modeling a
 // destructor ordering bug that unlinked before actual payload publication.
 o->phase=LRPC_RETIRED;
 CHECK(loom_provided_detach_locked(o)==-T_E_BUSY,"terminal bookkeeping retains stream");o->phase=LRPC_TERMINAL;
 struct loom_service_buffer_receipt receipt=o->receipt;
 CHECK(loom_pool_return(&pools.bank,&pools.pools[pool_ref.slot],&receipt)==-T_E_NOENT,"pending payload cannot return");
 l.cq_tail=4;CHECK(loom_provided_deliver(o)==-T_E_AGAIN && o->pool_pending,"full CQ preserves committed payload");
 protocol.abort_reason=-T_E_CANCELED;nodes.nodes[scope.slot].state=LOOM_SERVICE_ABORTING;
 ((struct loom_ring_hdr *)cq_storage)->cq_head=4;
 CHECK(!loom_provided_deliver(o) && !o->pool_pending && records[o->request.index].phase==LREQ_DELIVERED,"committed single survives scope abort and delivers once");
 struct loom_cqe *cq=(void *)(cq_storage+l.cqe_off);struct loom_service_buffer_receipt *receipts=(void *)(cq_storage+l.receipt_off);
 CHECK(cq[0].result==3 && cq[0].user_data==77 && cq[0].flags==LOOM_CQE_SERVICE_BUFFER && receipts[0].lease==receipt.lease && receipts[0].pool.slot==receipt.pool.slot && receipts[0].pool.incarnation==receipt.pool.incarnation && receipts[0].member==receipt.member && !receipts[0].reserved && ((struct loom_ring_hdr *)cq_storage)->cq_tail==5,"single CQE and receipt share publication");
 CHECK(loom_provided_deliver(o)==-T_E_NOENT,"single delivery never duplicates terminal");free_provided(o);
 CHECK(loom_service_pool_reap(&slots,&pools,pool_ref)==-T_E_BUSY,"scope retirement never returns lease");
 CHECK(!loom_pool_return(&pools.bank,&pools.pools[pool_ref.slot],&receipt) && loom_pool_return(&pools.bank,&pools.pools[pool_ref.slot],&receipt)==-T_E_NOENT,"late exact return only once");

 reset();make_pool(1);s=pool_sqe(true);CHECK(!loom_provided_prepare(&l,&creator,&s,1,&o) && !loom_provided_publish(o,2),"multi published");protocol.driving=true;
 CHECK(!loom_provided_begin_locked(o,3),"multi claim");completed_read(o,3);CHECK(!loom_provided_commit_locked(o,4) && o->phase==LRPC_SETTLING && records[o->request.index].phase==LREQ_ACTIVE,"MORE preserves active terminal obligation");
 CHECK(loom_retirement_post_terminal(o)==-T_E_BUSY,"terminal delivery cannot overtake pending MORE");
 CHECK(loom_rpc_rearm_read_locked(o,5)==-T_E_BUSY,"undelivered MORE prevents next shot");
 l.service_requests.detached=true;
 CHECK(loom_provided_deliver(o)==-T_E_CANCELED && o->pool_pending,"detached consumer cannot publish");l.service_requests.detached=false;
 CHECK(!loom_provided_deliver(o) && cq[0].flags==(LOOM_CQE_MORE|LOOM_CQE_SERVICE_BUFFER),"MORE delivery has explicit receipt");receipt=o->receipt;
 CHECK(loom_rpc_rearm_read_locked(o,100)==-T_E_TIMEDOUT,"rearm checks absolute deadline");
 CHECK(!loom_rpc_rearm_read_locked(o,5) && o->program.wire.offset==512,"rearm retains original offset");
 CHECK(loom_provided_begin_locked(o,6)==-T_E_AGAIN && o->phase==LRPC_QUEUED && !o->pool_busy && pools.bank.next_nonce==1,"empty pool waits without shot or nonce");
 CHECK(loom_provided_begin_locked(o,100)==-T_E_TIMEDOUT,"empty pool deadline remains absolute");
 CHECK(!loom_pool_return(&pools.bank,&pools.pools[pool_ref.slot],&receipt) && !loom_provided_begin_locked(o,7) && o->receipt.lease==2,"return enables fresh monotone lease");
 CHECK(loom_pool_return(&pools.bank,&pools.pools[pool_ref.slot],&receipt)==-T_E_NOENT,"old lease never returns next shot");
 completed_read(o,0);CHECK(!loom_provided_commit_locked(o,8) && !o->pool_busy && !o->pool_pending && !o->pool_terminal && records[o->request.index].result==0,"EOF returns busy without payload lease");free_provided(o);

 reset();make_pool(1);s=pool_sqe(true);CHECK(!loom_provided_prepare(&l,&creator,&s,1,&o) && !loom_provided_publish(o,2),"abort multi publish");protocol.driving=true;
 CHECK(!loom_provided_begin_locked(o,3),"abort multi claim");completed_read(o,3);CHECK(!loom_provided_commit_locked(o,4),"success before abort");receipt=o->receipt;
 protocol.abort_reason=-T_E_CANCELED;nodes.nodes[scope.slot].state=LOOM_SERVICE_ABORTING;
 CHECK(!loom_admission_cancel_locked(o,-T_E_CANCELED,true,5) && o->pool_pending && !records[o->request.index].borrows && !o->source_borrow,"abort commits terminal without CQ-dependent local borrow");
 CHECK(loom_retirement_post_terminal(o)==-T_E_BUSY && !l.cq_tail,"terminal waits behind committed MORE");
 CHECK(!loom_provided_deliver(o) && loom_rpc_rearm_read_locked(o,5)==-T_E_BUSY,"settled abort delivers MORE but cannot rearm");
 CHECK(!loom_retirement_post_terminal(o) && cq[0].flags==(LOOM_CQE_MORE|LOOM_CQE_SERVICE_BUFFER) && cq[1].result==-T_E_CANCELED && !cq[1].flags && !receipts[1].lease,"MORE precedes zero-receipt negative terminal");free_provided(o);
 CHECK(loom_service_pool_reap(&slots,&pools,pool_ref)==-T_E_BUSY && !loom_pool_return(&pools.bank,&pools.pools[pool_ref.slot],&receipt),"lease outlives request and scope");

 reset();make_pool(1);s=pool_sqe(false);CHECK(!loom_provided_prepare(&l,&creator,&s,1,&o) && !loom_provided_publish(o,2),"late abort publish");protocol.driving=true;
 CHECK(!loom_provided_begin_locked(o,3),"late abort claim");completed_read(o,3);
 protocol.abort_reason=-T_E_CANCELED;
 CHECK(loom_provided_commit_locked(o,4)==-T_E_CANCELED && o->pool_busy && !o->pool_pending,"abort before commit preserves only busy cleanup");
 CHECK(!loom_provided_release_busy_locked(o) && !o->program.wire.payload && !o->program.wire.payload_capacity && !loom_admission_cancel_locked(o,-T_E_CANCELED,true,5),"quiescent abort releases unpublished busy");free_provided(o);

 reset();make_pool(1);s=pool_sqe(false);CHECK(!loom_provided_prepare(&l,&creator,&s,1,&o) && !loom_provided_publish(o,2),"exhaust publish");protocol.driving=true;pools.bank.next_nonce=UINT64_MAX;
 CHECK(loom_provided_begin_locked(o,3)==-T_E_NOSPC && !o->pool_busy,"lease exhaustion admits no read");CHECK(!loom_rpc_settle_locked(o) && !loom_admission_cancel_locked(o,-T_E_NOSPC,true,4),"exhaust terminal");free_provided(o);
 reset();make_pool(1);s=pool_sqe(false);CHECK(!loom_provided_prepare(&l,&creator,&s,1,&o) && !loom_provided_publish(o,2),"retry publish");protocol.driving=true;
 CHECK(!loom_provided_begin_locked(o,3),"retry claim");receipt=o->receipt;
 // Model only a proven local no-wire submit refusal. The actual adapter's
 // distinction from remote EAGAIN is qualified in wire-draft controls.
 o->submitted=true;o->program.wire.local_again=true;loom_rpc_capture(&o->rpc,-T_E_AGAIN,NULL);
 enum loom_program_action action;
 CHECK(!loom_rpc_advance(o,&action) && action==LPROGRAM_NEXT && !loom_rpc_requeue_locked(o,4),"local no-wire refusal requeues");
 CHECK(!loom_provided_begin_locked(o,5) && o->receipt.lease==receipt.lease && pools.bank.next_nonce==1,"local retry retains the same busy member");
 o->submitted=true;o->program.wire.local_again=false;loom_rpc_capture(&o->rpc,-T_E_AGAIN,NULL);
 CHECK(!loom_rpc_advance(o,&action) && action==LPROGRAM_TERMINAL && !loom_rpc_settle_locked(o),"remote AGAIN is terminal");
 CHECK(!loom_provided_release_busy_locked(o) && !loom_admission_cancel_locked(o,-T_E_AGAIN,true,6),"negative completion returns only unpublished busy");free_provided(o);
 reset();make_pool(1);s=pool_sqe(true);CHECK(!loom_provided_prepare(&l,&creator,&s,1,&o) && !loom_provided_publish(o,2),"undelivered positive publish");protocol.driving=true;
 CHECK(!loom_provided_begin_locked(o,3),"undelivered positive claim");completed_read(o,3);
 CHECK(!loom_provided_release_busy_locked(o),"positive bytes discarded before cancellation");
 CHECK(loom_rpc_rearm_read_locked(o,4)==-T_E_BUSY,"new shot requires delivered MORE proof");
 CHECK(!loom_admission_cancel_locked(o,-T_E_CANCELED,true,5),"uncommitted positive can only terminate");free_provided(o);
 printf("PASS provided ownership: %u checks\n",checks);return 0;
}
static unsigned abort_calls,abort_completions;
static void backend_abort(void *ctx) {
 CHECK(ctx==&protocol && protocol.client.lock.value && !l.lock.value &&
       !protocol.rpc_quiesced && !protocol.progress.io.tx,"transport abort precedes quiescence");++abort_calls;
}
static void abort_callback(struct p9_rpc *rpc,int status,struct p9_dispatch_result *r) {
 CHECK(!rpc->sending && !protocol.progress.sending && !protocol.progress.io.tx &&
       !protocol.rpc_quiesced && !r && status==-P9_E_IO,"TX owner ends before abort callback");++abort_completions;
}
static void setup_abort(void) {
 protocol.client.progress=&protocol.progress;protocol.admission.conn=(struct SrvConn *)&protocol;
 protocol.progress.io.transport=&protocol.client.transport;
 protocol.progress.io.ops.abort=backend_abort;protocol.progress.io.ops.ctx=&protocol;
 protocol.progress.io.tx=storage;protocol.progress.io.tx_len=10;
 abort_calls=abort_completions=0;
}
static void close_latch(void) {
 l.service_closing=true;loom_slots_close(&slots);protocol.abort_reason=-T_E_CANCELED;
 nodes.nodes[scope.slot].state=LOOM_SERVICE_ABORTING;protocol.driving=true;
}
int main(void) {
 reset();setup_abort();struct p9_rpc rpc={.sending=true,.on_complete=abort_callback};
 protocol.progress.sending=&rpc;protocol.client.inflight[0]=&rpc;
 p9_client_progress_abort(&protocol.client);
 CHECK(abort_calls==1 && abort_completions==1 && !rpc.sending && !protocol.client.inflight[0],"actual progress abort releases in-flight TX owner");
 // A reply can already have removed the tag while TX still borrows its owner.
 reset();setup_abort();rpc=(struct p9_rpc){.sending=true};protocol.progress.sending=&rpc;
 p9_client_progress_abort(&protocol.client);
 CHECK(!rpc.sending && !abort_completions,"abort releases early-replied TX owner too");
 CHECK(!provided_cases(),"provided regressions");
 struct loom_service_operation *o;struct loom_sqe s;
 reset();setup_abort();s=sqe(LOOM_OP_WRITE);
 CHECK(!loom_private_io_prepare(&l,&creator,&s,1,&o) && !loom_private_io_publish(o,2),"fixed close publish");protocol.driving=true;
 CHECK(!loom_admission_begin_locked(o,3),"fixed close begin");
 o->submitted=true;o->rpc.sending=true;protocol.progress.sending=&o->rpc;protocol.client.inflight[0]=&o->rpc;
 close_latch();
 CHECK(loom_retirement_close_operation(o,4)==-T_E_BUSY,"close requires actual quiescence");
 CHECK(!loom_retirement_quiesce(&protocol) && protocol.rpc_quiesced && o->callback_seen && !o->rpc.sending && o->rpc.on_complete!=NULL,"actual abort returns before owner settling");
 CHECK(!loom_retirement_quiesce(&protocol) && abort_calls==1,"quiescence is idempotent");
 drop_watch=o;watch_free=true;
 CHECK(!loom_retirement_close_operation(o,5) && roots==1 && !pins && !charged && !protocol.op_count && backing.handle_count==1 && !l.service_requests.occupied,"fixed close consumes after actual buffer and storage return");watch_free=false;drop_watch=NULL;

 reset();setup_abort();make_pool(1);s=pool_sqe(true);
 CHECK(!loom_provided_prepare(&l,&creator,&s,1,&o) && !loom_provided_publish(o,2),"pending close publish");protocol.driving=true;
 CHECK(!loom_provided_begin_locked(o,3),"pending close begin");completed_read(o,3);CHECK(!loom_provided_commit_locked(o,4),"pending close commit");
 protocol.abort_reason=-T_E_CANCELED;CHECK(!loom_retirement_quiesce(&protocol),"scope abort actually quiesces");
 CHECK(loom_provided_discard_closed_locked(o)==-T_E_BUSY && o->pool_pending,"scope abort cannot discard pending success");
 close_latch();CHECK(loom_requests_detach(&l.service_requests,&slots)==-T_E_BUSY,"global detach blocked by active pending borrow");
 // Another constructor/owner remains independently visible. Closing this
 // operation must not detach the whole table or consume that reservation.
 struct loom_request_ref other;slots.closing=false;nodes.nodes[scope.slot].state=LOOM_SERVICE_READY;
 CHECK(!loom_request_prepare(&l.service_requests,&slots,&nodes,scope,88,0,0,&other),"other reservation setup");
 CHECK(!loom_request_hold_owner(&l.service_requests,other),"other physical reservation");slots.closing=true;nodes.nodes[scope.slot].state=LOOM_SERVICE_ABORTING;
 watch_free=true;CHECK(!loom_retirement_close_operation(o,5) && !l.service_requests.detached && records[other.index].phase==LREQ_PREPARED && records[other.index].owners==1 && l.service_requests.occupied==1 && pools.bank.cells[0].phase==LMEMBER_AVAILABLE && !pools.pools[pool_ref.slot].streams && !l.cq_tail,"pending close precedes global detach without losing other owner");watch_free=false;
 CHECK(!loom_request_drop_owner(&l.service_requests,other) && !loom_request_rollback(&l.service_requests,&slots,other),"other constructor owns its own rollback");

 reset();setup_abort();make_pool(1);s=pool_sqe(false);
 CHECK(!loom_provided_prepare(&l,&creator,&s,1,&o) && !loom_provided_publish(o,2),"single pending close publish");protocol.driving=true;
 CHECK(!loom_provided_begin_locked(o,3),"single pending close begin");completed_read(o,3);CHECK(!loom_provided_commit_locked(o,4),"single pending close commit");close_latch();CHECK(!loom_retirement_quiesce(&protocol),"single pending quiesce");
 watch_free=true;CHECK(!loom_retirement_close_operation(o,5) && !l.cq_tail && pools.bank.cells[0].phase==LMEMBER_AVAILABLE && !l.service_requests.occupied,"single committed terminal discarded once on whole close");watch_free=false;

 reset();setup_abort();make_pool(1);s=pool_sqe(false);
 CHECK(!loom_provided_prepare(&l,&creator,&s,1,&o) && !loom_provided_publish(o,2),"leased close publish");protocol.driving=true;
 CHECK(!loom_provided_begin_locked(o,3),"leased close begin");completed_read(o,3);CHECK(!loom_provided_commit_locked(o,4) && !loom_provided_deliver(o),"leased close deliver");
 struct loom_service_buffer_receipt receipt=o->receipt;close_latch();CHECK(!loom_retirement_quiesce(&protocol),"leased close quiesce");
 watch_free=true;CHECK(!loom_retirement_close_operation(o,5) && pools.bank.cells[0].phase==LMEMBER_LEASED && !pools.pools[pool_ref.slot].streams,"operation destruction does not auto-return lease");watch_free=false;
 CHECK(!loom_pool_return(&pools.bank,&pools.pools[pool_ref.slot],&receipt),"exact lease still returnable before final ring destruction");

 reset();setup_abort();s=sqe(LOOM_OP_LOPEN);CHECK(!loom_private_io_prepare(&l,&creator,&s,1,&o),"unpublished owner setup");close_latch();CHECK(!loom_retirement_quiesce(&protocol),"unpublished scope quiesce");
 CHECK(loom_retirement_close_operation(o,5)==-T_E_BUSY && allocations==1 && records[o->request.index].phase==LREQ_PREPARED,"close never steals unpublished constructor");
 CHECK(!loom_private_io_release_prepared(o),"constructor retains exclusive cleanup");
 reset();make_pool(2);
 CHECK(loom_private_pools_destroy(&l)==-T_E_BUSY && backing.handle_count==3,"live ring cannot discard pool leases");
 CHECK(!loom_pool_stream_get(&pools.pools[pool_ref.slot]),"final pool stream setup");
 struct loom_service_buffer_receipt lease;CHECK(!loom_pool_claim(&pools.bank,&pools.pools[pool_ref.slot],4,&lease) && !loom_pool_commit(&pools.bank,&pools.pools[pool_ref.slot],&lease,3,77,false) && !loom_pool_deliver(&pools.bank,&pools.pools[pool_ref.slot],&lease),"final leased pool setup");
 close_latch();l.refcount=0;
 CHECK(loom_private_pools_destroy(&l)==-T_E_BUSY && backing.handle_count==3,"final pool stream blocks reclamation");
 CHECK(!loom_pool_stream_put(&pools.pools[pool_ref.slot]),"final stream return");
 pools.bank.cells[1].phase=LMEMBER_BUSY;
 CHECK(loom_private_pools_destroy(&l)==-T_E_BUSY && pools.bank.cells[0].phase==LMEMBER_LEASED && backing.handle_count==3,"validate whole bank before any lease invalidation");pools.bank.cells[1].phase=LMEMBER_AVAILABLE;
 CHECK(!loom_private_pools_destroy(&l) && backing.handle_count==1 && loom_service_pools_empty(&pools) && slots.slots[pool_ref.slot].phase==LSLOT_FREE,"final exclusive destruction physically returns every member pin");
 printf("PASS retirement ownership: %u checks\n",checks);return 0;
}
