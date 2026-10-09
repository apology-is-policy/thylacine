#!/usr/bin/env python3
"""Actual CONNECT + request/slot/node/CQ code, checked protocol/native boundary doubles.
Deterministic nested entries exercise cancellation/driver ordering; no claim of
native wire fidelity or concurrent SMP stress from this host fixture.
"""
from pathlib import Path
import datetime,difflib,hashlib,importlib.util,json,os,re,subprocess
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('post',Path(__file__).with_name('request-post-control.py'))
post=importlib.util.module_from_spec(spec);spec.loader.exec_module(post)
PRE=post.PRE.replace('struct Loom {u32 magic;', 'struct Loom {struct loom_slot_table *service_slots;struct loom_service_node_table *service_nodes;u32 magic;')+r'''
enum {P9_HS_VERSION_SEND,P9_HS_VERSION_RECV,P9_HS_ATTACH_SEND,P9_HS_ATTACH_RECV,P9_HS_READY};
struct SrvConn {bool dead;};
struct SrvConnectAdmission {struct SrvConn *conn;bool published;};
struct p9_client {int dummy;};
struct loom_service_protocol {
 struct Loom *owner;struct SrvConnectAdmission admission;struct p9_client client;
 struct {struct {u32 phase;} handshake;} progress;
 struct loom_service_ref scope;struct loom_request_ref request;u64 connect_deadline;
 s32 abort_reason;u32 users,failure_phase;bool connect_bound,driving,locally_retired,indexed;
};
static struct loom_service_protocol *active;
static struct SrvConn cn;
static int mode,allocations,releases,steps,aborts,teardowns,publishes,accept_wakes;
static void extinction(const char *s) {CHECK(false,s);}
int loom_private_connect_step(struct loom_service_protocol *,u64);
int loom_private_connect_abort(struct loom_service_protocol *,s32);
int loom_private_connect_release(struct loom_service_protocol *);
static void close_owner(struct Loom *l) {l->service_closing=true;l->service_slots->closing=true;}
static int loom_private_protocol_prepare(struct Loom *l,struct loom_service_ref target,u64 deadline,u64 now,struct loom_service_protocol **out) {
 (void)target;(void)deadline;(void)now;
 CHECK(!held && l->service_requests.occupied==1,"terminal reserved before allocation");
 struct loom_service_request *r=&l->service_requests.records[0];
 CHECK(r->phase==LREQ_PREPARED && l->service_slots->slots[r->scope.slot].pins==2,"two exact scope pins before allocation");
 if(mode==10)return -T_E_NOMEM;
 allocations++;active=calloc(1,sizeof(*active));CHECK(active,"host allocation");
 active->owner=l;active->admission.conn=&cn;*out=active;return 0;
}
static int loom_private_protocol_release_unused(struct loom_service_protocol *p) {
 CHECK(!held && !p->connect_bound && !p->admission.published && !p->owner->service_requests.occupied &&
       !p->owner->service_slots->slots[p->scope.slot].pins,"rollback ledger before native release");
 releases++;free(p);active=NULL;return 0;
}
static int loom_private_protocol_release_retired(struct loom_service_protocol *p) {
 CHECK(!held && p->locally_retired && !p->connect_bound && !p->users && !p->driving && aborts &&
       !p->owner->service_requests.occupied && !p->owner->service_slots->slots[p->scope.slot].pins,
       "retired cleanup after ledger pins and protocol users");
 releases++;free(p);active=NULL;return 0;
}
static int devsrv_connect_publish(struct SrvConnectAdmission *a) {
 CHECK(held && active->owner->service_requests.records[0].phase==LREQ_PREPARED,"publication holds terminal reservation and lock");
 if(mode==9)return -T_E_NOENT;
 a->published=true;publishes++;return 0;
}
static void devsrv_connect_wake(struct SrvConnectAdmission *a) {
 CHECK(!held && a->published && active->owner->service_requests.records[0].phase==LREQ_ACTIVE,"accept wake after atomic admission outside lock");accept_wakes++;
}
static void srvconn_teardown(struct SrvConn *c) {
 CHECK(!held && c==&cn,"teardown outside lock");c->dead=true;teardowns++;
 if(mode==6 && teardowns==1) {
  CHECK(active->users==1,"abort pins transport during teardown");
  CHECK(loom_private_connect_step(active,4)==-T_E_CANCELED && !active->locally_retired &&
        active->owner->service_nodes->nodes[active->scope.slot].state==LOOM_SERVICE_ABORTING,
        "live abort user delays local retirement");
  CHECK(loom_private_connect_release(active)==-T_E_BUSY,"live abort user prevents free");
 }
}
static int p9_client_progress_step(struct p9_client *c,u64 now) {
 (void)c;(void)now;steps++;
 CHECK(!held && active->driving && active->owner->service_requests.records[0].borrows==1,"driver owns request borrow outside lock");
 CHECK(loom_private_connect_step(active,4)==-T_E_BUSY,"second driver refused");
 if(mode==2) CHECK(!loom_private_connect_abort(active,-T_E_CANCELED),"abort during client step");
 if(mode==4)close_owner(active->owner);
 if(mode==5)return -T_E_INVAL;
 active->progress.handshake.phase=steps==1?P9_HS_ATTACH_RECV:P9_HS_READY;
 return 1; // IO progress is not itself handshake completion.
}
static void p9_client_progress_abort(struct p9_client *c) {
 (void)c;CHECK(!held && active->driving && !active->owner->service_requests.records[0].borrows,"local abort after parser borrow outside lock");aborts++;
}
'''
MAIN=r'''
int main(int argc,char **argv) {
 mode=argc>1?atoi(argv[1]):0;
 struct loom_slot_table slots={0};struct loom_service_node_table nodes={0};
 struct loom_service_request records[2]={0};
 struct {struct loom_ring_hdr hdr;struct loom_cqe cq[2];struct loom_service_buffer_receipt receipts[2];} ring={0};
 struct Loom l={.magic=LOOM_MAGIC,.service_as=&ring,.service_slots=&slots,.service_nodes=&nodes,
 .ring_kva=(u8 *)&ring,.cq_entries=2,.cq_tail=2,
 .cqe_off=(u32)((u8 *)ring.cq-(u8 *)&ring),.receipt_off=(u32)((u8 *)ring.receipts-(u8 *)&ring),.receipt_size=sizeof(ring.receipts),
 .service_requests={.records=records,.capacity=2}};
 struct loom_service_ref scope,target={.slot=9,.incarnation=5};
 CHECK(!loom_service_node_prepare(&slots,&nodes,LOOM_SERVICE_SCOPE,(struct loom_service_ref){0},&scope) &&
       !loom_service_node_publish(&slots,&nodes,scope),"destination control");
 struct loom_service_protocol *p=NULL,*other=NULL;
 int rc=loom_private_connect_prepare(&l,scope,target,77,100,1,&p);
 if(mode==10) {CHECK(rc==-T_E_NOMEM && !p && !l.service_requests.occupied && !slots.slots[scope.slot].pins && !allocations,"allocation refusal unwinds request and pins");puts("PASS CONNECT control");return 0;}
 CHECK(!rc && p && allocations==1,"prepared control");
 CHECK(loom_private_connect_prepare(&l,scope,target,88,100,1,&other)==-T_E_BUSY && !other && allocations==1,"duplicate CONNECT refused before native allocation");
 if(mode==8)close_owner(&l);
 rc=loom_private_connect_publish(p,mode==7?100:2);
 if(mode>=7 && mode<=9) {
  CHECK(rc==(mode==7?-T_E_TIMEDOUT:mode==8?-T_E_CANCELED:-T_E_NOENT) && !publishes && !accept_wakes && !steps && records[0].phase==LREQ_PREPARED,"failed publication has no effects");
  CHECK(!loom_private_connect_rollback(p) && releases==1,"prepared rollback");puts("PASS CONNECT control");return 0;
 }
 if(mode==5)p->progress.handshake.phase=P9_HS_ATTACH_RECV;
 CHECK(!rc && publishes==1 && accept_wakes==1 && records[0].phase==LREQ_ACTIVE && nodes.nodes[scope.slot].state==LOOM_SERVICE_ADMITTED,"admitted control");
 CHECK(loom_private_connect_rollback(p)==-T_E_BUSY && loom_private_connect_release(p)==-T_E_BUSY,"published owner cannot free early");
 if(mode==1 || mode==6) {
  CHECK(!loom_private_connect_abort(p,-T_E_CANCELED) && !p->locally_retired && nodes.nodes[scope.slot].state==LOOM_SERVICE_ABORTING,"acknowledgement not retirement");
  CHECK(loom_private_connect_abort(p,-T_E_IO)==1 && p->abort_reason==-T_E_CANCELED,"first reason wins");
 }
 rc=loom_private_connect_step(p,mode==3?100:3);
 s32 result=mode==0?(s32)scope.slot:mode==3?-T_E_TIMEDOUT:mode==5?-T_E_IO:-T_E_CANCELED;
 if(mode==0) {
  CHECK(!rc && records[0].phase==LREQ_ACTIVE && nodes.nodes[scope.slot].state==LOOM_SERVICE_ATTACH,"IO progress is not READY");
  CHECK(loom_private_connect_step(p,4)==1 && records[0].result==result && nodes.nodes[scope.slot].state==LOOM_SERVICE_READY,"handshake READY commits success");
  CHECK(loom_private_connect_step(p,101)==1 && !p->abort_reason && steps==2,"deadline ends on success");
  CHECK(!loom_private_connect_abort(p,-T_E_CANCELED),"abort after success");
  rc=loom_private_connect_step(p,5);
 }
 CHECK(rc==(mode==0?-T_E_CANCELED:result) && p->locally_retired && aborts && nodes.nodes[scope.slot].state==LOOM_SERVICE_RETIRED,"actual driver abort retires");
 if(mode==5)CHECK(p->failure_phase==P9_HS_ATTACH_RECV,"protocol failure records attempted phase");
 CHECK(records[0].phase==LREQ_TERMINAL && records[0].result==result && !records[0].borrows,"immutable terminal result");
 if(mode==1 || mode==3 || mode==6) CHECK(!steps,"preexisting abort avoids client progress");
 ring.cq[0].user_data=999;
 CHECK(loom_private_post_terminal(&l,p->request)==-T_E_AGAIN && ring.cq[0].user_data==999 &&
       records[0].phase==LREQ_TERMINAL && loom_private_connect_release(p)==-T_E_BUSY,"CQ full retains protocol and terminal");
 ring.hdr.cq_head=1;
 CHECK(!loom_private_post_terminal(&l,p->request) && ring.cq[0].result==result && ring.cq[0].user_data==77,"exact CQ delivery");
 CHECK(loom_private_connect_reap(&l,scope)==-T_E_BUSY,"protocol pin blocks metadata reap");
 CHECK(!loom_private_connect_release(p) && releases==1 && !l.service_requests.occupied,"retired release");
 CHECK(!loom_private_connect_reap(&l,scope) && loom_private_connect_reap(&l,scope)==-T_E_NOENT,"scope reaps once");
 CHECK(!held,"balanced lock");puts("PASS CONNECT control");return 0;
}
'''
MUTANTS=[
 ('omit-failure-phase','p->failure_phase=attempted_phase;','(void)attempted_phase;',5,'protocol failure records attempted phase'),
 ('omit-duplicate','if (r->phase != LREQ_FREE && connect_same(r->scope,scope))','if (r->phase != LREQ_FREE && connect_same(r->scope,scope) && false)',0,'terminal reserved before allocation'),
 ('omit-own-pin','rc = loom_slot_pin(l->service_slots,scope,LOOM_SERVICE_SCOPE);','rc = 0;',0,'two exact scope pins before allocation'),
 ('omit-publish-lock','if (!rc) rc = devsrv_connect_publish(&p->admission);','if (!rc) { spin_unlock(&l->lock); rc = devsrv_connect_publish(&p->admission); spin_lock(&l->lock); }',0,'publication holds terminal reservation and lock'),
 ('omit-publish-deadline','l->service_nodes,p->request,now);','l->service_nodes,p->request,0);',7,'CONNECT publication lost terminal reservation'),
 ('early-ready','if (phase == P9_HS_READY)','if (phase >= P9_HS_ATTACH_RECV)',0,'IO progress is not READY'),
 ('persistent-deadline','if (r->phase == LREQ_ACTIVE && p->connect_deadline && p->connect_deadline <= now)','if (p->connect_deadline && p->connect_deadline <= now)',0,'deadline ends on success'),
 ('omit-parser-borrow','borrowed=true;','borrowed=true; (void)loom_request_put(&l->service_requests,p->request);',0,'driver owns request borrow outside lock'),
 ('omit-local-abort','    p9_client_progress_abort(&p->client);','    (void)p9_client_progress_abort; /* omitted cursor retirement */',1,'actual driver abort retires'),
 ('ignore-live-user','if (!p->users) { p->locally_retired=true;','if (true) { p->locally_retired=true;',6,'live abort user delays local retirement'),
 ('omit-delivery-gate','r->phase != LREQ_DELIVERED || r->borrows','r->phase == LREQ_FREE || r->borrows',0,'CONNECT lost retired ownership'),
 ('omit-close-recheck','    if (l->service_closing) connect_latch(p,n,-T_E_CANCELED);','    /* ignore close */',4,'actual driver abort retires'),
]
def main():
 os.chdir(ROOT)
 assert re.search(r'^mac +HELD by you',subprocess.check_output(['work/oct5-as-r9/astra-yip.sh','resources'],text=True),re.M)
 st=os.statvfs('.');assert st.f_bavail*st.f_frsize>=8*1024**3
 out=Path('work/oct5-as-r9/astra-takeover')/('connect-control-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));out.mkdir()
 paths=['kernel/loom_service_connect.c','kernel/loom_service_requests.c','kernel/loom_service_slots.c','kernel/loom_service_nodes.c','kernel/loom.c',str(Path(__file__).relative_to(ROOT))]
 hashes={p:hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in paths}
 body=re.sub(r'^#include[^\n]*\n','',Path(paths[0]).read_text(),flags=re.M)
 cq=post.extract.fun(Path('kernel/loom.c').read_text(),'loom_private_post_terminal')
 sdk=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip();results=[]
 for tag,old,new,mode,want in [('control',None,None,None,None)]+MUTANTS:
  s=body
  if old:
   assert s.count(old)==(2 if tag == 'omit-close-recheck' else 1),(tag,s.count(old));s=s.replace(old,new)
  src=out/(tag+'.c');src.write_text(PRE+s+cq+MAIN);exe=out/tag
  (out/(tag+'.diff')).write_text(''.join(difflib.unified_diff(body.splitlines(True),s.splitlines(True))))
  cmd=['/opt/homebrew/opt/llvm@22/bin/clang','-std=c11','-O1','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-isysroot',sdk,'-I',str(ROOT/'kernel/include'),str(src),*paths[1:4],'-o',str(exe)]
  (out/(tag+'-command.json')).write_text(json.dumps(cmd)+'\n')
  with (out/(tag+'-build.log')).open('wb') as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
  for case in range(11) if mode is None else [mode]:
   r=subprocess.run([str(exe.resolve()),str(case)],capture_output=True,text=True,timeout=15,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1','ASAN_OPTIONS':'detect_leaks=0'})
   text=r.stdout+r.stderr;(out/(tag+'-'+str(case)+'.log')).write_text(text)
   expected=want or 'PASS CONNECT control';rc=11 if want else 0
   assert r.returncode==rc and text==expected+'\n',(tag,case,r.returncode,text)
   results.append(dict(case=tag,mode=case,rc=rc,expected=expected,generated_sha256=hashlib.sha256(src.read_bytes()).hexdigest()))
  print('PASS',tag,flush=True)
 assert hashes=={p:hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in paths}
 (out/'result.json').write_text(json.dumps({'scope':__doc__,'source_head':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),'hashes':hashes,'results':results},indent=2)+'\n');print(out)
if __name__=='__main__':main()
