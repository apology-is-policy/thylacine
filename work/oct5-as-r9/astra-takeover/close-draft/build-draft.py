from pathlib import Path
import hashlib,json
R=Path('/Users/northkillpd/projects/thylacine-corona');D=Path(__file__).resolve().parent
files=['kernel/loom.c','kernel/include/thylacine/loom.h','kernel/loom_service_engine.c','kernel/include/thylacine/loom_service_engine.h','kernel/loom_service_connect.c','kernel/include/thylacine/loom_service_connect.h','kernel/CMakeLists.txt']
base={}
for name in files:
 p=R/name;q=D/name;q.parent.mkdir(parents=True,exist_ok=True);assert not q.exists(),name
 q.write_bytes(p.read_bytes());base[name]=hashlib.sha256(p.read_bytes()).hexdigest()
(D/'integration-base.json').write_text(json.dumps(base,indent=2)+'\n')
def edit(name,old,new):
 p=D/name;s=p.read_text();assert s.count(old)==1,(name,old[:100],s.count(old));p.write_text(s.replace(old,new))
def append(name,s):
 p=D/name;p.write_text(p.read_text()+s)
# Header links and notification contract.
edit('kernel/include/thylacine/loom.h','#include <thylacine/loom_service_abi.h>','#include <thylacine/loom_service_abi.h>\n#include <thylacine/loom_service_close_queue.h>')
edit('kernel/include/thylacine/loom.h','    u32 service_fixed_count;','    u32 service_fixed_settling; // physical drops outside Ll, still local borrows\n    u32 service_fixed_count;')
edit('kernel/include/thylacine/loom.h','    struct Loom *service_retire_next;','    struct loom_close_work service_close_work; // Q-locked; one root until DONE\n    struct Loom *service_retire_next;')
edit('kernel/include/thylacine/loom.h','void loom_handle_closed(struct Loom *l);','void loom_handle_closed(struct Loom *l);\n// External close prerequisite changed; caller retains l, no outer locks.\n// Call only AFTER ending the physical borrow; internal close puts are quiet.\nvoid loom_private_close_changed(struct Loom *l);')
edit('kernel/CMakeLists.txt','    loom_service_engine.c','    loom_service_close_queue.c\n    loom_service_engine.c')
edit('kernel/include/thylacine/loom_service_engine.h','bool loom_engine_empty(','''// One close quantum: 0 blocked on external local holders, 1 immediate work,
// 2 complete (index/request/fixed ownership empty after consumer detach).
// Retirer owns a queue root; its internal puts never kick its own RUNNING work.
int loom_engine_close_visit(struct Loom *);
bool loom_engine_empty(''')
edit('kernel/include/thylacine/loom_service_connect.h','int loom_private_connect_step(struct loom_service_protocol *, u64 now);','''int loom_private_connect_step(struct loom_service_protocol *, u64 now);
// Internal retiring driver variants: caller retains l and entry storage; do not
// kick the close queue for its own changes. External concurrent puts still kick.
int loom_private_connect_step_retiring(struct loom_service_protocol *);
int loom_private_connect_release_retiring(struct loom_service_protocol *);''')
# CONNECT helpers retain a temporary Loom root across consuming raw APIs, so
# callbacks after physical release cannot touch a freed owner.
edit('kernel/loom_service_connect.c','''        connect_rollback_locked(l,scope,request);
        spin_unlock(&l->lock);
        return rc;''','''        connect_rollback_locked(l,scope,request);
        spin_unlock(&l->lock);
        loom_private_close_changed(l);
        return rc;''')
