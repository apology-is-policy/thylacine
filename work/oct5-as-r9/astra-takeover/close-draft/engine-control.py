from pathlib import Path
import re,json,subprocess,hashlib,datetime
D=Path(__file__).resolve().parent;R=Path('/Users/northkillpd/projects/thylacine-corona')
# Reuse the already reviewed actual-C boundary doubles, not old implementation.
prior=(R/'work/oct5-as-r9/astra-takeover/engine-draft/control.py').read_text().split("out=D/('controls-'")[0]
e={"__file__":__file__};exec(prior,e)
pre=e['pre'].replace('u32 service_fixed_count;','u32 service_fixed_count,service_fixed_settling;')
pre+='''
static int close_kicks,temporary_roots;
static void loom_private_close_changed(struct Loom *l) {(void)l;CHECK(!held,"close notification outside Ll");close_kicks++;}
static void loom_ref(struct Loom *l) {(void)l;temporary_roots++;}
static void loom_unref(struct Loom *l) {(void)l;CHECK(temporary_roots>0,"temporary root balanced");temporary_roots--;}
'''
oldmain=e['main'].replace('int main(int argc,char **argv)','static int old_main(int argc,char **argv)')
setup=oldmain[oldmain.index(' struct loom_slot_table slots='):oldmain.index(' struct loom_service_ref scope,second=')]
newmain=r'''
static int close_main(int which) {
 mode=0;
SETUP
 struct loom_service_ref scope,target={.slot=9,.incarnation=5};destination(&l,&scope);
 struct loom_service_protocol *p=NULL,*pin=NULL;
 CHECK(!loom_private_connect_prepare(&l,scope,target,77,100,1,&p),"close prepare");
 if(which==21) {
  close_owner(&l);int before=close_kicks;
  CHECK(loom_engine_close_visit(&l)==0 && !l.service_requests.detached,"unindexed constructor parks close");
  CHECK(!loom_private_connect_rollback(p) && close_kicks>before && temporary_roots==0,"raw rollback wakes after physical release under temporary root");
  CHECK(loom_engine_close_visit(&l)==2 && l.service_requests.detached,"constructor rollback completes close");
  puts("PASS close control");return 0;
 }
 CHECK(!loom_engine_adopt(p,2),"close adoption");
 CHECK(!loom_engine_get(&l,scope,&pin),"close held entry");externally_pinned=pin;
 if(which==22) {index.entries[scope.slot].phase=LENGINE_INSTALLING;}
 if(which==23) {p->driving=true;}
 if(which==24) {p->users=1;}
 close_owner(&l);l.cq_tail=2;
 int before=close_kicks;
 if(which>=22 && which<=24) {
  CHECK(loom_engine_close_visit(&l)==0 && !p->locally_retired && !aborts && close_kicks==before,"installer driver or teardown borrower parks without spin");
  index.entries[scope.slot].phase=LENGINE_LIVE;p->driving=false;p->users=0;
 }
 CHECK(loom_engine_close_visit(&l)==0 && p->locally_retired && aborts==1 &&
       !l.service_requests.detached && !steps && close_kicks==before,"quiet close retires cursor without CQ or self kick");
 externally_pinned=NULL;
 CHECK(!loom_engine_put(&l,scope,pin) && close_kicks>before,"external entry put notifies close");pin=NULL;
 if(which==25)l.service_fixed_settling=1;
 if(which==26)l.service_fixed_count=1;
 before=close_kicks;
 if(which==25 || which==26) {
  CHECK(loom_engine_close_visit(&l)==0 && !l.service_requests.detached && releases==0 && close_kicks==before,"fixed physical settling or borrow prevents discard");
  l.service_fixed_settling=0;l.service_fixed_count=0;
 }
 CHECK(loom_engine_close_visit(&l)==2 && l.service_requests.detached && releases==1 &&
       !l.service_requests.occupied && loom_engine_empty(&index) && !steps && close_kicks==before,
       "quiet close finishes exact ownership independent of full CQ");
 CHECK(nodes.nodes[scope.slot].state==LOOM_SERVICE_RETIRED && !slots.slots[scope.slot].pins,
       "retired scope metadata survives until safe final cleanup");
 CHECK(!loom_private_connect_reap(&l,scope),"close metadata can reap");
 puts("PASS close control");return 0;
}
int main(int argc,char **argv) {
 int which=argc>1?atoi(argv[1]):0;
 return which>=20 ? close_main(which) : old_main(argc,argv);
}
'''.replace('SETUP',setup)
main=oldmain+newmain
body=lambda p:re.sub(r'^#include[^\n]*\n','',p.read_text(),flags=re.M)
engine=body(D/'kernel/loom_service_engine.c');connect=body(D/'kernel/loom_service_connect.c')
post=e['c'].post.extract.fun((D/'kernel/loom.c').read_text(),'loom_private_post_terminal')
mutants=[
 ('self-kick-put','engine_put(l,ref,p,false)','engine_put(l,ref,p,true)',20,'quiet close retires cursor without CQ or self kick'),
 ('self-kick-collect','engine_discard_closed(l,false)','engine_discard_closed(l,true)',20,'quiet close finishes exact ownership independent of full CQ'),
 ('ignore-settling',' || l->service_fixed_settling','',25,'fixed physical settling or borrow prevents discard'),
 ('skip-cursor-abort','(void)loom_private_connect_step_retiring(p);','(void)p;',20,'quiet close retires cursor without CQ or self kick'),
 ('missing-external-put-kick','if (!rc && notify) engine_changed(l);','if (!rc && false) engine_changed(l);',20,'external entry put notifies close'),
 ('treat-busy-as-complete','if (!engine_discard_closed(l,false)) return 2;','if (engine_discard_closed(l,false)) return 2;',21,'unindexed constructor parks close'),
 ('spin-busy','return more ? 1 : 0;','return 1;',23,'installer driver or teardown borrower parks without spin')]
