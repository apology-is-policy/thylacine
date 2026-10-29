from pathlib import Path
import importlib.util,subprocess,datetime,hashlib,json,os,difflib
D=Path(__file__).resolve().parent;R=Path('/Users/northkillpd/projects/thylacine-corona')
spec=importlib.util.spec_from_file_location('extract',R/'work/oct5-as-r9/final-reclaim-control.py');ex=importlib.util.module_from_spec(spec);spec.loader.exec_module(ex)
s=(R/'kernel/loom.c').read_text();body=ex.fun(s,'loom_private_nodes_destroy')
pre=r'''
#include <stdio.h>
#include <stdlib.h>
#include <thylacine/loom_service_fids.h>
#define CHECK(c,m) do{if(!(c)){puts(m);exit(11);}}while(0)
struct Loom {struct loom_slot_table *service_slots;struct loom_service_node_table *service_nodes;};
static void extinction(const char *s){puts(s);exit(86);}
'''
main=r'''
int main(int argc,char **argv){
 int mode=argc>1?atoi(argv[1]):0;
 struct loom_slot_table slots={0};struct loom_service_node_table nodes={0};
 struct Loom l={&slots,&nodes};struct loom_service_ref parent={0},child={0},none={0};
 CHECK(!loom_service_node_prepare(&slots,&nodes,LOOM_SERVICE_SCOPE,none,&parent) &&
       !loom_service_node_publish(&slots,&nodes,parent),"parent preparation");
 nodes.nodes[parent.slot].state=LOOM_SERVICE_READY;
 if(mode<=1 || mode>=8){
  CHECK(!loom_service_node_prepare(&slots,&nodes,LOOM_SERVICE_FID,parent,&child),"provisional child pins parent");
  if(mode==1 || mode>=8)CHECK(!loom_service_node_publish(&slots,&nodes,child),"live unused child pins parent");
  CHECK(slots.slots[parent.slot].pins==1,"exact parent pin");
 }
 if(mode>=8) {
  CHECK(!loom_fid_claim_walk(&slots,&nodes,child,parent,1),"derived claim");
  CHECK(!loom_fid_commit_walk(&slots,&nodes,child,parent,1,1),"derived binding");
  if(mode!=9)CHECK(!loom_fid_release_claim(&slots,&nodes,child,1),"derived release");
  if(mode==10){u32 wire;CHECK(!loom_fid_borrow(&slots,&nodes,child,parent,0,&wire),"derived borrow");}
 }
 nodes.nodes[parent.slot].state=LOOM_SERVICE_RETIRED;
 if(mode==2)nodes.nodes[parent.slot].state=LOOM_SERVICE_READY;
 if(mode==3)nodes.nodes[parent.slot].state=LOOM_SERVICE_ABORTING;
 if(mode==4)slots.slots[parent.slot].pins=1;
 if(mode==5)nodes.nodes[parent.slot].parent.incarnation=1;
 if(mode==6)nodes.nodes[parent.slot].reserved=1;
 if(mode==7)slots.slots[parent.slot].phase=LSLOT_PROVISIONAL;
 loom_slots_close(&slots);loom_private_nodes_destroy(&l);
 CHECK(loom_service_nodes_empty(&nodes),"retired scope and unused child removed");
 for(u32 i=0;i<LOOM_SERVICE_SLOT_COUNT;i++)CHECK(slots.slots[i].phase==LSLOT_FREE && !slots.slots[i].pins,"child and parent slots empty");
 puts("PASS child before retired parent cleanup");return 0;
}
'''
variants=[('control',body,None,None),('parent-first',body.replace('LOOM_SERVICE_FID, LOOM_SERVICE_SCOPE','LOOM_SERVICE_SCOPE, LOOM_SERVICE_FID'),0,'private Loom retained pinned retired scope'),('ignore-retired',body.replace('n->state==LOOM_SERVICE_RETIRED','false'),0,'private Loom retained active or invalid node'),('omit-parent-clear',body.replace('*n=(struct loom_service_node){0};','/* omitted */'),0,'private Loom retained unowned node'),('permit-ready',body.replace('n->state==LOOM_SERVICE_RETIRED','(n->state==LOOM_SERVICE_RETIRED || n->state==LOOM_SERVICE_READY)'),2,'PASS child before retired parent cleanup'),('ignore-parent',body.replace('n->parent.incarnation ||','false ||'),5,'PASS child before retired parent cleanup'),('ignore-reserved',body.replace('|| n->reserved ||', '|| false ||'),6,'PASS child before retired parent cleanup')]
variants += [('omit-derived-reap',body.replace('loom_fid_reap(l->service_slots,l->service_nodes,ref)','0'),8,'private Loom retained pinned retired scope')]
out=D/('metadata-controls-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));out.mkdir();sdk=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip();results=[]
for name,code,case,expected in variants:
 src=out/(name+'.c');src.write_text(pre+code+main);exe=out/name
 (out/(name+'.diff')).write_text(''.join(difflib.unified_diff(body.splitlines(True),code.splitlines(True))))
 cmd=['/opt/homebrew/opt/llvm@22/bin/clang','-std=c11','-O1','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-isysroot',sdk,'-I',str(R/'kernel/include'),str(src),str(R/'kernel/loom_service_slots.c'),str(R/'kernel/loom_service_nodes.c'),str(R/'kernel/loom_service_fids.c'),'-o',str(exe)]
 r=subprocess.run(cmd,capture_output=True,text=True);(out/(name+'-build.log')).write_text(r.stdout+r.stderr);assert r.returncode==0,r.stderr
 (out/(name+'-command.json')).write_text(json.dumps(cmd)+'\n')
 for mode in range(11) if name=='control' else [case]:
  r=subprocess.run([str(exe),str(mode)],capture_output=True,text=True,timeout=10,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
  txt=r.stdout+r.stderr;(out/f'{name}-{mode}.log').write_text(txt)
  want=expected or ('PASS child before retired parent cleanup' if mode<=1 or mode==8 else 'private Loom retained live derived fid' if mode>=9 else 'private Loom retained active or invalid node' if mode<=3 else 'private Loom retained pinned retired scope')
  rc=0 if want.startswith('PASS') else 86
  assert r.returncode==rc and txt==want+'\n',(name,mode,r.returncode,txt,want)
  results.append({'variant':name,'case':mode,'rc':rc,'expected':want,'sha256':hashlib.sha256(src.read_bytes()).hexdigest()})
assert s==(R/'kernel/loom.c').read_text()
(out/'result.json').write_text(json.dumps({'scope':'Actual final metadata cleanup, sequential host slot/node ledgers; test simulates retirement, not protocol proof. Eleven clean cases plus seven targeted mutants; derived bind is simulated, not a wire proof.','loom_sha256':hashlib.sha256(s.encode()).hexdigest(),'results':results},indent=2)+'\n');print(out)
