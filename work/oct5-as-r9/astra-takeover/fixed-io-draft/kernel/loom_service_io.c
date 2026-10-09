#include <thylacine/loom_service_io.h>
#include <thylacine/loom_service_decode.h>
#include <thylacine/loom.h>
#include <thylacine/burrow.h>
#include <thylacine/page.h>
#include <thylacine/errno.h>
#include <thylacine/extinction.h>

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
