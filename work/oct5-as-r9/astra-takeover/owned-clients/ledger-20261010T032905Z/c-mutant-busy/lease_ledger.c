#include "lease_ledger.h"
#include <stdatomic.h>
static _Atomic uint64_t next_owner=1;
enum { AVAILABLE,LEASED,RETURNING };
static int same_ref(struct t_loom_service_ref a,struct t_loom_service_ref b) {
    return a.slot==b.slot && a.reserved==b.reserved && a.incarnation==b.incarnation;
}
int t_lease_init(struct t_lease_ledger *out,struct t_loom_service_ref pool,
    const struct t_loom_service_pool_member *ranges,size_t count,const uint64_t *sizes,size_t buffers) {
    if(!out || !ranges || !sizes || !count || count>64 || buffers>64 ||
       pool.reserved || !pool.incarnation || pool.slot>=64)return T_LEASE_GEOMETRY;
    struct t_lease_ledger next={0};next.pool=pool;next.count=count;
    for(size_t i=0;i<count;++i) {
        struct t_loom_service_pool_member r=ranges[i];
        if(r.reserved || !r.length || r.buffer_index>=buffers ||
           r.length>UINT64_MAX-r.offset || r.offset+r.length>sizes[r.buffer_index])return T_LEASE_GEOMETRY;
        for(size_t j=0;j<i;++j) {
            struct t_loom_service_pool_member p=ranges[j];
            if(p.buffer_index==r.buffer_index && r.offset<p.offset+p.length && p.offset<r.offset+r.length)
                return T_LEASE_GEOMETRY;
        }
        next.members[i].range=r;
    }
    uint64_t owner=atomic_load_explicit(&next_owner,memory_order_relaxed);
    do { if(owner==UINT64_MAX)return T_LEASE_EXHAUSTED; }
    while(!atomic_compare_exchange_weak_explicit(&next_owner,&owner,owner+1,memory_order_relaxed,memory_order_relaxed));
    next.owner=owner;*out=next;return 0;
}
int t_lease_accept_read(struct t_lease_ledger *b,int32_t result,uint32_t flags,
    struct t_loom_service_buffer_receipt r,struct t_lease_token *out) {
    if(!b || !out || (flags & ~(1u|T_LOOM_CQE_SERVICE_BUFFER)))return T_LEASE_RECEIPT;
    if(!(flags&T_LOOM_CQE_SERVICE_BUFFER)) {
        if(flags || result>0 || r.pool.slot || r.pool.reserved || r.pool.incarnation || r.member || r.reserved || r.lease)
            return T_LEASE_RECEIPT;
        return 0; /* EOF/final/error does not return earlier payload leases. */
    }
    if(result<=0 || !same_ref(r.pool,b->pool) || r.reserved || !r.lease || r.member>=b->count)
        return T_LEASE_RECEIPT;
    struct t_lease_member *m=&b->members[r.member];
    if((uint64_t)result>m->range.length)return T_LEASE_RECEIPT;
    
    if(r.lease<=m->last)return T_LEASE_STALE;
    m->last=r.lease;m->state=LEASED;++b->outstanding;
    *out=(struct t_lease_token){.owner=b->owner,.receipt=r,.length=(size_t)result};return 1;
}
static struct t_lease_member *exact(struct t_lease_ledger *b,const struct t_lease_token *t) {
    if(!b || !t || t->owner!=b->owner || !same_ref(t->receipt.pool,b->pool) ||
       t->receipt.reserved || !t->receipt.lease || t->receipt.member>=b->count)return NULL;
    struct t_lease_member *m=&b->members[t->receipt.member];
    return m->last==t->receipt.lease?m:NULL;
}
int t_lease_begin_return(struct t_lease_ledger *b,const struct t_lease_token *t) {
    struct t_lease_member *m=exact(b,t);if(!m)return T_LEASE_STALE;
    if(m->state!=LEASED && m->state!=RETURNING)return T_LEASE_STATE;
    m->state=RETURNING;return 0;
}
int t_lease_finish_return(struct t_lease_ledger *b,const struct t_lease_token *t,int64_t rc) {
    struct t_lease_member *m=exact(b,t);if(!m)return T_LEASE_STALE;
    if(m->state!=RETURNING)return T_LEASE_STATE;
    /* Even NOENT retains the charge: only confirmed success allows reuse. */
    if(rc==0){m->state=AVAILABLE;--b->outstanding;}return 0;
}
