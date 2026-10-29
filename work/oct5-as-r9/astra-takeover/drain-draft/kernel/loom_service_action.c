#include <thylacine/loom_service_action.h>
#include <thylacine/loom_service_connect.h>
#include <thylacine/loom.h>
#include <thylacine/errno.h>
#include <thylacine/extinction.h>
#include "../arch/arm64/timer.h"

static int action_abort(struct loom_service_protocol *p,s32 reason) {
    int rc=loom_private_connect_abort(p,reason);
    return rc<0?rc:1;
}
static int action_cancel(struct loom_service_operation *o,s32 reason) {
    struct Loom *l=o->owner;struct loom_service_protocol *p=o->protocol;
    // A failed CLUNK still owns the retired FID. Abort the whole scope before
    // releasing that claim; a mere error completion is not transport teardown.
    if(o->fid_claim==P9_TCLUNK && !p->rpc_quiesced)return action_abort(p,reason);
    spin_lock(&l->lock);int rc=0;
    if(o->phase==LRPC_QUEUED || o->phase==LRPC_INFLIGHT)rc=loom_rpc_settle_locked(o);
    if(!rc && o->pool_busy)rc=loom_provided_release_busy_locked(o);
    spin_unlock(&l->lock);if(rc)return rc;
    if(o->fixed.owner || o->fixed_dropping) {
        rc=loom_private_io_drop_fixed(o);if(rc)return rc;
    }
    spin_lock(&l->lock);
    rc=loom_admission_cancel_locked(o,reason,p->rpc_quiesced,timer_now_ns());
    spin_unlock(&l->lock);return rc?rc:1;
}
// Readiness is only a hint. With exclusive driving held, recheck absolute
// deadlines and cancellation immediately before the next bounded wire copy.
static int action_io(struct loom_service_protocol *p) {
    struct Loom *l=p->owner;u64 now=timer_now_ns();
    spin_lock(&l->lock);
    struct loom_service_node *n=&l->service_nodes->nodes[p->scope.slot];
    s32 reason=p->abort_reason<0?p->abort_reason:n->reason<0?n->reason:0;
    if(!reason && l->service_closing)reason=-T_E_CANCELED;
    u32 count=0;
    for(struct loom_service_operation *o=p->op_head;o;o=o->next) {
        if(++count>p->op_count || o->request.index>=l->service_requests.capacity)
            extinction("IO preflight operation list corrupt");
        struct loom_service_request *r=&l->service_requests.records[o->request.index];
        if(r->identity!=o->request.identity)extinction("IO preflight request changed");
        if(!reason && o->phase!=LRPC_TERMINAL && r->deadline && r->deadline<=now)
            reason=-T_E_TIMEDOUT;
    }
    if(count!=p->op_count)extinction("IO preflight operation count corrupt");
    spin_unlock(&l->lock);
    if(reason)return action_abort(p,reason);
    int rc=p9_client_progress_step(&p->client,now);
    return rc<0?action_abort(p,rc):rc>0?1:0;
}
static int action_execute(struct loom_service_protocol *p,struct loom_ready_selection s) {
    struct Loom *l=p->owner;struct loom_service_operation *o=s.operation;
    int rc;enum loom_program_action next;
    switch(s.action) {
    case LREADY_QUIESCE:
        rc=action_abort(p,s.reason);if(rc<0)return rc;
        rc=loom_retirement_quiesce(p);return rc?rc:1;
    case LREADY_CANCEL:return action_cancel(o,s.reason);
    case LREADY_SUBMIT:
        spin_lock(&l->lock);
        rc=o->pool_stream?loom_provided_begin_locked(o,timer_now_ns()):
                          loom_admission_begin_locked(o,timer_now_ns());
        spin_unlock(&l->lock);
        if(rc==-T_E_AGAIN || rc==-T_E_BUSY)return 0;
        if(rc==-T_E_NOSPC)return action_cancel(o,rc);
        if(rc)return action_abort(p,rc);
        // Successful begin transfers exactly one attempt to the client. The
        // adapter captures synchronous negative refusals (including local AGAIN)
        // exactly once; ADVANCE decides retry versus remote terminal next visit.
        (void)loom_rpc_submit(o);return 1;
    case LREADY_ADVANCE:
        rc=loom_rpc_advance(o,&next);if(rc)return rc;
        spin_lock(&l->lock);
        if(next==LPROGRAM_NEXT) {
            rc=loom_rpc_requeue_locked(o,timer_now_ns());
            // Never leave an already-consumed callback selected as ADVANCE.
            if(rc && loom_rpc_settle_locked(o))extinction("READY requeue settle lost");
        } else rc=loom_rpc_settle_locked(o);
        spin_unlock(&l->lock);
        if(rc)return action_abort(p,rc);
        if(next==LPROGRAM_ABORT)return action_abort(p,-T_E_IO);
        return 1;
    case LREADY_COMMIT:
        if(o->program.result<0)return action_cancel(o,o->program.result);
        if(o->fixed.owner || o->fixed_dropping) {
            rc=loom_private_io_drop_fixed(o);if(rc)return rc;
        }
        spin_lock(&l->lock);
        rc=o->pool_stream?loom_provided_commit_locked(o,timer_now_ns()):
                          loom_admission_commit_locked(o,timer_now_ns());
        spin_unlock(&l->lock);
        return rc?action_abort(p,rc):1;
    case LREADY_DELIVER:
        rc=loom_provided_deliver(o);return rc?rc:1;
    case LREADY_REARM:
        spin_lock(&l->lock);rc=loom_rpc_rearm_read_locked(o,timer_now_ns());spin_unlock(&l->lock);
        return rc?action_abort(p,rc):1;
    case LREADY_POST:
        rc=loom_retirement_post_terminal(o);return rc?rc:1;
    case LREADY_FREE:
        rc=loom_private_operation_release_retired(o);return rc?rc:1;
    case LREADY_DISCARD:
        rc=loom_retirement_close_operation(o,timer_now_ns());return rc?rc:1;
    case LREADY_IO:return action_io(p);
    default:return -T_E_INVAL;
    }
}
int loom_ready_step(struct loom_service_protocol *p,u64 now,bool native_ready) {
    if(!p || !p->owner)return -T_E_INVAL;
    struct Loom *l=p->owner;struct loom_ready_selection s;
    spin_lock(&l->lock);int rc=loom_ready_claim_locked(p,now,native_ready,&s);spin_unlock(&l->lock);
    if(rc)return rc==-T_E_BUSY || rc==-T_E_AGAIN?0:rc;
    rc=action_execute(p,s);
    // s.operation may have been freed. Only the caller's separate entry/root
    // licenses p/l access here; no operation-derived lifetime is sufficient.
    spin_lock(&l->lock);p->driving=false;spin_unlock(&l->lock);
    loom_private_close_changed(l);
    return rc==-T_E_BUSY || rc==-T_E_AGAIN?0:rc;
}