edit('kernel/loom_service_connect.c','int loom_private_connect_rollback(struct loom_service_protocol *p) {','static int connect_rollback_consume(struct loom_service_protocol *p) {')
edit('kernel/loom_service_connect.c','int loom_private_connect_publish(struct loom_service_protocol *p, u64 now) {','''int loom_private_connect_rollback(struct loom_service_protocol *p) {
    if (!p || !p->owner) return -T_E_INVAL;
    struct Loom *l=p->owner;
    loom_ref(l);
    int rc=connect_rollback_consume(p);
    if (!rc) loom_private_close_changed(l);
    loom_unref(l);
    return rc;
}
int loom_private_connect_publish(struct loom_service_protocol *p, u64 now) {''')
edit('kernel/loom_service_connect.c','''    --p->users;
    spin_unlock(&l->lock);
    return 0;''','''    --p->users;
    spin_unlock(&l->lock);
    loom_private_close_changed(l);
    return 0;''')
edit('kernel/loom_service_connect.c','int loom_private_connect_step(struct loom_service_protocol *p, u64 now) {','static int connect_step(struct loom_service_protocol *p, u64 now, bool notify) {')
edit('kernel/loom_service_connect.c','''        p->driving=false;
        spin_unlock(&l->lock);
        return rc;''','''        p->driving=false;
        spin_unlock(&l->lock);
        if (notify) loom_private_close_changed(l);
        return rc;''')
edit('kernel/loom_service_connect.c','''    p->driving=false;
    spin_unlock(&l->lock);
    return reason;''','''    p->driving=false;
    spin_unlock(&l->lock);
    if (notify) loom_private_close_changed(l);
    return reason;''')
edit('kernel/loom_service_connect.c','int loom_private_connect_release(struct loom_service_protocol *p) {','''int loom_private_connect_step(struct loom_service_protocol *p, u64 now) {
    return connect_step(p,now,true);
}
int loom_private_connect_step_retiring(struct loom_service_protocol *p) {
    return connect_step(p,0,false);
}
int loom_private_connect_release_retiring(struct loom_service_protocol *p) {''')
edit('kernel/loom_service_connect.c','int loom_private_connect_reap(struct Loom *l, struct loom_service_ref scope) {','''int loom_private_connect_release(struct loom_service_protocol *p) {
    if (!p || !p->owner) return -T_E_INVAL;
    struct Loom *l=p->owner;
    loom_ref(l);
    int rc=loom_private_connect_release_retiring(p);
    if (!rc) loom_private_close_changed(l);
    loom_unref(l);
    return rc;
}
int loom_private_connect_reap(struct Loom *l, struct loom_service_ref scope) {''')
# Engine quiet variants prevent its own close driver from rescheduling itself.
edit('kernel/loom_service_engine.c','''    wakeup(&l->sqpoll_park);
}''','''    wakeup(&l->sqpoll_park);
    loom_private_close_changed(l);
}''')
edit('kernel/loom_service_engine.c','int loom_engine_put(struct Loom *l, struct loom_service_ref ref, struct loom_service_protocol *p) {','static int engine_put(struct Loom *l, struct loom_service_ref ref, struct loom_service_protocol *p, bool notify) {')
edit('kernel/loom_service_engine.c','    if (!rc) engine_changed(l);','    if (!rc && notify) engine_changed(l);')
edit('kernel/loom_service_engine.c','int loom_engine_abort(struct Loom *l, struct loom_service_ref ref, s32 reason) {','''int loom_engine_put(struct Loom *l, struct loom_service_ref ref, struct loom_service_protocol *p) {
    return engine_put(l,ref,p,true);
}
int loom_engine_abort(struct Loom *l, struct loom_service_ref ref, s32 reason) {''')
edit('kernel/loom_service_engine.c','int loom_engine_collect(struct Loom *l, struct loom_service_ref ref) {','static int engine_collect(struct Loom *l, struct loom_service_ref ref, bool notify) {')
edit('kernel/loom_service_engine.c','''    if (loom_private_connect_release(p)) extinction("engine lost final CONNECT ownership");
    engine_changed(l);''','''    if (loom_private_connect_release_retiring(p)) extinction("engine lost final CONNECT ownership");
    if (notify) engine_changed(l);''')
