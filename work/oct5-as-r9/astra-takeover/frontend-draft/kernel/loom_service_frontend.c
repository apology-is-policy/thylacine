#include <thylacine/loom_service_frontend.h>
#include <thylacine/loom_service_decode.h>
#include <thylacine/loom_service_connect.h>
#include <thylacine/loom_service_provided.h>
#include <thylacine/loom.h>
#include <thylacine/errno.h>
#include <thylacine/extinction.h>
#include "../arch/arm64/timer.h"

_Static_assert(sizeof(struct loom_service_engine)<=4096,"frontend fits charged engine page");
static int frontend_dispatch(struct Loom *l,struct Proc *p,const struct loom_sqe *s) {
    int rc=loom_private_sqe_shape(s,l->receipt_size!=0);if(rc)return rc;
    if(s->opcode==LOOM_OP_SERVICE_CONNECT) {
        struct loom_service_ref target={.slot=s->handle_idx,.incarnation=s->_resv1[1]};
        struct loom_service_ref scope={.slot=(u32)s->offset,.incarnation=s->_resv1[2]};
        struct loom_service_protocol *protocol;
        rc=loom_private_connect_prepare(l,scope,target,s->user_data,s->_resv1[3],timer_now_ns(),&protocol);
        if(rc)return rc;
        rc=loom_engine_adopt(protocol,timer_now_ns());
        // Successful adoption owns even its failed native publication terminal.
        if(rc && loom_private_connect_rollback(protocol))extinction("frontend lost CONNECT rollback");
        return rc;
    }
    bool pooled=s->opcode==LOOM_OP_READ && (s->flags&LOOM_SQE_BUFFER_SELECT);
    struct loom_service_operation *o;
    rc=pooled?loom_provided_prepare(l,p,s,timer_now_ns(),&o):
              loom_private_io_prepare(l,p,s,timer_now_ns(),&o);
    if(rc)return rc;
    rc=pooled?loom_provided_publish(o,timer_now_ns()):loom_private_io_publish(o,timer_now_ns());
    if(rc) {
        int drop=pooled?loom_provided_release_prepared(o):loom_private_io_release_prepared(o);
        if(drop)extinction("frontend lost prepared operation rollback");
    }
    return rc; // o may already have been freed after successful publication
}
int loom_frontend_step(struct Loom *l) {
    spin_lock(&l->lock);
    struct loom_service_engine *e=l->service_engine;
    struct loom_ring_hdr *h=(void *)(l->ring_kva+l->hdr_off);
    if(!e->rejected || e->submitting || l->service_closing ||
       (u32)(l->cq_tail-__atomic_load_n(&h->cq_head,__ATOMIC_ACQUIRE))>=l->cq_entries) {
        spin_unlock(&l->lock);return 0;
    }
    u32 at=l->cq_tail&(l->cq_entries-1u);
    struct loom_cqe *cq=(void *)(l->ring_kva+l->cqe_off);
    cq[at]=(struct loom_cqe){.user_data=e->rejected_user_data,.result=e->rejected_result};
    if(l->receipt_size) {
        struct loom_service_buffer_receipt *receipts=(void *)(l->ring_kva+l->receipt_off);
        receipts[at]=(struct loom_service_buffer_receipt){0};
    }
    e->rejected=false;e->rejected_user_data=0;e->rejected_result=0;
    ++l->cq_tail;__atomic_store_n(&h->cq_tail,l->cq_tail,__ATOMIC_RELEASE);
    spin_unlock(&l->lock);
    poll_waiter_list_wake(&l->cq_waiters);loom_private_close_changed(l);return 1;
}
int loom_private_submit(struct Loom *l,struct Proc *p,u32 budget) {
    if(!l || l->magic!=LOOM_MAGIC || !loom_is_private(l) || !p || p->magic!=PROC_MAGIC)
        return -T_E_INVAL;
    if(p->as!=l->service_as || p->stripes!=l->service_creator)return -T_E_ACCES;
    spin_lock(&l->lock);bool closed=l->service_closing;spin_unlock(&l->lock);
    if(closed)return -T_E_CANCELED;
    // ENTER is also the explicit CQ-reap kick, including a zero-submit call.
    // Caller retains l across every unlock and notification in this function.
    struct loom_ring_hdr *h=(void *)(l->ring_kva+l->hdr_off);
    __atomic_fetch_and(&h->flags,~(u32)LOOM_RING_SQ_NEED_WAKEUP,__ATOMIC_RELEASE);
    loom_private_close_changed(l);
    if(budget>l->sq_entries)budget=l->sq_entries;
    u32 consumed=0;
    for(u32 i=0;i<budget;++i) {
        struct loom_sqe copy;
        spin_lock(&l->lock);
        struct loom_service_engine *e=l->service_engine;
        if(l->service_closing){spin_unlock(&l->lock);return consumed?(int)consumed:-T_E_CANCELED;}
        if(e->submitting || e->rejected ||
           (u32)(l->cq_tail-__atomic_load_n(&h->cq_head,__ATOMIC_ACQUIRE))>=l->cq_entries ||
           __atomic_load_n(&h->sq_tail,__ATOMIC_ACQUIRE)==l->sq_head) {
            spin_unlock(&l->lock);break;
        }
        u32 *indices=(void *)(l->ring_kva+l->sq_array_off);
        u32 index=__atomic_load_n(&indices[l->sq_head&(l->sq_entries-1u)],__ATOMIC_ACQUIRE);
        bool valid=index<l->sq_entries;
        if(valid) {
            const volatile u8 *from=(void *)(l->ring_kva+l->sqe_off+index*sizeof(copy));
            u8 *to=(void *)&copy;
            for(u32 b=0;b<sizeof(copy);++b)to[b]=from[b];
            e->submitting=true; // reserves the precharged rejection record
        } else __atomic_fetch_add(&h->dropped,1,__ATOMIC_RELEASE);
        ++l->sq_head;__atomic_store_n(&h->sq_head,l->sq_head,__ATOMIC_RELEASE);
        spin_unlock(&l->lock);++consumed;
        if(!valid)continue;
        int rc=frontend_dispatch(l,p,&copy);
        spin_lock(&l->lock);
        if(!e->submitting || e->rejected)extinction("frontend lost serialized claim");
        if(rc<0 && !l->service_closing) {
            e->rejected_user_data=copy.user_data;e->rejected_result=rc;e->rejected=true;
        }
        e->submitting=false;
        spin_unlock(&l->lock);
        loom_private_close_changed(l);
        (void)loom_frontend_step(l);
    }
    return (int)consumed;
}
