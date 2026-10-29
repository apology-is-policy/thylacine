#include <thylacine/loom_service_fids.h>
#include <thylacine/errno.h>

static bool fid_same(struct loom_service_ref a,struct loom_service_ref b) {
    return a.slot==b.slot && a.incarnation==b.incarnation && a.reserved==b.reserved;
}
static struct loom_service_node *fid_exact(struct loom_slot_table *t,
    struct loom_service_node_table *n,struct loom_service_ref r,bool retiring) {
    if(!t || !n || r.reserved || !r.incarnation || r.slot>=LOOM_SERVICE_SLOT_COUNT)return NULL;
    struct loom_service_slot *s=&t->slots[r.slot];
    if(s->incarnation!=r.incarnation || (s->phase!=LSLOT_LIVE &&
       !(retiring && s->phase==LSLOT_RETIRING)) ||
       (s->kind!=LOOM_SERVICE_SCOPE && s->kind!=LOOM_SERVICE_FID))return NULL;
    return &n->nodes[r.slot];
}
static bool fid_scope_ready(struct loom_slot_table *t,struct loom_service_node_table *n,
    struct loom_service_ref scope) {
    struct loom_service_node *s=fid_exact(t,n,scope,false);
    return s && !t->closing && t->slots[scope.slot].kind==LOOM_SERVICE_SCOPE &&
           s->state==LOOM_SERVICE_READY && !s->reason;
}
static bool fid_parent(struct loom_slot_table *t,struct loom_service_node *n,
    struct loom_service_ref ref,struct loom_service_ref scope) {
    return t->slots[ref.slot].kind==LOOM_SERVICE_SCOPE ? fid_same(ref,scope) : fid_same(n->parent,scope);
}
static bool fid_duplicate(struct loom_slot_table *t,struct loom_service_node_table *n,
    struct loom_service_ref scope,u32 wire) {
    for(u32 i=0;i<LOOM_SERVICE_SLOT_COUNT;++i) {
        struct loom_service_slot *s=&t->slots[i];struct loom_service_node *p=&n->nodes[i];
        if((s->phase==LSLOT_LIVE || s->phase==LSLOT_RETIRING) &&
           (p->fid_flags&LFID_BOUND) && p->wire_fid==wire &&
           ((s->kind==LOOM_SERVICE_SCOPE && i==scope.slot && s->incarnation==scope.incarnation) ||
            (s->kind==LOOM_SERVICE_FID && fid_same(p->parent,scope))))return true;
    }
    return false;
}
int loom_fid_bind_root(struct loom_slot_table *t,struct loom_service_node_table *nodes,
    struct loom_service_ref scope,u32 wire) {
    if(wire==UINT32_MAX)return -T_E_INVAL;
    if(!fid_scope_ready(t,nodes,scope))return -T_E_CANCELED;
    struct loom_service_node *n=&nodes->nodes[scope.slot];
    if(n->fid_flags || n->fid_claim || n->wire_fid || n->fid_access || n->fid_borrows ||
       !t->slots[scope.slot].pins)return -T_E_BUSY;
    if(fid_duplicate(t,nodes,scope,wire))return -T_E_BUSY;
    n->wire_fid=wire;n->fid_flags=LFID_BOUND;
    return 0;
}
int loom_fid_claim_walk(struct loom_slot_table *t,struct loom_service_node_table *nodes,
    struct loom_service_ref dest,struct loom_service_ref scope,u64 id) {
    if(!id)return -T_E_INVAL;
    struct loom_service_node *n=fid_exact(t,nodes,dest,false);
    if(!n || t->slots[dest.slot].kind!=LOOM_SERVICE_FID)return -T_E_NOENT;
    if(!fid_parent(t,n,dest,scope) || !fid_scope_ready(t,nodes,scope))return -T_E_CANCELED;
    if(n->state!=LOOM_SERVICE_RESERVED || n->fid_claim || n->fid_flags ||
       n->wire_fid || n->fid_access)return -T_E_BUSY;
    int rc=loom_slot_pin(t,dest,LOOM_SERVICE_FID);
    if(!rc)n->fid_claim=id;
    return rc;
}
int loom_fid_commit_walk(struct loom_slot_table *t,struct loom_service_node_table *nodes,
    struct loom_service_ref dest,struct loom_service_ref scope,u64 id,u32 wire) {
    if(!id || wire==UINT32_MAX)return -T_E_INVAL;
    struct loom_service_node *n=fid_exact(t,nodes,dest,false);
    if(!n || t->slots[dest.slot].kind!=LOOM_SERVICE_FID)return -T_E_NOENT;
    if(!fid_parent(t,n,dest,scope) || !fid_scope_ready(t,nodes,scope))return -T_E_CANCELED;
    if(n->state!=LOOM_SERVICE_RESERVED || n->fid_claim!=id || n->fid_flags ||
       !t->slots[dest.slot].pins)return -T_E_BUSY;
    if(fid_duplicate(t,nodes,scope,wire))return -T_E_BUSY;
    n->wire_fid=wire;n->fid_flags=LFID_BOUND;n->state=LOOM_SERVICE_READY;
    return 0;
}
int loom_fid_release_claim(struct loom_slot_table *t,struct loom_service_node_table *nodes,
    struct loom_service_ref dest,u64 id) {
    struct loom_service_node *n=fid_exact(t,nodes,dest,true);
    if(!n)return -T_E_NOENT;
    if((n->fid_flags&LFID_CLUNKING) && n->state!=LOOM_SERVICE_RETIRED)return -T_E_BUSY;
    if(!id || n->fid_claim!=id || !t->slots[dest.slot].pins)return -T_E_BUSY;
    int rc=loom_slot_unpin(t,dest);
    if(!rc){n->fid_claim=0;n->fid_flags&=~LFID_OPENING;}
    return rc;
}
int loom_fid_borrow(struct loom_slot_table *t,struct loom_service_node_table *nodes,
    struct loom_service_ref source,struct loom_service_ref scope,u32 need,u32 *wire) {
    if(!wire || (need&~(LFID_READ|LFID_WRITE)))return -T_E_INVAL;
    struct loom_service_node *n=fid_exact(t,nodes,source,false);
    if(!n)return -T_E_NOENT;
    if(!fid_parent(t,n,source,scope) || !fid_scope_ready(t,nodes,scope))return -T_E_CANCELED;
    if(n->state!=LOOM_SERVICE_READY || !(n->fid_flags&LFID_BOUND))return -T_E_NOENT;
    if(n->fid_claim)return -T_E_BUSY;
    if(n->fid_borrows==UINT32_MAX)return -T_E_NOSPC;
    if(need && (!(n->fid_flags&LFID_OPEN) || (n->fid_access&need)!=need))return -T_E_ACCES;
    int rc=loom_slot_pin(t,source,t->slots[source.slot].kind);
    if(!rc){(void)0;*wire=n->wire_fid;}
    return rc;
}
int loom_fid_commit_open(struct loom_slot_table *t,struct loom_service_node_table *nodes,
    struct loom_service_ref source,struct loom_service_ref scope,u64 id,u32 mode) {
    if(!id || mode>2)return -T_E_INVAL;
    struct loom_service_node *n=fid_exact(t,nodes,source,false);
    if(!n)return -T_E_NOENT;
    if(!fid_parent(t,n,source,scope) || !fid_scope_ready(t,nodes,scope))return -T_E_CANCELED;
    if(n->state!=LOOM_SERVICE_READY || !(n->fid_flags&LFID_BOUND))return -T_E_NOENT;
    if(!(n->fid_flags&LFID_OPENING) || (n->fid_flags&LFID_OPEN) ||
       n->fid_claim!=id || n->fid_borrows || !t->slots[source.slot].pins)return -T_E_BUSY;
    n->fid_access=mode==0?LFID_READ:mode==1?LFID_WRITE:LFID_READ|LFID_WRITE;
    n->fid_flags|=LFID_OPEN;
    return 0;
}

