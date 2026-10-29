from pathlib import Path
import re, subprocess, json, hashlib, datetime
R=Path('/Users/northkillpd/projects/thylacine-corona');D=Path(__file__).resolve().parent
resources=subprocess.check_output([str(R/'work/oct5-as-r9/astra-yip.sh'),'resources'],text=True)
assert re.search(r'^mac +HELD by you',resources,re.M),resources
prior=(R/'work/oct5-as-r9/astra-takeover/engine-draft/control.py').read_text().split("out=D/('controls-'")[0]
e={'__file__':str(D/'control.py')};exec(prior,e)
extract=e['c'].post.extract.fun
pre=e['pre'].replace('u32 service_fixed_count;', 'u32 service_fixed_count,service_fixed_settling;bool sqpoll_stopping,sqpoll_exited;void *sqpoll;')
poll=(R/'kernel/include/thylacine/poll.h').read_text()
waiter=poll[poll.index('struct poll_waiter {'):poll.index('#define POLL_WAITER_MAGIC')].replace('struct Rendez','int')
pre=pre.replace('#include <thylacine/proc.h>','struct ProcSqpollTicket { u64 stripes; };')
pre=pre.replace('#include <thylacine/poll.h>',waiter+'\n#define POLLIN 1\n#define POLLOUT 4\n#define POLLERR 8\n#define POLLHUP 16\n')
pre=pre.replace(extract(pre,'loom_private_close_changed'),'''static void loom_private_close_changed(struct Loom *l) {
 CHECK(!held,"change wake outside Ll");__atomic_fetch_add(&l->service_engine->change,1,__ATOMIC_RELEASE);
}''')
pre=pre.replace('CHECK(!held && conn==&cn,"teardown outside lock");','CHECK(!held,"teardown outside lock");')
pre=pre.replace('static void wakeup(int *r) {(void)r;CHECK(!held,"owner change wakes outside lock");}','')
# Exact cursor/native operations remain deterministic doubles; real CONNECT,
# index, readiness, request, slot and node implementation is compiled below.
pre+=r'''
static struct Loom *test_ring;
static short native_ready[64];
static int race,linked,unlinked;
static u64 held_epoch;
static void poll_waiter_init(struct poll_waiter *pw,int *r) { *pw=(struct poll_waiter){.magic=0x504f4c57,.rendez=r}; }
static short srvconn_poll(struct SrvConn *c,bool client,short events,struct poll_waiter *pw) {
 CHECK(!held && client,"native poll outside Ll");
 u32 slot=64;
 for(u32 i=0;i<64;++i)if(test_ring->service_engine->entries[i].protocol && test_ring->service_engine->entries[i].protocol->admission.conn==c){slot=i;break;}
 CHECK(slot<64 && test_ring->service_engine->entries[slot].pins,"poll retains native owner");
 if(!pw && race==3) {
  race=0;
  struct loom_service_protocol *p=test_ring->service_engine->entries[slot].protocol;
  CHECK(!loom_engine_abort(test_ring,p->scope,-T_E_CANCELED),"race abort");
  (void)loom_private_connect_step(p,3);(void)loom_private_post_terminal(test_ring,p->request);
  u64 deadline=0;CHECK(!loom_engine_wait_arm(test_ring,3,&deadline),"collector parks behind transient ready pin");
  held_epoch=test_ring->service_engine->change;
 }
 if(!pw && race==4) {
  race=0;close_owner(test_ring);loom_private_close_changed(test_ring);
  CHECK(loom_engine_close_visit(test_ring)==0,"close parks behind transient ready pin");
  held_epoch=test_ring->service_engine->change;
 }
 if(pw) {
  CHECK(!pw->list,"register exactly once");pw->list=(void *)c;linked++;
  if(race==1){pw->ready=true;race=0;} // ready after level sample, before sleep
  if(race==2){loom_private_close_changed(test_ring);race=0;}
 }
 return native_ready[slot] & (events|POLLERR|POLLHUP);
}
static void poll_waiter_list_unregister(struct poll_waiter *pw) {
 CHECK(!held,"unhook outside Ll");
 if(!pw->list)return;
 for(u32 i=0;i<64;++i) if(test_ring->service_engine->wait_mask&(1ull<<i))
  CHECK(test_ring->service_engine->entries[i].pins,"all pins retained through whole unhook sweep");
 pw->list=NULL;unlinked++;
}
'''
# Add hook-before-final-storage boundary proof to the native storage double.
pre=pre.replace('releases++;free(p);active=NULL;return 0;', 'for(u32 j=0;j<64;++j) CHECK(!p->owner->service_engine->waiters[j].list,"no hook at physical storage free");releases++;free(p);active=NULL;return 0;')
setup=e['main'][e['main'].index(' struct loom_slot_table slots='):e['main'].index(' struct loom_service_ref scope,second=')]
destination=e['main'][:e['main'].index('int main(')]
main=destination+r'''
int main(int argc,char **argv) {
 int which=argc>1?atoi(argv[1]):0;mode=0;
SETUP
 test_ring=&l;
 struct loom_service_ref scope,second={0},target={.slot=9,.incarnation=5};destination(&l,&scope);
 struct loom_service_protocol *p=NULL,*q=NULL;
 CHECK(!loom_private_connect_prepare(&l,scope,target,77,100,1,&p) && !loom_engine_adopt(p,2),"adopt control");
 u64 deadline=999;
 if(which==0 || which==2 || which==3) {
  CHECK(!loom_engine_ready_visit(&l,3) && !steps,"stalled owner does not spin");
  race=which==2?1:which==3?2:0;
  CHECK(loom_engine_wait_arm(&l,3,&deadline)==(which==3?1:0) && deadline==100 && linked==1,"register then sample and absolute deadline");
  CHECK(loom_engine_wait_changed(&l)==(which?1:0),"registration race remains observable");
  CHECK(loom_engine_wait_arm(&l,3,&deadline)==-T_E_BUSY && linked==1,"one wait owner");
  CHECK(index.entries[scope.slot].pins==1 && !loom_engine_empty(&index),"wait retains entry lifetime");
  loom_engine_wait_disarm(&l);CHECK(unlinked==linked && !index.entries[scope.slot].pins,"disarm exact pins");
 }
 if(which==1) {
  native_ready[scope.slot]=POLLOUT;
  CHECK(loom_engine_ready_visit(&l,3)==1 && steps==1,"already ready runs one quantum");
  CHECK(!loom_engine_ready_visit(&l,4) && steps==1,"receive phase ignores write readiness");
  CHECK(!loom_engine_wait_arm(&l,4,&deadline) && deadline==100,"receive interest parks");
  native_ready[scope.slot]=POLLIN;index.waiters[scope.slot].ready=true;
  CHECK(loom_engine_wait_changed(&l),"native delayed response wakes");loom_engine_wait_disarm(&l);
  CHECK(loom_engine_ready_visit(&l,5)==1 && steps==2 && records[p->request.index].phase==LREQ_DELIVERED,"ready commits and delivers");
  CHECK(!loom_engine_ready_visit(&l,101) && !loom_engine_wait_arm(&l,101,&deadline) && !deadline && linked==unlinked,"READY has no timer or native wait");
  loom_engine_wait_disarm(&l);
 }
 if(which==4) {
  CHECK(!loom_engine_wait_arm(&l,3,&deadline),"park before close");
  close_owner(&l);loom_private_close_changed(&l);
  CHECK(loom_engine_wait_changed(&l) && loom_engine_close_visit(&l)==0 && !l.service_requests.detached,"close wakes but pins block detach");
  loom_engine_wait_disarm(&l);
 }
 if(which==5) {
  CHECK(!loom_engine_wait_arm(&l,3,&deadline) && deadline==100,"absolute timeout armed without readiness");loom_engine_wait_disarm(&l);
  l.cq_tail=2;
  CHECK(loom_engine_ready_visit(&l,100)==1 && !steps && p->locally_retired && records[p->request.index].result==-T_E_TIMEDOUT,"deadline progresses without native event or CQ room");
  CHECK(!loom_engine_ready_visit(&l,101),"full CQ retirement does not spin");
 }
 if(which==6) {
  l.cq_tail=2;native_ready[scope.slot]=POLLOUT;
  CHECK(loom_engine_ready_visit(&l,3)==1,"first success step");native_ready[scope.slot]=POLLIN;
  CHECK(loom_engine_ready_visit(&l,4)==1 && records[p->request.index].phase==LREQ_TERMINAL,"full CQ preserves committed success");
  CHECK(!loom_engine_ready_visit(&l,101) && !loom_engine_wait_arm(&l,101,&deadline) && !deadline,"backpressure has no spin or old timer");
  ring.hdr.cq_head=1;
  CHECK(loom_engine_wait_changed(&l),"CQ drain changes wait predicate");loom_engine_wait_disarm(&l);
  CHECK(loom_engine_ready_visit(&l,102)==1 && records[p->request.index].phase==LREQ_DELIVERED && steps==2,"drain posts without duplicate protocol step");
 }
 if(which==7 || which==10) {
  if(which==7)p->driving=true;else p->users=1;
  native_ready[scope.slot]=POLLOUT;
  CHECK(!loom_engine_ready_visit(&l,101) && !loom_engine_wait_arm(&l,101,&deadline) && !deadline && !linked,"busy local holder parks instead of deadline spin");
  p->driving=false;p->users=0;loom_private_close_changed(&l);
  CHECK(loom_engine_wait_changed(&l),"holder return invalidates wait snapshot");loom_engine_wait_disarm(&l);
 }
 if(which==8 || which==9) {
  static struct SrvConn other;
  destination(&l,&second);CHECK(!loom_private_connect_prepare(&l,second,target,88,90,1,&q),"second prepare");q->admission.conn=&other;
  CHECK(!loom_engine_adopt(q,2),"second adopt");
  if(which==8) {
   native_ready[second.slot]=POLLOUT;
   CHECK(loom_engine_ready_visit(&l,3)==1 && !p->client.dummy && q->client.dummy==1,"stalled first owner cannot starve ready second");
   native_ready[second.slot]=POLLIN;
   CHECK(loom_engine_ready_visit(&l,4)==1 && !p->client.dummy && q->client.dummy==2,"round robin second quantum");
  }else{
   CHECK(!loom_engine_wait_arm(&l,3,&deadline) && deadline==90 && linked==2,"nearest deadline across all armed owners");
   close_owner(&l);loom_private_close_changed(&l);
   CHECK(loom_engine_close_visit(&l)==1 && loom_engine_close_visit(&l)==0 && !releases,"wait sweep retains both retired owners");
   loom_engine_wait_disarm(&l);CHECK(unlinked==2,"whole unhook sweep");
  }
 }
 if(which==12 || which==13) {
  race=which==12?3:4;
  CHECK(!loom_engine_ready_visit(&l,3),"transient scan has no direct ready action");
  CHECK(index.change>held_epoch,"transient pin return wakes dependent collector or close");
  if(which==12){CHECK(loom_engine_wait_changed(&l),"collector wait invalidated by final external pin return");loom_engine_wait_disarm(&l);}
 }
 if(which==11) {
  native_ready[scope.slot]=POLLHUP;
  CHECK(loom_engine_ready_visit(&l,3)==1 && steps==1,"hangup prompts nonblocking driver independent of requested direction");
 }
 close_owner(&l);loom_private_close_changed(&l);
 for(int i=0;i<5;++i) (void)loom_engine_close_visit(&l);
 CHECK(loom_engine_empty(&index) && allocations==releases && linked==unlinked && !held,"all owners and wait hooks settle");
 CHECK(sizeof(index)<=4096,"wait storage stays within charged page");
 puts("PASS readiness control");return 0;
}
'''.replace('SETUP',setup)
body=lambda p:re.sub(r'^#include[^\n]*\n','',p.read_text(),flags=re.M)
engine=body(D/'kernel/loom_service_engine.c')+(D/'readiness.inc').read_text()
connect=body(R/'kernel/loom_service_connect.c')
post=extract((R/'kernel/loom.c').read_text(),'loom_private_post_terminal')
mutants=[
 ('omit-pin-return-wake','if (dependent) engine_changed(l);','(void)dependent;',12,'transient pin return wakes dependent collector or close'),
 ('omit-close-return-wake','l->service_closing || l->service_engine->wait_active','l->service_engine->wait_active',13,'transient pin return wakes dependent collector or close'),
 ('omit-change','t->wait_change!=__atomic_load_n(&t->change,__ATOMIC_ACQUIRE)','false',3,'register then sample and absolute deadline'),
 ('wrong-nearest-deadline','a.deadline<t->wait_deadline','a.deadline>t->wait_deadline',9,'nearest deadline across all armed owners'),
 ('ignore-native-ready','a.events && srvconn_poll(p->admission.conn,true,a.events,NULL)','a.events',0,'stalled owner does not spin'),
 ('wrong-direction','case P9_HS_ATTACH_RECV: a.events=POLLIN','case P9_HS_ATTACH_RECV: a.events=POLLOUT',1,'receive phase ignores write readiness'),
 ('omit-register','srvconn_poll(p->admission.conn,true,a.events,pw)','srvconn_poll(p->admission.conn,true,a.events,NULL)',0,'register then sample and absolute deadline'),
 ('omit-flag','&& t->waiters[i].ready','&& false',2,'registration race remains observable'),
 ('omit-cq-drain','t->wait_cq_head!=__atomic_load_n(&h->cq_head,__ATOMIC_ACQUIRE)','false',6,'CQ drain changes wait predicate'),
 ('ignore-busy','if (p->driving || p->users || p->locally_retired) return a;','if (p->locally_retired) return a;',7,'busy local holder parks instead of deadline spin'),
 ('ignore-teardown-user','if (p->driving || p->users || p->locally_retired) return a;','if (p->driving || p->locally_retired) return a;',10,'busy local holder parks instead of deadline spin'),
 ('drop-deadline','a.deadline=p->connect_deadline;','a.deadline=0;',5,'absolute timeout armed without readiness'),
 ('reset-deadline','a.deadline=p->connect_deadline;','a.deadline=now+p->connect_deadline;',5,'absolute timeout armed without readiness'),
 ('skip-timeout','p->connect_deadline<=now','false',5,'deadline progresses without native event or CQ room'),
 ('ignore-cq-room','(u32)(l->cq_tail-head)<l->cq_entries','true',6,'backpressure has no spin or old timer'),
 ('skip-unhook','poll_waiter_list_unregister(&t->waiters[i]);','(void)i;',0,'disarm exact pins'),
 ('allow-second-wait','if (t->wait_active) {','if (false) {',0,'one wait owner'),
]
out=D/('controls-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));out.mkdir();results=[]
for tag,old,new,case,want in [('control',None,None,None,None)]+mutants:
 code=engine
 if old: assert code.count(old)==(2 if tag=='omit-change' else 1),(tag,code.count(old));code=code.replace(old,new)
 src=out/(tag+'.c');src.write_text(pre+connect+code+post+main)
 exe=out/tag
 cmd=['/opt/homebrew/opt/llvm@22/bin/clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-function','-Wno-unused-variable','-Wno-unused-but-set-variable','-fsanitize=address,undefined','-isysroot',subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip(),'-I',str(R/'kernel/include'),str(src),str(R/'kernel/loom_service_requests.c'),str(R/'kernel/loom_service_slots.c'),str(R/'kernel/loom_service_nodes.c'),'-o',str(exe)]
 (out/(tag+'-command.json')).write_text(json.dumps(cmd)+'\n')
 with (out/(tag+'-build.log')).open('wb') as f:b=subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT)
 assert b.returncode==0,(tag,'build',out)
 for n in range(14) if case is None else [case]:
  run=subprocess.run([str(exe),str(n)],capture_output=True,text=True,timeout=10);output=run.stdout+run.stderr;(out/(tag+'-'+str(n)+'.log')).write_text(output)
  assert run.returncode==(11 if want else 0) and run.stdout==(want or 'PASS readiness control')+'\n' and not run.stderr,(tag,n,run.returncode,output,out)
  results.append(dict(tag=tag,case=n,rc=run.returncode,source_sha256=hashlib.sha256(src.read_bytes()).hexdigest(),log_sha256=hashlib.sha256(output.encode()).hexdigest()))
 print('PASS',tag,flush=True)
(out/'result.json').write_text(json.dumps({'results':results,'inputs':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in [D/'kernel/loom_service_engine.c',D/'kernel/include/thylacine/loom_service_engine.h',D/'readiness.inc',R/'kernel/loom_service_connect.c',Path(__file__)]},'limits':'Actual draft readiness/index and canonical CONNECT/ledgers; sequential checked native/poll/scheduler doubles. Not native or SMP qualification; worker start/join not yet implemented.'},indent=2)+'\n');print(out)
