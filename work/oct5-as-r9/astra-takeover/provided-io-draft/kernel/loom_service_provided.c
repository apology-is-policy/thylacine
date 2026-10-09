#include <thylacine/loom_service_provided.h>
#include <thylacine/loom_service_pool_table.h>
#include <thylacine/loom_service_decode.h>
#include <thylacine/loom.h>
#include <thylacine/burrow.h>
#include <thylacine/page.h>
#include <thylacine/errno.h>
#include <thylacine/extinction.h>
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
    } else if(o->phase!=LRPC_SETTLING || !o->multishot){rc=-T_E_BUSY;goto done;}
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
