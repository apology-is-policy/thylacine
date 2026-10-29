from pathlib import Path
import importlib.util,re,subprocess,os,json,hashlib,datetime
ROOT=Path('/Users/northkillpd/projects/thylacine-corona');D=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('connect',ROOT/'work/oct5-as-r9/connect-control.py');c=importlib.util.module_from_spec(spec);spec.loader.exec_module(c)
header=(D/'kernel/include/thylacine/loom_service_engine.h').read_text()
pre=c.PRE.replace('struct Loom {','\n'+header+'\nstruct Loom {struct loom_service_engine *service_engine;u32 service_fixed_count;int sqpoll_park;')
pre=pre.replace('bool connect_bound,driving,locally_retired;','bool connect_bound,driving,locally_retired,indexed;')
pre=pre.replace('static int held,wakes;','static int held,wakes;\nstatic void boundary_unlocked(void);')
pre=pre.replace('l->held=0;held--;}', 'l->held=0;held--;boundary_unlocked();}')
# Replace boundary doubles, retaining the real locked CONNECT and owner code.
replacements={
'loom_private_protocol_prepare':r'''static int loom_private_protocol_prepare(struct Loom *l,struct loom_service_ref target,u64 deadline,u64 now,struct loom_service_protocol **out) {
 (void)target;(void)deadline;(void)now;
 CHECK(!held && l->service_requests.occupied,"request reservation before protocol allocation");
 allocations++;active=calloc(1,sizeof(*active));CHECK(active,"host allocation");
 active->owner=l;active->admission.conn=&cn;*out=active;return 0;
}''',
'loom_private_protocol_release_unused':r'''static int loom_private_protocol_release_unused(struct loom_service_protocol *p) {
 CHECK(!held && !p->indexed && !p->connect_bound && !p->admission.published,"unpublished owner exclusive cleanup");
 releases++;free(p);active=NULL;return 0;
}''',
'loom_private_protocol_release_retired':r'''static int loom_private_protocol_release_retired(struct loom_service_protocol *p) {
 CHECK(!held && p->locally_retired && !p->indexed && !p->connect_bound && !p->users && !p->driving &&
       !p->owner->service_slots->slots[p->scope.slot].pins,"indexed lifetime ends after pins and terminal");
 releases++;free(p);active=NULL;return 0;
}''',
'devsrv_connect_publish':r'''static int devsrv_connect_publish(struct SrvConnectAdmission *a) {
 CHECK(held && active->indexed,"native publication has indexed lifetime");
 struct loom_engine_entry *e=&active->owner->service_engine->entries[active->scope.slot];
 CHECK(e->phase==LENGINE_INSTALLING && e->pins==1,"installation pin covers native publication");
 if(mode==3)return -T_E_NOENT;
 a->published=true;publishes++;return 0;
}''',
'devsrv_connect_wake':r'''static void devsrv_connect_wake(struct SrvConnectAdmission *a) {
 CHECK(!held && a->published,"native accept wake outside lock");accept_wakes++;
 struct Loom *l=active->owner;struct loom_service_protocol *p=NULL;
 CHECK(loom_engine_get(l,active->scope,&p)==-T_E_AGAIN && !p &&
       loom_engine_collect(l,active->scope)==-T_E_BUSY,"accept wake retains installing lifetime");
 if(mode==2)CHECK(!loom_engine_abort(l,active->scope,-T_E_CANCELED),"abort during accept wake");
 if(mode==5)close_owner(l);
}''',
'srvconn_teardown':r'''static void srvconn_teardown(struct SrvConn *conn) {
 CHECK(!held && conn==&cn,"teardown outside lock");conn->dead=true;teardowns++;
}''',
'p9_client_progress_step':r'''static int p9_client_progress_step(struct p9_client *client,u64 now) {
 (void)now;active=(void *)((u8 *)client-__builtin_offsetof(struct loom_service_protocol,client));
 CHECK(!held && active->driving && active->owner->service_requests.records[active->request.index].borrows==1,
       "entry owns parser borrow during client step");
 steps++;
 if(mode==6 && active->scope.slot==0)return 0;
 ++active->client.dummy;
 active->progress.handshake.phase=active->client.dummy==1?P9_HS_ATTACH_RECV:P9_HS_READY;
 return 1;
}''',
'p9_client_progress_abort':r'''static void p9_client_progress_abort(struct p9_client *client) {
 active=(void *)((u8 *)client-__builtin_offsetof(struct loom_service_protocol,client));
 CHECK(!held && active->driving && !active->owner->service_requests.records[active->request.index].borrows,"cursor abort outside lock after parser borrow");aborts++;
}'''
}
for fn,new in replacements.items():pre=pre.replace(c.post.extract.fun(pre,fn),new)
pre=pre.replace('static struct loom_service_protocol *active;', 'static struct loom_service_protocol *active, *externally_pinned;')
pre=pre.replace('CHECK(!held && p->locally_retired', 'CHECK(p!=externally_pinned,"external entry prevents storage release");\n CHECK(!held && p->locally_retired')
pre+=r'''
static int hook_armed;
static void boundary_unlocked(void) {
 if(!hook_armed || !active || !active->indexed)return;
 struct loom_engine_entry *e=&active->owner->service_engine->entries[active->scope.slot];
 if(e->phase!=LENGINE_INSTALLING)return;
 hook_armed=0;
 if(mode==1)CHECK(!loom_engine_abort(active->owner,active->scope,-T_E_CANCELED),"abort before publication");
 if(mode==7)close_owner(active->owner);
}
static void wakeup(int *r) {(void)r;CHECK(!held,"owner change wakes outside lock");}
int loom_private_post_terminal(struct Loom *,struct loom_request_ref);
'''
main=r'''
static void destination(struct Loom *l,struct loom_service_ref *ref) {
 CHECK(!loom_service_node_prepare(l->service_slots,l->service_nodes,LOOM_SERVICE_SCOPE,(struct loom_service_ref){0},ref) &&
       !loom_service_node_publish(l->service_slots,l->service_nodes,*ref),"destination control");
}
int main(int argc,char **argv) {
 mode=argc>1?atoi(argv[1]):0;
 struct loom_slot_table slots={0};struct loom_service_node_table nodes={0};struct loom_service_request records[2]={0};
 struct {struct loom_ring_hdr hdr;struct loom_cqe cq[2];struct loom_service_buffer_receipt receipts[2];} ring={0};
 struct loom_service_engine index={0};
 struct Loom l={.service_engine=&index,.magic=LOOM_MAGIC,.service_as=&ring,.service_slots=&slots,.service_nodes=&nodes,
 .ring_kva=(u8 *)&ring,.cq_entries=2,.cqe_off=(u32)((u8 *)ring.cq-(u8 *)&ring),
 .receipt_off=(u32)((u8 *)ring.receipts-(u8 *)&ring),.receipt_size=sizeof(ring.receipts),
 .service_requests={.records=records,.capacity=2}};
 struct loom_service_ref scope,second={0},target={.slot=9,.incarnation=5};destination(&l,&scope);
 struct loom_service_protocol *p=NULL,*q=NULL,*pin=NULL;
 CHECK(!loom_private_connect_prepare(&l,scope,target,77,100,1,&p),"prepare control");
 if(mode==9 || mode==10) {
  close_owner(&l);
  if(mode==10)CHECK(loom_engine_discard_closed(&l)==-T_E_BUSY && !l.service_requests.detached,"unindexed constructor prevents consumer detach");
  CHECK(loom_engine_adopt(p,2)==-T_E_CANCELED && !p->indexed && loom_engine_empty(l.service_engine),"refused adoption preserves caller ownership");
  CHECK(!loom_private_connect_rollback(p),"refused adoption rolls back");
  CHECK(!loom_engine_discard_closed(&l),"constructor unwind permits consumer detach");puts("PASS engine control");return 0;
 }
 hook_armed=mode==1 || mode==7;
 CHECK(!loom_engine_adopt(p,mode==4?100:2) && p->indexed && l.service_engine->count==1 &&
       l.service_engine->entries[scope.slot].phase==LENGINE_LIVE,"indexed adoption control");
 CHECK(loom_private_connect_rollback(p)==-T_E_BUSY && loom_private_connect_release(p)==-T_E_BUSY,"raw caller cannot free indexed storage");
 struct loom_service_ref stale=scope;stale.incarnation++;
 CHECK(loom_engine_get(&l,stale,&pin)==-T_E_NOENT && !pin,"stale lookup leaves output unchanged");
 CHECK(!loom_engine_get(&l,scope,&pin) && pin==p,"exact lookup retains entry");externally_pinned=pin;
 if(mode==6) {
  destination(&l,&second);
  CHECK(!loom_private_connect_prepare(&l,second,target,88,100,1,&q) && !loom_engine_adopt(q,2),"second scope control");
  for(int i=0;i<4;i++)CHECK(loom_engine_visit(&l,3)==1,"round robin visits");
  CHECK(records[p->request.index].phase==LREQ_ACTIVE && records[q->request.index].phase==LREQ_DELIVERED &&
        p->client.dummy==0 && q->client.dummy==2,"stalled first peer does not starve second");
 }
 if(mode==0 || mode==8) {
  if(mode==8)l.cq_tail=2;
  CHECK(loom_engine_visit(&l,3)==1 && loom_engine_visit(&l,4)==1 && nodes.nodes[scope.slot].state==LOOM_SERVICE_READY,"owner drives handshake to READY");
  CHECK(!loom_engine_abort(&l,scope,-T_E_CANCELED),"indexed abort");
 }
 if(mode==6) {
  CHECK(!loom_engine_abort(&l,scope,-T_E_CANCELED) && !loom_engine_abort(&l,second,-T_E_CANCELED),"independent scope aborts");
 }
 if(mode==5 || mode==7)l.cq_tail=2; // close must progress with no CQ space
 for(int i=0;i<4;i++)(void)loom_engine_visit(&l,5);
 CHECK(p->locally_retired && releases==(mode==6?1:0) && loom_engine_collect(&l,scope)==-T_E_BUSY,
       "held entry prevents collection after retirement");
 if(mode==5 || mode==7) {
  CHECK(loom_engine_discard_closed(&l)==-T_E_BUSY && !l.service_requests.detached,
        "live entry prevents consumer detach");
 }
 CHECK(loom_engine_put(&l,stale,p)==-T_E_NOENT,"stale put cannot drop entry");
 CHECK(!loom_engine_put(&l,scope,pin),"last external entry put");pin=NULL;externally_pinned=NULL;
 if(mode==5 || mode==7) {
  CHECK(!loom_engine_discard_closed(&l) && l.service_requests.detached,"closed full CQ discards after local quiescence");
 } else {
  if(mode==8) {
   CHECK(loom_engine_collect(&l,scope)==-T_E_BUSY,"undelivered terminal blocks indexed collection");
   ring.hdr.cq_head=l.cq_tail;
   CHECK(loom_engine_visit(&l,6)==1,"CQ drain resumes indexed terminal delivery");
  } else (void)loom_engine_collect(&l,scope);
  if(mode==6) {ring.hdr.cq_head=l.cq_tail;for(int i=0;i<4;i++)(void)loom_engine_visit(&l,6);}
 }
 CHECK(loom_engine_empty(l.service_engine) && releases==allocations && !l.service_requests.occupied,
       "all indexed ownership settles exactly once");
 CHECK(loom_engine_get(&l,scope,&pin)==-T_E_NOENT && !pin && !loom_private_connect_reap(&l,scope),"collected pointer unavailable; metadata reaps");
 if(mode==6)CHECK(!loom_private_connect_reap(&l,second),"second metadata reaps");
 CHECK(!held,"balanced locks");puts("PASS engine control");return 0;
}
'''
def body(path):return re.sub(r'^#include[^\n]*\n','',path.read_text(),flags=re.M)

