#!/usr/bin/env python3
"""Extracted target owner transactions and real shared slot ledger.
Checked sequential handle/native-capture/Loom doubles under ASan/UBSan; forced
close during capture. Native fixture separately covers actual registry lifetime.
No protocol, concurrent schedule or guest allocator qualification.
"""
from pathlib import Path
import datetime,difflib,hashlib,importlib.util,json,os,re,subprocess
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('extract',Path(__file__).with_name('final-reclaim-control.py'))
extract=importlib.util.module_from_spec(spec);spec.loader.exec_module(extract)
PRE=r'''
#include <stdio.h>
#include <stdlib.h>
#include <thylacine/loom_service_slots.h>
#include <thylacine/errno.h>
#define THYLACINE_DEVSRV_H
struct SrvServiceTarget { void *view,*service;u64 generation;u8 name_len;char name[32]; };
#include <thylacine/loom_service_targets.h>
#define LOOM_MAGIC 123
#define PROC_MAGIC 456
#define KOBJ_SPOOR 4
#define CHECK(c,s) do { if(!(c)) { puts(s);exit(11); } } while(0)
typedef struct { int held; } spin_lock_t;
static int held,registry_refs=1,handle_refs=1,retired,capture_error,handle_kind=KOBJ_SPOOR;
struct AddrSpace { int unused; };
struct Proc { u32 magic;struct AddrSpace *as;u64 stripes; };
struct Loom { u32 magic;spin_lock_t lock;struct AddrSpace *service_as;u64 service_creator;
 bool service_closing;struct loom_slot_table *service_slots;struct loom_service_target_table *service_targets;int refs; };
struct Handle { int kind;void *obj; };
static struct AddrSpace a,other;
static struct Loom *close_during_capture;
static void spin_lock(spin_lock_t *l) { CHECK(!l->held,"lock reentry");l->held=1;held++; }
static void spin_unlock(spin_lock_t *l) { CHECK(l->held,"lock release");l->held=0;held--; }
static void extinction(const char *s) {puts(s);exit(12);}
static bool loom_is_private(struct Loom *l) { return l && l->service_as; }
static void loom_private_targets_destroy(struct Loom *);
static void loom_ref(struct Loom *l) {CHECK(held==1 && l->refs>0,"owner pin under lock");l->refs++;}
static void loom_unref(struct Loom *l) {
 CHECK(!held && l->refs>0,"owner drop outside lock");
 if (!--l->refs) { loom_private_targets_destroy(l);retired++; }
}
static int handle_get(struct Proc *p,int fd,struct Handle *h) {
 (void)p;CHECK(!held,"handle resolution outside lock");
 if(fd!=7)return -1;h->obj=&a;h->kind=handle_kind;handle_refs++;return 0;
}
static void handle_put(struct Handle *h) {
 CHECK(!held && h->obj==&a && handle_refs>1,"handle release outside lock");handle_refs--;h->obj=NULL;
}
static int devsrv_service_target_init(void *root,const char *name,u32 len,struct SrvServiceTarget *out) {
 CHECK(!held && handle_refs==2 && root==&a && len==1 && name[0]=='x',"capture through retained actual handle");
 if(capture_error)return capture_error;
 *out=(struct SrvServiceTarget){.view=&a,.service=&other,.generation=42,.name_len=1,.name={'x'}};
 registry_refs++;
 if(close_during_capture) {close_during_capture->service_closing=true;loom_slots_close(close_during_capture->service_slots);close_during_capture=NULL;}
 return 0;
}
static void devsrv_service_target_clear(struct SrvServiceTarget *t) {
 CHECK(!held,"registry drop outside lock");
 if(t->view) {CHECK(t->view==&a && registry_refs>1,"registry ref owned");registry_refs--;}
 *t=(struct SrvServiceTarget){0};
}
'''
MAIN=r'''
int main(void) {
 struct Proc p={.magic=PROC_MAGIC,.as=&a,.stripes=99};
 struct loom_slot_table slots={0};struct loom_service_target_table targets={0};
 struct Loom l={.magic=LOOM_MAGIC,.refs=1,.service_as=&a,.service_creator=99,.service_slots=&slots,.service_targets=&targets};
 struct loom_service_target in={.size=sizeof(in),.version=1,.registry_fd=7,.name_len=1,.name={'x'}};
 struct loom_service_ref ref={0},stale={0},prov={0};const struct SrvServiceTarget *borrow=NULL;
 p.as=&other;CHECK(loom_private_target_prepare(&l,&p,&in,&ref)==-T_E_ACCES,"target image authority");p.as=&a;
 p.stripes++;CHECK(loom_private_target_prepare(&l,&p,&in,&ref)==-T_E_ACCES,"target creator authority");p.stripes--;
 in.flags=1;CHECK(loom_private_target_prepare(&l,&p,&in,&ref)==-T_E_INVAL,"target header validated");in.flags=0;
 in.name[255]=1;CHECK(loom_private_target_prepare(&l,&p,&in,&ref)==-T_E_INVAL,"target tail validated");in.name[255]=0;
 in.registry_fd=8;CHECK(loom_private_target_prepare(&l,&p,&in,&ref)==-T_E_BADF && !ref.incarnation && slots.slots[0].phase==LSLOT_FREE,"bad fd unwinds slot");in.registry_fd=7;
 handle_kind=3;CHECK(loom_private_target_prepare(&l,&p,&in,&ref)==-T_E_OPNOTSUPP && handle_refs==1,"typed handle refused");handle_kind=KOBJ_SPOOR;
 capture_error=-T_E_NOENT;CHECK(loom_private_target_prepare(&l,&p,&in,&ref)==-T_E_NOENT && handle_refs==1 && registry_refs==1 && slots.slots[0].phase==LSLOT_FREE,"capture refusal unwinds");capture_error=0;
 CHECK(!loom_private_target_prepare(&l,&p,&in,&ref) && registry_refs==2 && handle_refs==1,"provisional owns registry only");
 CHECK(loom_private_target_get(&l,ref,&borrow)==-T_E_NOENT && !borrow,"provisional borrow refused");
 stale=ref;CHECK(!loom_private_target_rollback(&l,ref) && registry_refs==1,"rollback drops registry");
 CHECK(!loom_private_target_prepare(&l,&p,&in,&ref) && ref.incarnation>stale.incarnation,"rollback burns identity");
 CHECK(!loom_private_target_publish(&l,ref),"publish target");
 CHECK(loom_private_target_get(&l,stale,&borrow)==-T_E_NOENT,"stale identity refused");
 CHECK(loom_private_target_reap(&l,stale)==-T_E_NOENT && registry_refs==2,"stale reap unchanged");
 CHECK(!loom_private_target_get(&l,ref,&borrow) && l.refs==2 && slots.slots[ref.slot].pins==1 && borrow->view==&a && borrow->service==&other && borrow->generation==42,"borrow pins owner and exact target");
 CHECK(loom_private_target_reap(&l,ref)==-T_E_BUSY && registry_refs==2,"busy reap unchanged");
 CHECK(!loom_private_target_put(&l,ref) && l.refs==1 && !slots.slots[ref.slot].pins,"put releases owner pin");
 CHECK(loom_private_target_put(&l,ref)==-T_E_INVAL && l.refs==1,"double put unchanged");
 CHECK(!loom_private_target_reap(&l,ref) && registry_refs==1 && slots.slots[ref.slot].phase==LSLOT_FREE,"reap drops registry and slot");
 // Capture takes a registry ref before close wins the unlocked gap.
 close_during_capture=&l;
 CHECK(loom_private_target_prepare(&l,&p,&in,&ref)==-T_E_CANCELED && registry_refs==1 && handle_refs==1 && slots.slots[0].phase==LSLOT_FREE,"close during capture rolls back both ownerships");
 // New independent owner for close-after-prepare and final borrower schedules.
 struct loom_slot_table slots2={0};struct loom_service_target_table targets2={0};
 struct Loom r={.magic=LOOM_MAGIC,.refs=1,.service_as=&a,.service_creator=99,.service_slots=&slots2,.service_targets=&targets2};
 CHECK(!loom_private_target_prepare(&r,&p,&in,&ref) && !loom_private_target_publish(&r,ref),"second owner target");
 CHECK(!loom_private_target_prepare(&r,&p,&in,&prov),"second owner provisional");
 CHECK(!loom_private_target_get(&r,ref,&borrow),"second owner borrow");
 r.service_closing=true;loom_slots_close(&slots2);
 CHECK(loom_private_target_publish(&r,prov)==-T_E_CANCELED,"close wins publish");
 CHECK(loom_private_target_get(&r,ref,&borrow)==-T_E_CANCELED,"close blocks new borrow");
 loom_unref(&r);CHECK(!retired && r.refs==1 && registry_refs==3,"borrow holds closed owner");
 CHECK(!loom_private_target_put(&r,ref) && retired==1 && registry_refs==1 && handle_refs==1,"last borrow clears live and provisional targets");
 CHECK(!held,"no held locks");puts("PASS target transactions and final borrower ownership");return 0;
}
'''
def main():
 os.chdir(ROOT)
 assert re.search(r'^mac +HELD by you',subprocess.check_output(['work/oct5-as-r9/astra-yip.sh','resources'],text=True),re.M)
 st=os.statvfs('.');assert st.f_bavail*st.f_frsize>=8*1024**3
 out=Path('work/oct5-as-r9/astra-takeover')/('private-targets-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));out.mkdir()
 source=Path('kernel/loom.c').read_text()
 names=['loom_private_targets_destroy','loom_private_target_exact','loom_private_target_prepare','loom_private_target_publish','loom_private_target_remove','loom_private_target_rollback','loom_private_target_reap','loom_private_target_get','loom_private_target_put']
 funcs={n:extract.fun(source,n) for n in names}
 def program(fs):return PRE+'\n'+'\n'.join(fs.values())+'\n'+MAIN
 control=program(funcs)
 cases=[('control',None,None,None,None),
 ('omit-image','loom_private_target_prepare','p->as != l->service_as || ','','target image authority'),
 ('omit-creator','loom_private_target_prepare',' || p->stripes != l->service_creator','','target creator authority'),
 ('allow-tail','loom_private_target_prepare','if (in->name[i])','if (in->name[i] && false)','target tail validated'),
 ('omit-handle-put','loom_private_target_prepare','        handle_put(&h);','        if(false) handle_put(&h);','typed handle refused'),
 ('omit-rollback-drop','loom_private_target_remove','if (!rc) devsrv_service_target_clear(&detached);','if (!rc && false) devsrv_service_target_clear(&detached);','rollback drops registry'),
 ('omit-stale','loom_private_target_exact','s->incarnation == ref.incarnation && ','','stale reap unchanged'),
 ('omit-owner-pin','loom_private_target_get','loom_ref(l);','if(false) loom_ref(l);','borrow pins owner and exact target'),
 ('allow-busy-reap','loom_private_target_remove','l->service_slots->slots[ref.slot].pins ?','l->service_slots->slots[ref.slot].pins && false ?','busy reap unchanged'),
 ('omit-owner-drop','loom_private_target_put','if (!rc) loom_unref(l);','if (!rc && false) loom_unref(l);','put releases owner pin'),
 ('allow-capture-close','loom_private_target_prepare','if (!rc && l->service_closing)','if (!rc && l->service_closing && false)','close during capture rolls back both ownerships'),
 ('omit-capture-drop','loom_private_target_prepare','if (rc) devsrv_service_target_clear(&fresh);','if (rc && false) devsrv_service_target_clear(&fresh);','close during capture rolls back both ownerships'),
 ('omit-final-drop','loom_private_targets_destroy','            devsrv_service_target_clear(t);','            if(false) devsrv_service_target_clear(t);','last borrow clears live and provisional targets')]
 sdk=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip();results=[]
 module=Path('kernel/loom_service_slots.c');module_hash=hashlib.sha256(module.read_bytes()).hexdigest()
 for tag,fn,old,new,want in cases:
  fs=dict(funcs)
  if fn:
   assert fs[fn].count(old)==1,(tag,old);fs[fn]=fs[fn].replace(old,new)
  src=out/(tag+'.c');src.write_text(program(fs));exe=out/tag
  (out/(tag+'.diff')).write_text(''.join(difflib.unified_diff(control.splitlines(True),src.read_text().splitlines(True),fromfile='control',tofile=tag)))
  cmd=['/opt/homebrew/opt/llvm@22/bin/clang','-std=c11','-O1','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-isysroot',sdk,'-I',str(ROOT/'kernel/include'),str(src),str(module),'-o',str(exe)]
  (out/(tag+'-command.json')).write_text(json.dumps(cmd)+'\n')
  with (out/(tag+'-build.log')).open('wb') as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
  r=subprocess.run([str(exe.resolve())],capture_output=True,text=True,timeout=15,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
  text=r.stdout+r.stderr;(out/(tag+'.log')).write_text(text)
  expected=want or 'PASS target transactions and final borrower ownership';rc=11 if want else 0
  assert r.returncode==rc and text==expected+'\n',(tag,r.returncode,text)
  results.append(dict(case=tag,rc=rc,expected=expected,generated_sha256=hashlib.sha256(src.read_bytes()).hexdigest()));print('PASS',tag,expected,flush=True)
 assert source==Path('kernel/loom.c').read_text() and module_hash==hashlib.sha256(module.read_bytes()).hexdigest()
 (out/'result.json').write_text(json.dumps({'scope':__doc__,'source_head':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),'loom_sha256':hashlib.sha256(source.encode()).hexdigest(),'slot_sha256':module_hash,'results':results},indent=2)+'\n');print(out)
if __name__=='__main__':main()
