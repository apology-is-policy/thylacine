#include <thylacine/loom_service_ready.h>
#include <thylacine/loom.h>
#include <thylacine/errno.h>
#include <thylacine/extinction.h>
#include <thylacine/poll.h>
#include <thylacine/loom_service_pool_table.h>
static bool ready_member(struct loom_service_operation *o) {
    if(!o->pool_stream || o->pool_busy)return true;
    struct loom_pool_bank *b=&o->owner->service_pools->bank;
    // Exhaustion is actionable even when no member is free: begin must report
    // NOSPC rather than parking forever on an impossible future receipt.
    if(b->next_nonce==UINT64_MAX)return true;
    for(u32 i=0;i<LOOM_SERVICE_POOL_MEMBERS;++i) {
        struct loom_pool_cell *c=&b->cells[i];
        if(c->phase==LMEMBER_AVAILABLE && c->pool.slot==o->pool.slot &&
           true && !c->pool.reserved)return true;
    }
    return false;
}
static struct loom_ready_interest ready_scan(struct loom_service_protocol *p,u64 now,
                                              struct loom_ready_selection *pick) {
    struct loom_ready_interest a={0};
    *pick=(struct loom_ready_selection){0};
    if(!p || !p->owner || !p->indexed || !p->connect_bound || p->driving || p->users)
        return a;
    struct Loom *l=p->owner;
    // A locally retired scope may retain CQ-backpressured pool/result owners.
    // They still need delivery and free visits but may never submit again.
    struct loom_service_node *scope=&l->service_nodes->nodes[p->scope.slot];
    s32 reason=p->abort_reason<0?p->abort_reason:scope->reason<0?scope->reason:0;
    if(!reason && l->service_closing)reason=-T_E_CANCELED;
    if(!reason && p->client.dead)reason=-T_E_IO;
    if(!reason && scope->state!=LOOM_SERVICE_READY && !p->locally_retired)return a;
    struct loom_ring_hdr *h=(void *)(l->ring_kva+l->hdr_off);
    u32 head=__atomic_load_n(&h->cq_head,__ATOMIC_ACQUIRE);
    bool room=(u32)(l->cq_tail-head)<l->cq_entries;
    bool wire=!reason && !p->locally_retired && !p->progress.io.tx &&
              p9_session_has_free_tag(&p->client.session);
    u32 seen=0;struct loom_service_operation *previous=NULL;
    for(struct loom_service_operation *o=p->op_head;o;o=o->next) {
        if(++seen>p->op_count || o->prev!=previous || o->protocol!=p || o->owner!=l ||
           o->request.index>=l->service_requests.capacity)
            extinction("READY operation list corrupt");
        previous=o;
        struct loom_service_request *r=&l->service_requests.records[o->request.index];
        if(r->identity!=o->request.identity || r->owners!=1)
            extinction("READY operation request changed");
        bool active=o->phase!=LRPC_TERMINAL;
        if(active && r->deadline && (!a.deadline || r->deadline<a.deadline))a.deadline=r->deadline;
        // A deadline must end possible wire effects before metadata cancellation;
        // the executor latches the whole private scope then actually quiesces it.
        s32 why=reason?reason:(active && r->deadline && r->deadline<=now)?-T_E_TIMEDOUT:0;
        enum loom_ready_action action=LREADY_NONE;
        if(l->service_closing && p->rpc_quiesced)action=LREADY_DISCARD;
        else if(why && !p->rpc_quiesced)action=LREADY_QUIESCE;
        else if(why && active)action=LREADY_CANCEL;
        else if(o->pool_pending && room)action=LREADY_DELIVER;
        else if(o->phase==LRPC_TERMINAL) {
            if(r->phase==LREQ_DELIVERED)action=LREADY_FREE;
            else if(!o->pool_pending && room)action=LREADY_POST;
        } else if(o->phase==LRPC_INFLIGHT && o->callback_seen && !o->rpc.sending)
            action=LREADY_ADVANCE;
        else if(o->phase==LRPC_SETTLING && !o->pool_pending)
            action=o->pool_more_delivered?LREADY_REARM:LREADY_COMMIT;
        else if(o->phase==LRPC_QUEUED && wire && ready_member(o)) {
            bool clunk_wait=o->fid_claim==P9_TCLUNK &&
                l->service_nodes->nodes[o->source.slot].fid_borrows;
            if(!clunk_wait)action=LREADY_SUBMIT;
        }
        // Always finish scanning for the earliest deadline, even after a pick.
        if(action!=LREADY_NONE && (pick->action==LREADY_NONE ||
           (action==LREADY_QUIESCE && pick->action!=LREADY_QUIESCE)))
            *pick=(struct loom_ready_selection){action,o,why};
    }
    if(seen!=p->op_count || previous!=p->op_tail)extinction("READY list count mismatch");
    if(reason && !p->rpc_quiesced && pick->action==LREADY_NONE)
        *pick=(struct loom_ready_selection){LREADY_QUIESCE,NULL,reason};
    if(!reason && !p->locally_retired) {
        // Retain RX interest even with a full CQ/empty pool: unexpected EOF or
        // malformed peer bytes must still break the scope. Progress itself is
        // one bounded frame/msize quantum and never waits for native readiness.
        a.events=POLLIN;if(p->progress.io.tx)a.events|=POLLOUT;
    }
    a.work=pick->action!=LREADY_NONE;
    return a;
}
struct loom_ready_interest loom_ready_interest_locked(struct loom_service_protocol *p,u64 now) {
    struct loom_ready_selection ignored;return ready_scan(p,now,&ignored);
}
int loom_ready_claim_locked(struct loom_service_protocol *p,u64 now,bool native_ready,
                            struct loom_ready_selection *out) {
    if(!out || !p || !p->owner)return -T_E_INVAL;
    if(p->driving || p->users)return -T_E_BUSY;
    struct loom_ready_selection selected;
    struct loom_ready_interest a=ready_scan(p,now,&selected);
    if(!a.work) {
        if(!native_ready || !a.events)return -T_E_AGAIN;
        selected=(struct loom_ready_selection){LREADY_IO,NULL,0};
    }
    struct loom_service_operation *o=selected.operation;
    if(o && o!=p->op_tail) {
        if(o->prev)o->prev->next=o->next;else p->op_head=o->next;
        o->next->prev=o->prev;
        o->prev=p->op_tail;o->next=NULL;p->op_tail->next=o;p->op_tail=o;
    }
    p->driving=true;*out=selected;return 0;
}