out=D/('engine-controls-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));out.mkdir();results=[]
for tag,old,new,case,want in [('control',None,None,None,None)]+mutants:
 code=engine
 if old:assert code.count(old)==1,(tag,code.count(old));code=code.replace(old,new)
 src=out/(tag+'.c');src.write_text(pre+connect+code+post+main)
 exe=out/(tag+'.exe');cmd=['/opt/homebrew/opt/llvm@22/bin/clang','-std=c11','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-but-set-variable','-g','-fsanitize=address,undefined','-isysroot',subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip(),'-I',str(D/'kernel/include'),'-I',str(R/'kernel/include'),str(src),str(R/'kernel/loom_service_requests.c'),str(R/'kernel/loom_service_slots.c'),str(R/'kernel/loom_service_nodes.c'),'-o',str(exe)]
 (out/(tag+'-command.json')).write_text(json.dumps(cmd)+'\n')
 with (out/(tag+'-build.log')).open('wb') as f:r=subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT)
 assert r.returncode==0,(tag,'build',str(out))
 for n in list(range(11))+list(range(20,27)) if case is None else [case]:
  r=subprocess.run([str(exe),str(n)],capture_output=True,text=True,timeout=15);s=r.stdout+r.stderr;(out/(tag+'-'+str(n)+'.log')).write_text(s)
  expected=want or ('PASS close control' if n>=20 else 'PASS engine control')
  assert r.returncode==(11 if want else 0) and r.stdout==expected+'\n' and not r.stderr,(tag,n,r.returncode,s,str(out))
  results.append(dict(case=tag,mode=n,expected=expected,rc=r.returncode,source_sha256=hashlib.sha256(src.read_bytes()).hexdigest(),log_sha256=hashlib.sha256(s.encode()).hexdigest()))
 print('PASS',tag,flush=True)
(out/'result.json').write_text(json.dumps({'results':results,'inputs':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in [D/'kernel/loom_service_engine.c',D/'kernel/loom_service_connect.c',D/'kernel/include/thylacine/loom_service_engine.h',Path(__file__)]},'limits':'Actual draft engine and CONNECT C with production request/slot/node ledgers; deterministic boundary doubles; kernel queue wrapper/native qualification still pending.'},indent=2)+'\n');print(out)