edit('kernel/loom_service_engine.c','int loom_engine_visit(struct Loom *l, u64 now) {','''int loom_engine_collect(struct Loom *l, struct loom_service_ref ref) {
    return engine_collect(l,ref,true);
}
int loom_engine_visit(struct Loom *l, u64 now) {''')
edit('kernel/loom_service_engine.c','int loom_engine_discard_closed(struct Loom *l) {','static int engine_discard_closed(struct Loom *l, bool notify) {')
edit('kernel/loom_service_engine.c','if (!l->service_closing || l->service_fixed_count)', 'if (!l->service_closing || l->service_fixed_count || l->service_fixed_settling)')
edit('kernel/loom_service_engine.c','for (u32 i=0;i<count;++i) (void)loom_engine_collect(l,refs[i]);','for (u32 i=0;i<count;++i) (void)engine_collect(l,refs[i],notify);')
append('kernel/loom_service_engine.c','''
int loom_engine_discard_closed(struct Loom *l) {
    return engine_discard_closed(l,true);
}
int loom_engine_close_visit(struct Loom *l) {
    if (!l || !loom_is_private(l)) return -T_E_INVAL;
    struct loom_service_protocol *p=NULL;
    struct loom_service_ref ref={0};
    spin_lock(&l->lock);
    if (!l->service_closing) { spin_unlock(&l->lock); return -T_E_INVAL; }
    for (u32 n=0;n<LOOM_SERVICE_SLOT_COUNT;++n) {
        u32 at=l->service_engine->cursor;
        l->service_engine->cursor=(at+1u)%LOOM_SERVICE_SLOT_COUNT;
        struct loom_engine_entry *e=&l->service_engine->entries[at];
        if (e->phase != LENGINE_LIVE || e->pins==UINT32_MAX ||
            e->protocol->driving || e->protocol->users || e->protocol->locally_retired) continue;
        ++e->pins; p=e->protocol;
        ref=(struct loom_service_ref){.slot=at,.incarnation=e->incarnation};
        break;
    }
    spin_unlock(&l->lock);
    if (p) {
        // service_closing latches cancel before any native client step. No CQ
        // post/readiness wait: this visit ends the actual local cursor instead.
        (void)loom_private_connect_step_retiring(p);
        if (engine_put(l,ref,p,false)) extinction("close driver lost entry pin");
    }
    if (!engine_discard_closed(l,false)) return 2;
    spin_lock(&l->lock);
    bool more=false;
    for (u32 i=0;i<LOOM_SERVICE_SLOT_COUNT;++i) {
        struct loom_engine_entry *e=&l->service_engine->entries[i];
        if (e->phase==LENGINE_LIVE && e->pins<UINT32_MAX &&
            !e->protocol->locally_retired && !e->protocol->driving && !e->protocol->users) more=true;
    }
    spin_unlock(&l->lock);
    // An external return between this observation and Q finish sets dirty;
    // a return after PARKED appends it once. Internal entry returns are quiet.
    return more ? 1 : 0;
}
''')
# Loom wrapper: no reference operation can ever resurrect the zero-ref queue.
edit('kernel/loom.c','static bool service_retire_ready;','static bool service_retire_ready;\nstatic struct loom_close_queue service_close_queue;')
edit('kernel/loom.c','    service_retire_ready = true;','    loom_closeq_init(&service_close_queue);\n    service_retire_ready = true;')
edit('kernel/loom.c','''void loom_handle_closed(struct Loom *l) {
    if (!loom_is_private(l)) return;
    loom_exec_latch(l);
    poll_waiter_list_wake(&l->cq_waiters);
    wakeup(&l->sqpoll_park);
}

static void loom_private_enqueue(struct Loom *l) {
    loom_handle_closed(l);''','''static void loom_private_close_notify(struct Loom *l) {
    loom_exec_latch(l);
    poll_waiter_list_wake(&l->cq_waiters);
    wakeup(&l->sqpoll_park);
}
void loom_private_close_changed(struct Loom *l) {
    if (!loom_is_private(l)) return;
    spin_lock(&service_retire_lock);
    bool wake=loom_closeq_kick(&service_close_queue,&l->service_close_work);
    spin_unlock(&service_retire_lock);
    if (wake) wakeup(&service_retire_wait);
}
void loom_handle_closed(struct Loom *l) {
    if (!loom_is_private(l)) return;
    // Actual handle close retains l, outside process/handle-table locks. Exec's
    // earlier leaf latch deliberately does not enter this scheduler path.
    loom_private_close_notify(l);
    spin_lock(&service_retire_lock);
    bool started=loom_closeq_start(&service_close_queue,&l->service_close_work);
    if (started) loom_ref(l); // caller still holds a live ref; never resurrection
    spin_unlock(&service_retire_lock);
    if (started) wakeup(&service_retire_wait);
}

static void loom_private_enqueue(struct Loom *l) {
    // refcount is already zero. Do not call the root-taking handle-close path.
    loom_private_close_notify(l);''')