int loom_fid_put(struct loom_slot_table *t,struct loom_service_node_table *nodes,
    struct loom_service_ref source) {
    struct loom_service_node *n=fid_exact(t,nodes,source,true);
    if(!n)return -T_E_NOENT;
    if(!n->fid_borrows)return -T_E_INVAL;
    int rc=loom_slot_unpin(t,source);
    if(!rc)--n->fid_borrows;
    return rc;
}
int loom_fid_claim_open(struct loom_slot_table *t,struct loom_service_node_table *nodes,
    struct loom_service_ref source,struct loom_service_ref scope,u64 id) {
    if(!id)return -T_E_INVAL;
    struct loom_service_node *n=fid_exact(t,nodes,source,false);
    if(!n)return -T_E_NOENT;
    if(!fid_parent(t,n,source,scope) || !fid_scope_ready(t,nodes,scope))return -T_E_CANCELED;
    if(n->state!=LOOM_SERVICE_READY || n->fid_flags!=LFID_BOUND)return -T_E_NOENT;
    if(n->fid_claim || n->fid_borrows)return -T_E_BUSY;
    int rc=loom_slot_pin(t,source,t->slots[source.slot].kind);
    if(!rc){n->fid_claim=id;n->fid_flags|=LFID_OPENING;}
    return rc;
}
int loom_fid_begin_clunk(struct loom_slot_table *t,struct loom_service_node_table *nodes,
    struct loom_service_ref source,struct loom_service_ref scope,u64 id) {
    if(!id)return -T_E_INVAL;
    struct loom_service_node *n=fid_exact(t,nodes,source,false);
    if(!n || t->slots[source.slot].kind!=LOOM_SERVICE_FID)return -T_E_NOENT;
    if(!fid_parent(t,n,source,scope) || !fid_scope_ready(t,nodes,scope))return -T_E_CANCELED;
    if(n->state!=LOOM_SERVICE_READY || !(n->fid_flags&LFID_BOUND))return -T_E_NOENT;
    if(n->fid_claim)return -T_E_BUSY;
    int rc=loom_slot_pin(t,source,LOOM_SERVICE_FID);
    if(rc)return rc;
    n->fid_claim=id;n->fid_flags|=LFID_CLUNKING;n->state=LOOM_SERVICE_ABORTING;
    (void)loom_slot_retire(t,source);
    return 0;
}
int loom_fid_clunk_wire(struct loom_slot_table *t,struct loom_service_node_table *nodes,
    struct loom_service_ref source,u64 id,u32 *wire) {
    if(!wire || !id)return -T_E_INVAL;
    struct loom_service_node *n=fid_exact(t,nodes,source,true);
    if(!n || t->slots[source.slot].kind!=LOOM_SERVICE_FID)return -T_E_NOENT;
    if(n->state!=LOOM_SERVICE_ABORTING || !(n->fid_flags&LFID_CLUNKING) ||
       n->fid_claim!=id || !t->slots[source.slot].pins || n->fid_borrows)return -T_E_BUSY;
    *wire=n->wire_fid;return 0;
}
int loom_fid_finish_clunk(struct loom_slot_table *t,struct loom_service_node_table *nodes,
    struct loom_service_ref source,u64 id) {
    u32 wire;
    int rc=loom_fid_clunk_wire(t,nodes,source,id,&wire);
    if(!rc)nodes->nodes[source.slot].state=LOOM_SERVICE_RETIRED;
    return rc;
}
int loom_fid_retire_aborted(struct loom_slot_table *t,struct loom_service_node_table *nodes,
    struct loom_service_ref source) {
    struct loom_service_node *n=fid_exact(t,nodes,source,true);
    if(!n || t->slots[source.slot].kind!=LOOM_SERVICE_FID)return -T_E_NOENT;
    struct loom_service_node *p=fid_exact(t,nodes,n->parent,true);
    if(!p || t->slots[n->parent.slot].kind!=LOOM_SERVICE_SCOPE ||
       (p->state!=LOOM_SERVICE_ABORTING && p->state!=LOOM_SERVICE_RETIRED))return -T_E_BUSY;
    if(n->fid_claim || n->fid_borrows)return -T_E_BUSY;
    (void)loom_slot_retire(t,source);n->state=LOOM_SERVICE_RETIRED;
    return 0;
}
int loom_fid_reap(struct loom_slot_table *t,struct loom_service_node_table *nodes,
    struct loom_service_ref source) {
    struct loom_service_node *n=fid_exact(t,nodes,source,true);
    if(!n || t->slots[source.slot].kind!=LOOM_SERVICE_FID)return -T_E_NOENT;
    if(n->state!=LOOM_SERVICE_RETIRED || t->slots[source.slot].phase!=LSLOT_RETIRING ||
       t->slots[source.slot].pins || n->fid_claim || n->fid_borrows)return -T_E_BUSY;
    struct loom_service_ref parent=n->parent;
    struct loom_service_node *p=fid_exact(t,nodes,parent,true);
    if(!p || t->slots[parent.slot].kind!=LOOM_SERVICE_SCOPE || !t->slots[parent.slot].pins)
        return -T_E_INVAL;
    // Exact parent remains pinned until the child identity and payload are gone.
    // Explicit bytes avoid a freestanding libc dependency for the 64-byte node.
    u8 *bytes=(u8 *)n;
    for(size_t i=0;i<sizeof(*n);++i)bytes[i]=0;
    (void)loom_slot_reap(t,source);
    (void)loom_slot_unpin(t,parent);
    return 0;
}
