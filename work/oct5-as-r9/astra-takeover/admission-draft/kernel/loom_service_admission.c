#include <thylacine/loom_service_admission.h>
#include <thylacine/loom_service_fids.h>
#include <thylacine/loom.h>
#include <thylacine/errno.h>
#include <thylacine/extinction.h>
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
    if(reason>=0)return -T_E_INVAL;
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
