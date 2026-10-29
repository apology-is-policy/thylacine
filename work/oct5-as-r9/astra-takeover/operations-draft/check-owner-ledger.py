from pathlib import Path
import json,datetime,subprocess,hashlib
R=Path('/Users/northkillpd/projects/thylacine-corona');D=Path(__file__).resolve().parent
out=D/('owner-controls-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));out.mkdir()
source=(D/'kernel/loom_service_requests.c').read_text()
harness=r'''
#include <stdio.h>
#include <string.h>
#include <assert.h>
static struct loom_slot_table slots;
static struct loom_service_node_table nodes;
static struct loom_service_request records[1];
static struct loom_request_table t;
static struct loom_service_ref scope;
static struct loom_request_ref ref;
#define CHECK(c,n) do{if(!(c)){fprintf(stderr,"%s\n",n);assert(0);}}while(0)
static void init(void) {
 memset(&slots,0,sizeof(slots));memset(&nodes,0,sizeof(nodes));memset(records,0,sizeof(records));
 t=(struct loom_request_table){.records=records,.capacity=1};
 CHECK(!loom_service_node_prepare(&slots,&nodes,LOOM_SERVICE_SCOPE,(struct loom_service_ref){0},&scope),"scope prepare");
 CHECK(!loom_service_node_publish(&slots,&nodes,scope),"scope publish");
 nodes.nodes[scope.slot].state=LOOM_SERVICE_READY;
 CHECK(!loom_request_prepare(&t,&slots,&nodes,scope,11,0,1,&ref),"request prepare");
}
int main(void) {
 init();CHECK(sizeof(records[0])==56,"charge geometry unchanged");
 CHECK(!loom_request_hold_owner(&t,ref)&&records[0].owners==1,"owner retained");
 CHECK(loom_request_hold_owner(&t,ref)==-T_E_BUSY,"one physical owner");
 CHECK(loom_request_rollback(&t,&slots,ref)==-T_E_BUSY,"rollback waits physical owner");
 CHECK(t.occupied==1&&slots.slots[scope.slot].pins==1,"rollback retains identity and scope");
 CHECK(!loom_request_drop_owner(&t,ref)&&records[0].owners==0,"owner dropped");
 CHECK(loom_request_drop_owner(&t,ref)==-T_E_BUSY,"double owner drop rejected");
 CHECK(!loom_request_rollback(&t,&slots,ref)&&t.occupied==0,"rollback after physical free");
 struct loom_request_ref old=ref;
 CHECK(!loom_request_prepare(&t,&slots,&nodes,scope,12,0,1,&ref)&&ref.identity>old.identity,"identity burned");
 CHECK(loom_request_hold_owner(&t,old)==-T_E_NOENT,"stale hold refused");
 CHECK(!loom_request_hold_owner(&t,ref),"new owner");
 CHECK(loom_request_drop_owner(&t,old)==-T_E_NOENT&&records[0].owners==1,"stale drop cannot release new owner");
 CHECK(!loom_request_admit(&t,&slots,&nodes,ref,1),"admit held owner");
 CHECK(loom_request_hold_owner(&t,ref)==-T_E_BUSY,"late attach refused");
 CHECK(loom_request_drop_owner(&t,ref)==-T_E_BUSY,"active owner cannot disappear");
 CHECK(!loom_request_borrow(&t,&slots,&nodes,ref),"parser borrow");
 CHECK(loom_request_drop_owner(&t,ref)==-T_E_BUSY,"borrow blocks owner drop");
 CHECK(!loom_request_put(&t,ref),"parser returns");
 CHECK(!loom_request_finish(&t,&slots,&nodes,ref,4),"commit while retaining physical owner");
 CHECK(!loom_request_deliver(&t,ref),"delivery independent from physical owner");
 CHECK(loom_request_reap(&t,&slots,ref)==-T_E_BUSY,"reap waits physical owner");
 CHECK(!loom_request_drop_owner(&t,ref),"destroy then drop");
 CHECK(!loom_request_reap(&t,&slots,ref),"reap after physical free");
 init();CHECK(!loom_request_hold_owner(&t,ref),"failed constructor owner");
 loom_slots_close(&slots);nodes.nodes[scope.slot].state=LOOM_SERVICE_ABORTING;
 CHECK(!loom_request_reject(&t,&slots,ref,-T_E_NOMEM),"failed admission terminal");
 CHECK(!loom_requests_detach(&t,&slots)&&!loom_request_discard(&t,ref),"consumer discard");
 CHECK(loom_request_reap(&t,&slots,ref)==-T_E_BUSY,"discard does not free physical owner");
 CHECK(!loom_request_drop_owner(&t,ref)&&!loom_request_reap(&t,&slots,ref),"close cleanup exact settlement");
 init();records[0].borrows=1;CHECK(loom_request_hold_owner(&t,ref)==-T_E_BUSY,"constructor cannot overlap borrowed record");
 records[0].borrows=0;CHECK(!loom_request_hold_owner(&t,ref),"attach after borrow ends");
 CHECK(!loom_request_reject(&t,&slots,ref,-T_E_CANCELED),"rejection");
 records[0].borrows=1;CHECK(loom_request_drop_owner(&t,ref)==-T_E_BUSY,"terminal borrow prevents physical release");
 records[0].borrows=0;CHECK(!loom_request_drop_owner(&t,ref),"terminal release");
 init();CHECK(!loom_request_admit(&t,&slots,&nodes,ref,1),"legacy no-owner admission");
 CHECK(loom_request_hold_owner(&t,ref)==-T_E_BUSY,"attach only before admission");
 CHECK(!loom_request_finish(&t,&slots,&nodes,ref,0)&&!loom_request_deliver(&t,ref)&&!loom_request_reap(&t,&slots,ref),"existing CONNECT owner path unchanged");
 puts("PASS physical-owner lifecycle, stale identity, close/discard and no-owner compatibility");
}
'''
mutants={
 'erase-removal-barrier':('r->phase != phase || r->borrows || r->owners','r->phase != phase || r->borrows','rollback waits physical owner'),
 'duplicate-owner':('r->phase!=LREQ_PREPARED || r->owners || r->borrows','r->phase!=LREQ_PREPARED || r->borrows','one physical owner'),
 'late-owner':('r->phase!=LREQ_PREPARED || r->owners || r->borrows','r->owners || r->borrows','attach only before admission'),
 'forget-hold':('r->owners=1','r->owners=0','owner retained'),
 'forget-drop':('r->owners=0','r->owners=1','owner dropped'),
 'double-drop':('r->owners!=1 || r->borrows || r->phase==LREQ_ACTIVE','r->borrows || r->phase==LREQ_ACTIVE','double owner drop rejected'),
 'active-drop':(' || r->phase==LREQ_ACTIVE','', 'active owner cannot disappear'),
 'borrowed-drop':('r->owners!=1 || r->borrows || r->phase==LREQ_ACTIVE','r->owners!=1 || r->phase==LREQ_ACTIVE','terminal borrow prevents physical release'),
 'stale-identity':('r->identity == ref.identity','true','stale hold refused')}
sdk=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip();results=[]
for name,edit in [('clean',None),*mutants.items()]:
 s=source
 if edit:assert edit[0] in s;s=s.replace(edit[0],edit[1],1)
 c=out/(name+'.c');c.write_text(s+harness);exe=out/name
 cmd=['/opt/homebrew/opt/llvm@22/bin/clang','-isysroot',sdk,'-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g','-I'+str(D/'kernel/include'),'-I'+str(R/'kernel/include'),str(c),str(R/'kernel/loom_service_slots.c'),str(R/'kernel/loom_service_nodes.c'),'-o',str(exe)]
 with (out/(name+'-compile.log')).open('wb') as f:r=subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT)
 assert r.returncode==0,(name,'compile')
 with (out/(name+'.log')).open('wb') as f:r=subprocess.run([str(exe)],stdout=f,stderr=subprocess.STDOUT)
 assert (r.returncode==0)==(name=='clean'),(name,r.returncode)
 if edit:assert edit[2] in (out/(name+'.log')).read_text(),(name,'wrong witness')
 results.append(dict(case=name,rc=r.returncode));print('PASS',name,flush=True)
(out/'result.json').write_text(json.dumps(dict(source_sha256=hashlib.sha256(source.encode()).hexdigest(),cases=results,limits='Request-ledger physical owner flag only; actual charged RPC allocation/free and native integration unimplemented.'),indent=2)+'\n');print(out)