edit('kernel/loom.c','    int pending = service_retire_head != NULL;','    int pending = service_retire_head != NULL || service_close_queue.head != NULL;')
edit('kernel/loom.c','if (l->service_fixed || l->service_fixed_count)','if (l->service_fixed || l->service_fixed_count || l->service_fixed_settling)')
edit('kernel/loom.c','''            int rc = s->phase == LSLOT_PROVISIONAL ?
                loom_service_node_rollback''','''            struct loom_service_node *n=&l->service_nodes->nodes[i];
            if (kinds[pass]==LOOM_SERVICE_SCOPE && n->state==LOOM_SERVICE_RETIRED) {
                // Protocol/index/request cleanup precedes final zero-ref free.
                // Unused children were removed above; no caller can still own
                // a provisional child here. READY/ABORTING never use this path.
                if (s->phase!=LSLOT_LIVE || s->pins || n->parent.slot ||
                    n->parent.reserved || n->parent.incarnation || n->reserved)
                    extinction("private Loom retained pinned retired scope");
                *n=(struct loom_service_node){0};
                if (loom_slot_retire(l->service_slots,ref) || loom_slot_reap(l->service_slots,ref))
                    extinction("private Loom lost retired scope metadata");
                continue;
            }
            int rc = s->phase == LSLOT_PROVISIONAL ?
                loom_service_node_rollback''')
edit('kernel/loom.c','''        struct Loom *l = service_retire_head;
        if (l) {''','''        struct loom_close_work *work=loom_closeq_take(&service_close_queue);
        struct Loom *l = service_retire_head;
        if (l) {''')
edit('kernel/loom.c','''        if (l) loom_private_destroy(l);
    }
}''','''        if (work) {
            struct Loom *closing=(void *)((char *)work-__builtin_offsetof(struct Loom,service_close_work));
            int progress=loom_engine_close_visit(closing);
            if (progress<0) extinction("close queue reached invalid private owner");
            spin_lock(&service_retire_lock);
            enum loom_close_finish action=loom_closeq_finish(&service_close_queue,work,progress==2,progress==1);
            spin_unlock(&service_retire_lock);
            if (action==LCLOSE_DROP) loom_unref(closing); // final drop outside Q
        }
        if (l) loom_private_destroy(l);
    }
}''')
edit('kernel/loom.c','''    --l->service_fixed_count;
    *f = (struct loom_private_fixed){0};''','''    --l->service_fixed_count;
    ++l->service_fixed_settling;
    *f = (struct loom_private_fixed){0};''')
edit('kernel/loom.c','''    loom_private_drop_buffer(as,b);
    loom_unref(l);
    return 0;''','''    loom_private_drop_buffer(as,b);
    spin_lock(&l->lock);
    --l->service_fixed_settling;
    spin_unlock(&l->lock);
    loom_private_close_changed(l);
    loom_unref(l);
    return 0;''')
print('Draft integration written; canonical files unchanged.')
