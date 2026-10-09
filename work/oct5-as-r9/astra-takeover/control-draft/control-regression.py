from pathlib import Path
import json,re,subprocess,hashlib,datetime,os
R=Path('/Users/northkillpd/projects/thylacine-corona');D=Path(__file__).resolve().parent
source=(R/'work/oct5-as-r9/astra-takeover/rollback-draft/engine-regression.py').read_text().split("out=Path(")[0]
e={'__file__':str(R/'work/oct5-as-r9/astra-takeover/rollback-draft/engine-regression.py')};exec(source,e)
pre=e['pre'];ex=e['e']['c'].post.extract
pre=pre.replace('struct Loom {','struct Loom {u64 service_creator;')
pre=pre.replace('struct {struct {u32 phase;} handshake;} progress;','struct {struct {u32 phase;} handshake;struct {bool bytes_sent;} io;} progress;')
pre=pre.replace('static struct loom_service_protocol *active,', '''static void progress_boundary(struct loom_service_protocol *);
static struct loom_service_protocol *active,''')
pre=pre.replace('CHECK(held && active->indexed,"native publication has indexed lifetime");','CHECK(held && (active->indexed || mode==101),"native publication has indexed lifetime");')
pre=pre.replace('CHECK(e->phase==LENGINE_INSTALLING && e->pins==1,"installation pin covers native publication");','CHECK(mode==101 || (e->phase==LENGINE_INSTALLING && e->pins==1),"installation pin covers native publication");')
pre=pre.replace(' CHECK(loom_engine_get(l,active->scope,&p)==-T_E_AGAIN &&', ' CHECK(loom_engine_get(l,active->scope,&p)==-T_E_AGAIN &&')
fn=ex.fun(pre,'devsrv_connect_wake');pre=pre.replace(fn,fn.replace(' struct Loom *l=', ' if(mode==101)return;\n struct Loom *l='))
fn=ex.fun(pre,'p9_client_progress_step');fn=fn.replace(' steps++;',' steps++;active->progress.io.bytes_sent=true;\n progress_boundary(active);');fn=fn.replace('active->client.dummy==1?','mode!=101 && active->client.dummy==1?');pre=pre.replace(ex.fun(pre,'p9_client_progress_step'),fn)
pre=pre.replace('static struct loom_service_protocol *active,', 'static u32 expected_child_pins;\nstatic struct loom_service_protocol *active,')
pre=pre.replace('!p->owner->service_slots->slots[p->scope.slot].pins,"indexed lifetime ends after pins and terminal"','p->owner->service_slots->slots[p->scope.slot].pins==expected_child_pins,"indexed lifetime ends after pins and terminal"')
pre=pre.replace('static void progress_boundary(struct loom_service_protocol *);','static void progress_boundary(struct loom_service_protocol *);\nstatic void destruction_boundary(struct loom_service_protocol *);\nstatic void installation_boundary(struct loom_service_protocol *);')
fn=ex.fun(pre,'loom_private_protocol_release_unused');pre=pre.replace(fn,fn.replace(' releases++;free(p);',' destruction_boundary(p);\n releases++;free(p);'))
fn=ex.fun(pre,'p9_client_progress_step');pre=pre.replace(fn,fn.replace(' ++active->client.dummy;', ' if(mode==104)return -T_E_INVAL;\n ++active->client.dummy;'))
pre=pre.replace(' if(mode==1)CHECK(!loom_engine_abort', ' if(mode==105)installation_boundary(active);\n if(mode==1)CHECK(!loom_engine_abort')
pre+='''
#include <string.h>
#define PROC_MAGIC 456
struct Proc {u32 magic;void *as;u64 stripes;};
static struct Proc creator;
static int dispatch_kind;
static int loom_private_target_reap(struct Loom *l,struct loom_service_ref r) {(void)l;(void)r;CHECK(!held,"target drop outside Ll");dispatch_kind=LOOM_SERVICE_TARGET;return 0;}
static int loom_private_pool_reap(struct Loom *l,struct loom_service_ref r) {(void)l;(void)r;CHECK(!held,"pool drop outside Ll");dispatch_kind=LOOM_SERVICE_POOL;return 0;}
'''
body=lambda p:re.sub(r'^#include[^\n]*\n','',p.read_text(),flags=re.M)
connect=body(D/'kernel/loom_service_connect.c');engine=body(R/'kernel/loom_service_engine.c');control=body(D/'kernel/loom_service_control.c')
post=e['post'];nodewrap=ex.fun((R/'kernel/loom.c').read_text(),'loom_private_node_reap_unused');dest=ex.fun(e['e']['main'],'destination')
main=r'''
static struct loom_service_snapshot query(struct Loom *l,struct loom_service_ref ref) {
 struct loom_service_snapshot in={.size=sizeof(in),.version=1,.object=ref},out={0};
 CHECK(!loom_private_service_query(l,&creator,&in,&out),"exact query succeeds");return out;
}
static struct loom_service_control input(struct loom_service_ref ref) {
 return (struct loom_service_control){.size=sizeof(struct loom_service_control),.version=1,.object=ref};
}
static void destruction_boundary(struct loom_service_protocol *p) {
 if(mode!=103)return;
 struct loom_service_snapshot s=query(p->owner,p->scope);
 struct loom_service_control c=input(p->scope);
 CHECK(s.state==LOOM_SERVICE_ABORTING && s.active_ops==1 && !(s.status_flags&LOOM_SERVICE_LOCAL_RETIRED) &&
       p->owner->service_slots->slots[p->scope.slot].pins==2 && loom_private_service_reap(p->owner,&creator,&c)==-T_E_BUSY,
       "destruction boundary retains observable ownership");
}
static void installation_boundary(struct loom_service_protocol *p) {
 struct loom_service_snapshot s=query(p->owner,p->scope);
 CHECK(s.state==LOOM_SERVICE_RESERVED && s.active_ops==1 && !s.status_flags,"INSTALLING query stays reserved before publication");
 struct loom_service_control c=input(p->scope);
 CHECK(!loom_private_service_abort(p->owner,&creator,&c),"INSTALLING abort accepted");
}
static void progress_boundary(struct loom_service_protocol *p) {
 if(mode!=101)return;
 struct loom_service_snapshot s=query(p->owner,p->scope);
 CHECK(s.active_ops==1 && (s.status_flags&LOOM_SERVICE_BYTES_SENT),"unindexed in-flight driver reports possible bytes");
 struct loom_service_control c=input(p->scope);
 CHECK(!loom_private_service_abort(p->owner,&creator,&c),"raw abort while driver outside Ll");
}
int main(int argc,char **argv) {
 int which=argc>1?atoi(argv[1]):0;mode=0;
SETUP
 l.service_creator=77;creator=(struct Proc){.magic=PROC_MAGIC,.as=l.service_as,.stripes=77};
 struct loom_service_ref scope,target={.slot=9,.incarnation=5};destination(&l,&scope);
 struct loom_service_control c=input(scope);struct loom_service_snapshot s=query(&l,scope);
 struct loom_service_protocol *p=NULL,*pin=NULL;
 CHECK(s.kind==LOOM_SERVICE_SCOPE && s.state==LOOM_SERVICE_RESERVED && !s.active_ops &&
       !s.pending_terminals && !s.reason && !s.status_flags && control_same(s.scope,scope),"unused snapshot exact and empty");
 if(which==0) {
  struct Proc stranger=creator;stranger.stripes++;
  struct loom_service_snapshot in={.size=sizeof(in),.version=1,.object=scope},out,before;
  memset(&out,0xa5,sizeof(out));before=out;
  CHECK(loom_private_service_query(&l,&stranger,&in,&out)==-T_E_ACCES && !memcmp(&out,&before,sizeof(out)),"foreign creator refuses untouched output");
  stranger=creator;stranger.as=&stranger;
  CHECK(loom_private_service_abort(&l,&stranger,&c)==-T_E_ACCES,"foreign image denied");
  in.kind=1;CHECK(loom_private_service_query(&l,&creator,&in,&out)==-T_E_INVAL && !memcmp(&out,&before,sizeof(out)),"output fields rejected without write");in.kind=0;
  in.object.incarnation++;CHECK(loom_private_service_query(&l,&creator,&in,&out)==-T_E_NOENT && !memcmp(&out,&before,sizeof(out)),"stale query leaves output unchanged");
  c.flags=1;CHECK(loom_private_service_abort(&l,&creator,&c)==-T_E_INVAL,"unknown flags denied");c.flags=0;
  CHECK(!loom_private_service_reap(&l,&creator,&c) && loom_private_service_reap(&l,&creator,&c)==-T_E_NOENT,"unused reap and stale reuse");
 } else if(which==1) {
  CHECK(!loom_private_service_abort(&l,&creator,&c),"unused abort accepted");s=query(&l,scope);
  CHECK(s.state==LOOM_SERVICE_RETIRED && s.reason==-T_E_CANCELED && s.status_flags==LOOM_SERVICE_LOCAL_RETIRED,"unused abort reports completed local retirement");
  CHECK(loom_private_service_abort(&l,&creator,&c)==1 && !loom_private_service_reap(&l,&creator,&c),"unused abort idempotence and reap");
 } else if(which==2) {
  mode=103;
  CHECK(!loom_private_connect_prepare(&l,scope,target,77,100,1,&p),"raw prepare");s=query(&l,scope);
  CHECK(s.active_ops==1 && !s.pending_terminals,"PREPARED owner counted");
  CHECK(!loom_private_service_abort(&l,&creator,&c) && loom_private_connect_publish(p,2)==-T_E_CANCELED,"abort denies raw publication");
  s=query(&l,scope);CHECK(s.state==LOOM_SERVICE_ABORTING && !(s.status_flags&LOOM_SERVICE_LOCAL_RETIRED) && loom_private_service_reap(&l,&creator,&c)==-T_E_BUSY,"constructing abort does not claim retirement");
  CHECK(!loom_private_connect_rollback(p),"raw aborted constructor rolls back");s=query(&l,scope);
  CHECK(s.state==LOOM_SERVICE_RETIRED && !s.active_ops && s.reason==-T_E_CANCELED && s.status_flags==LOOM_SERVICE_LOCAL_RETIRED,"rollback retains diagnostic after physical cleanup");
  CHECK(!loom_private_service_reap(&l,&creator,&c),"aborted constructor reap");
 } else if(which==3 || which==5) {
  CHECK(!loom_private_connect_prepare(&l,scope,target,77,100,1,&p) && !loom_engine_adopt(p,2),"indexed preparation");
  l.cq_tail=2;CHECK(loom_engine_visit(&l,3)==1 && loom_engine_visit(&l,4)==1,"drive to READY behind full CQ");s=query(&l,scope);
  CHECK(s.state==LOOM_SERVICE_READY && !s.active_ops && s.pending_terminals==1 && s.status_flags==LOOM_SERVICE_BYTES_SENT,"READY has retained terminal and sticky bytes");
  struct loom_service_ref child={0};
  if(which==5)expected_child_pins=1;
  if(which==5)CHECK(!loom_service_node_prepare(&slots,&nodes,LOOM_SERVICE_FID,scope,&child) && !loom_service_node_publish(&slots,&nodes,child),"child pins READY parent");
  CHECK(!loom_engine_get(&l,scope,&pin) && !loom_private_service_abort(&l,&creator,&c),"abort indexed READY owner");
  CHECK(loom_engine_visit(&l,5)==1,"actual cursor abort");s=query(&l,scope);
  CHECK(s.state==LOOM_SERVICE_RETIRED && s.reason==-T_E_CANCELED && s.pending_terminals==1 && s.status_flags==3 && records[p->request.index].result==(s32)scope.slot,"abort retains prior success and diagnostics");
  CHECK(loom_private_service_reap(&l,&creator,&c)==-T_E_BUSY,"retired owner pin blocks reap");
  ring.hdr.cq_head=1;CHECK(!loom_private_post_terminal(&l,p->request),"post retained terminal");s=query(&l,scope);
  CHECK(!s.active_ops && !s.pending_terminals && s.reason==-T_E_CANCELED,"DELIVERED is storage not active obligation");
  CHECK(!loom_engine_put(&l,scope,pin) && !loom_engine_collect(&l,scope),"return pin and collect");s=query(&l,scope);
  CHECK(s.state==LOOM_SERVICE_RETIRED && s.status_flags==3 && s.reason==-T_E_CANCELED,"collected owner leaves stable diagnostic");
  if(which==5) {
   CHECK(loom_private_service_reap(&l,&creator,&c)==-T_E_BUSY,"child pin prevents parent reap");
   struct loom_service_snapshot f=query(&l,child);CHECK(f.kind==LOOM_SERVICE_FID && control_same(f.scope,scope),"FID query reports exact parent");
   struct loom_service_control fc=input(child);CHECK(!loom_private_service_reap(&l,&creator,&fc),"unused child reap after abort");
  }
  CHECK(!loom_private_service_reap(&l,&creator,&c),"collected retired scope reap");
 } else if(which==4) {
  mode=101;CHECK(!loom_private_connect_prepare(&l,scope,target,77,100,1,&p) && !loom_private_connect_publish(p,2),"raw published owner");
  CHECK(loom_private_connect_step(p,3)==-T_E_CANCELED && records[p->request.index].result==-T_E_CANCELED,"raw metadata abort wins before READY commit");
  s=query(&l,scope);CHECK(s.state==LOOM_SERVICE_RETIRED && s.reason==-T_E_CANCELED && s.status_flags==3,"raw driver publishes sticky diagnostics");
  CHECK(!loom_private_post_terminal(&l,p->request) && !loom_private_connect_release(p) && !loom_private_service_reap(&l,&creator,&c),"raw cancelled owner fully reaps");
 } else if(which==7 || which==8) {
  mode=which==7?104:105;
  CHECK(!loom_private_connect_prepare(&l,scope,target,77,100,1,&p),"error/installation prepare");
  if(which==8)hook_armed=1;
  CHECK(!loom_engine_adopt(p,2),"error/installation adoption");
  if(which==7)p->progress.handshake.phase=P9_HS_ATTACH_SEND;
  l.cq_tail=2;CHECK(loom_engine_visit(&l,3)==1,"error/installation retirement visit");s=query(&l,scope);
  CHECK(s.state==LOOM_SERVICE_RETIRED && s.reason==(which==7?-T_E_IO:-T_E_CANCELED) && s.pending_terminals==1,"error/installation diagnostics");
  if(which==7)CHECK(nodes.nodes[scope.slot].failure_phase==P9_HS_ATTACH_SEND,"retained actual failure phase");
  ring.hdr.cq_head=1;CHECK(!loom_private_post_terminal(&l,p->request) && !loom_engine_collect(&l,scope),"error/installation collect");
  if(which==7)CHECK(nodes.nodes[scope.slot].failure_phase==P9_HS_ATTACH_SEND,"failure phase survives collection");
  CHECK(!loom_private_service_reap(&l,&creator,&c),"error/installation reap");
 } else if(which==6) {
  struct loom_service_ref ref={0};CHECK(!loom_slot_reserve(&slots,LOOM_SERVICE_TARGET,&ref) && !loom_slot_publish(&slots,ref),"target fixture");
  s=query(&l,ref);CHECK(s.kind==LOOM_SERVICE_TARGET && s.state==LOOM_SERVICE_READY && control_zero_ref(s.scope),"target query");
  c=input(ref);CHECK(loom_private_service_abort(&l,&creator,&c)==-T_E_INVAL && !loom_private_service_reap(&l,&creator,&c) && dispatch_kind==LOOM_SERVICE_TARGET,"target controls dispatch exact primitive");
  CHECK(!loom_slot_reserve(&slots,LOOM_SERVICE_POOL,&ref) && !loom_slot_publish(&slots,ref),"pool fixture");c=input(ref);
  struct loom_service_snapshot in={.size=sizeof(in),.version=1,.object=ref};
  CHECK(loom_private_service_query(&l,&creator,&in,&s)==-T_E_INVAL,"POOL uses dedicated query");
  l.receipt_size=0;CHECK(loom_private_service_reap(&l,&creator,&c)==-T_E_OPNOTSUPP,"pool mode required");l.receipt_size=32;
  CHECK(!loom_private_service_reap(&l,&creator,&c) && dispatch_kind==LOOM_SERVICE_POOL,"pool reap dispatch");
 }
 CHECK(!held && !temporary_roots,"control locks and temporary roots balance");puts("PASS service control");return 0;
}
'''.replace('SETUP',e['setup'])
out=D/('controls-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));out.mkdir();src=out/'control.c';src.write_text(pre+nodewrap+connect+engine+post+control+dest+main)
cmd=['/opt/homebrew/opt/llvm@22/bin/clang','-std=c11','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-but-set-variable','-g','-fsanitize=address,undefined','-isysroot',subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip(),'-I',str(D/'kernel/include'),'-I',str(R/'kernel/include'),str(src),str(R/'kernel/loom_service_requests.c'),str(R/'kernel/loom_service_slots.c'),str(D/'kernel/loom_service_nodes.c'),'-o',str(out/'control')]
(out/'command.json').write_text(json.dumps(cmd)+'\n');r=subprocess.run(cmd,capture_output=True,text=True);(out/'build.log').write_text(r.stdout+r.stderr);assert r.returncode==0,(out,r.stderr)
results=[]
for case in range(9):
 r=subprocess.run([str(out/'control'),str(case)],capture_output=True,text=True,timeout=15,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'});(out/(str(case)+'.log')).write_text(r.stdout+r.stderr)
 assert r.returncode==0 and r.stdout=='PASS service control\n' and not r.stderr,(out,case,r.returncode,r.stdout,r.stderr)
 results.append(case);print('PASS control',case,flush=True)
(out/'result.json').write_text(json.dumps({'cases':results,'source_sha256':hashlib.sha256(src.read_bytes()).hexdigest(),'limits':'Actual draft control+CONNECT, actual canonical engine/request/slot/CQ, draft node ledger; checked protocol/native/Proc/lock and target/pool-dispatch doubles. No native or activation qualification.'},indent=2)+'\n');print(out)
