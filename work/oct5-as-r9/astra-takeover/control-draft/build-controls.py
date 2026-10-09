from pathlib import Path
import json,hashlib
R=Path('/Users/northkillpd/projects/thylacine-corona');D=Path(__file__).resolve().parent
paths=['kernel/include/thylacine/loom_service_nodes.h','kernel/loom_service_nodes.c','kernel/loom_service_connect.c','kernel/CMakeLists.txt']
base={}
for name in paths:
 p=D/name;assert not p.exists(),name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes((R/name).read_bytes());base[name]=hashlib.sha256((R/name).read_bytes()).hexdigest()
(D/'base.json').write_text(json.dumps(base,indent=2)+'\n')
def edit(name,old,new):
 p=D/name;s=p.read_text();assert s.count(old)==1,(name,old,s.count(old));p.write_text(s.replace(old,new))
edit(paths[0],'    u32 reserved;','''    u32 reserved;
    s32 reason; // first scope abort reason, retained after protocol collection
    u32 status_flags; // completed-driver diagnostics only; protected by Ll
    u32 failure_phase;
    u32 diagnostic_reserved;''')
edit(paths[0],'== 24,','== 40,');edit(paths[0],'== 1536,','== 2560,')
edit(paths[1],'!node_zero_ref(nodes->nodes[i].parent) || nodes->nodes[i].reserved)', '''!node_zero_ref(nodes->nodes[i].parent) || nodes->nodes[i].reserved ||
            nodes->nodes[i].reason || nodes->nodes[i].status_flags ||
            nodes->nodes[i].failure_phase || nodes->nodes[i].diagnostic_reserved)''')
# Unindexed constructor ownership is represented by the two scope pins.
edit(paths[2],'        extinction("CONNECT lost prepared ownership");\n}', '''        extinction("CONNECT lost prepared ownership");
    struct loom_service_node *n=connect_scope(l,scope);
    if (n && n->state==LOOM_SERVICE_ABORTING && !l->service_slots->slots[scope.slot].pins) {
        n->state=LOOM_SERVICE_RETIRED;
        n->status_flags|=LOOM_SERVICE_LOCAL_RETIRED;
    }
}''')
edit(paths[2],'!n || n->state != LOOM_SERVICE_RESERVED ? -T_E_NOENT : 0;', '''n && n->state==LOOM_SERVICE_ABORTING && n->reason<0 ? n->reason :
        !n || n->state != LOOM_SERVICE_RESERVED ? -T_E_NOENT : 0;''')
edit(paths[2],'    if (!p->abort_reason) p->abort_reason = reason;', '''    if (!p->abort_reason) p->abort_reason = n->reason<0 ? n->reason : reason;
    if (!n->reason) n->reason=p->abort_reason;''')
edit(paths[2],'    struct loom_service_request *r = connect_request(p);\n    if (!p->connect_bound || (!p->admission.published', '''    struct loom_service_request *r = connect_request(p);
    if (n && n->state==LOOM_SERVICE_ABORTING && n->reason<0) connect_latch(p,n,n->reason);
    if (!p->connect_bound || (!p->admission.published''')
edit(paths[2],'    if (borrowed && loom_request_put(&l->service_requests,p->request))', '''    // Sole driver has returned: only now read its unlocked progress cursor.
    // Query conservatively reports possible bytes while driving is true.
    if (p->progress.io.bytes_sent) n->status_flags|=LOOM_SERVICE_BYTES_SENT;
    if (borrowed && loom_request_put(&l->service_requests,p->request))''')
edit(paths[2],'        p->failure_phase=attempted_phase;', '        p->failure_phase=attempted_phase; n->failure_phase=attempted_phase;')
edit(paths[2],'if (!p->users) { p->locally_retired=true; n->state=LOOM_SERVICE_RETIRED; }', '''if (!p->users) {
        p->locally_retired=true; n->state=LOOM_SERVICE_RETIRED;
        n->status_flags|=LOOM_SERVICE_LOCAL_RETIRED;
    }''')
edit(paths[3],'    loom_service_close_queue.c','    loom_service_control.c\n    loom_service_close_queue.c')
print('Prepared retained diagnostic draft; canonical source remains frozen.')