MUTANTS=[
 ('ignore-unindexed-constructor','if (r->phase == LREQ_FREE) continue;','if (true) continue;',10,'unindexed constructor prevents consumer detach'),
 ('omit-indexed','p->indexed=true;','p->indexed=false;',0,'native publication has indexed lifetime'),
 ('omit-installer-pin','.pins=1,.phase=LENGINE_INSTALLING','.pins=0,.phase=LENGINE_INSTALLING',0,'installation pin covers native publication'),
 ('expose-installing','if (!installing && e->phase != LENGINE_LIVE)','if (!installing && e->phase != LENGINE_LIVE && false)',0,'accept wake retains installing lifetime'),
 ('stale-lookup','e->incarnation == ref.incarnation','true',0,'stale lookup leaves output unchanged'),
 ('drop-pinned-owner','if (e->phase != LENGINE_LIVE || e->pins || !p->locally_retired || p->users || p->driving ||','if (e->phase != LENGINE_LIVE || !p->locally_retired || p->users || p->driving ||',0,'external entry prevents storage release'),
 ('detach-live-entry','if (e->phase != LENGINE_LIVE || e->pins || !p->locally_retired || p->users || p->driving) {','if (e->phase != LENGINE_LIVE || !p->locally_retired || p->users || p->driving) {',5,'live entry prevents consumer detach'),
 ('forget-round-robin','l->service_engine->cursor=(at+1u)%LOOM_SERVICE_SLOT_COUNT;','l->service_engine->cursor=0;',6,'stalled first peer does not starve second'),
 ('collect-undelivered','!r || r->phase != LREQ_DELIVERED || r->borrows','!r || r->phase == LREQ_FREE || r->borrows',8,'engine lost final CONNECT ownership'),
]
out=D/('controls-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));out.mkdir()
engine=body(D/'kernel/loom_service_engine.c');connect=body(D/'kernel/loom_service_connect.c')
paths=[D/'kernel/loom_service_connect.c',D/'kernel/loom_service_engine.c',D/'kernel/loom_service_requests.c',D/'kernel/include/thylacine/loom_service_requests.h',D/'kernel/include/thylacine/loom_service_engine.h',Path(__file__)]
hashes={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths};results=[]
for tag,old,new,mode,want in [('control',None,None,None,None)]+MUTANTS:
 text=engine
 if old:
  assert text.count(old)==1,(tag,text.count(old));text=text.replace(old,new)
 src=out/(tag+'.c');src.write_text(pre+connect+text+c.post.extract.fun((ROOT/'kernel/loom.c').read_text(),'loom_private_post_terminal')+main)
 exe=out/tag
 cmd=['/opt/homebrew/opt/llvm@22/bin/clang','-std=c11','-O1','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-isysroot',subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip(),'-I',str(D/'kernel/include'),'-I',str(ROOT/'kernel/include'),str(src),str(D/'kernel/loom_service_requests.c'),str(ROOT/'kernel/loom_service_slots.c'),str(ROOT/'kernel/loom_service_nodes.c'),'-o',str(exe)]
 (out/(tag+'-command.json')).write_text(json.dumps(cmd)+'\n')
 with (out/(tag+'-build.log')).open('wb') as f:subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT,check=True)
 for case in range(11) if mode is None else [mode]:
  r=subprocess.run([str(exe),str(case)],capture_output=True,text=True,timeout=15)
  (out/(tag+'-'+str(case)+'.log')).write_text(r.stdout+r.stderr)
  assert r.returncode==(11 if want else 0) and r.stdout==(want or 'PASS engine control')+'\n' and not r.stderr,(tag,case,r.returncode,r.stdout,r.stderr)
  results.append({'case':tag,'mode':case,'expected':want,'rc':r.returncode,'source_hash':hashlib.sha256(src.read_bytes()).hexdigest()})
 print('PASS',tag,flush=True)
assert hashes=={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
(out/'result.json').write_text(json.dumps({'hashes':hashes,'controls':results,'limits':'actual draft engine/connect/requests/slots/nodes/CQ; checked boundary doubles, deterministic interleavings; not native qualification'},indent=2)+'\n');print(out)
