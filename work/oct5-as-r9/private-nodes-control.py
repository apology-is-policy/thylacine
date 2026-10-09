#!/usr/bin/env python3
"""Extracted node owner transactions with the real shared slot/node modules.
Checked sequential lock/image doubles and the shared native fixture. READY and
ABORTING are injected test states; no protocol/usercopy/concurrency claim.
"""
from pathlib import Path
import datetime,difflib,hashlib,importlib.util,json,os,re,subprocess
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('extract',Path(__file__).with_name('final-reclaim-control.py'))
extract=importlib.util.module_from_spec(spec);spec.loader.exec_module(extract)
PRE=r'''
#include <stdio.h>
#include <stdlib.h>
#include <thylacine/loom_service_nodes.h>
#include <thylacine/errno.h>
#define LOOM_MAGIC 123
#define PROC_MAGIC 456
#define CHECK(c,s) do { if(!(c)) { puts(s);exit(11); } } while(0)
typedef struct { int held; } spin_lock_t;
static int held;
struct AddrSpace { int unused; };
struct Proc { u32 magic;struct AddrSpace *as;u64 stripes; };
struct Loom { u32 magic;spin_lock_t lock;struct AddrSpace *service_as;u64 service_creator;
 bool service_closing;struct loom_slot_table *service_slots;struct loom_service_node_table *service_nodes; };
static void spin_lock(spin_lock_t *l) {CHECK(!l->held && !held,"lock reentry");l->held=1;held++;}
static void spin_unlock(spin_lock_t *l) {CHECK(l->held && held==1,"lock release");l->held=0;held--;}
static bool loom_is_private(struct Loom *l) {return l && l->service_as;}
static void loom_exec_latch(struct Loom *l) {
 spin_lock(&l->lock);l->service_closing=true;loom_slots_close(l->service_slots);spin_unlock(&l->lock);
}
// Instrument module call boundaries only; the production modules are linked
// unmodified. This checks that the actual extracted wrapper holds its lock.
static int checked_prepare(struct loom_slot_table *s,struct loom_service_node_table *n,u32 k,
 struct loom_service_ref p,struct loom_service_ref *out) {
 CHECK(held==1,"prepare requires owner lock");return loom_service_node_prepare(s,n,k,p,out);
}
static int checked_publish(struct loom_slot_table *s,struct loom_service_node_table *n,struct loom_service_ref r) {
 CHECK(held==1,"publish requires owner lock");return loom_service_node_publish(s,n,r);
}
static int checked_rollback(struct loom_slot_table *s,struct loom_service_node_table *n,struct loom_service_ref r) {
 CHECK(held==1,"rollback requires owner lock");return loom_service_node_rollback(s,n,r);
}
static int checked_reap_unused(struct loom_slot_table *s,struct loom_service_node_table *n,struct loom_service_ref r) {
 CHECK(held==1,"reap requires owner lock");return loom_service_node_reap_unused(s,n,r);
}
#define loom_service_node_prepare checked_prepare
#define loom_service_node_publish checked_publish
#define loom_service_node_rollback checked_rollback
#define loom_service_node_reap_unused checked_reap_unused
'''
MAIN=r'''
#include "loom_node_owner_fixture.h"
int main(void) {
 struct AddrSpace a={0};struct Proc p={.magic=PROC_MAGIC,.as=&a,.stripes=99};
 struct loom_slot_table slots={0};struct loom_service_node_table nodes={0};
 struct Loom l={.magic=LOOM_MAGIC,.service_as=&a,.service_creator=99,.service_slots=&slots,.service_nodes=&nodes};
 const char *error=loom_node_owner_fixture(&l,&p);
 CHECK(!held && !l.lock.held,"wrapper leaves lock balanced");
 if(error) {puts(error);return 11;}
 puts("PASS node owner transactions");return 0;
}
'''
def main():
 os.chdir(ROOT)
 assert re.search(r'^mac +HELD by you',subprocess.check_output(['work/oct5-as-r9/astra-yip.sh','resources'],text=True),re.M)
 st=os.statvfs('.');assert st.f_bavail*st.f_frsize>=8*1024**3
 out=Path('work/oct5-as-r9/astra-takeover')/('private-nodes-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));out.mkdir()
 source=Path('kernel/loom.c').read_text()
 funcs={n:extract.fun(source,n) for n in ['loom_private_node_prepare','loom_private_node_publish','loom_private_node_rollback','loom_private_node_reap_unused']}
 def program(fs):return PRE+'\n'+'\n'.join(fs.values())+'\n'+MAIN
 control=program(funcs)
 cases=[('control',None,None,None,None),
 ('omit-image','loom_private_node_prepare','p->as != l->service_as || ','','node owner requires exact creator image'),
 ('omit-creator','loom_private_node_prepare',' || p->stripes != l->service_creator','','node owner requires exact creator incarnation')]
 for tag,old in [('size','in->size != sizeof(*in) || '),('version','in->version != LOOM_SERVICE_ABI_VERSION ||'),('flags','in->flags || '),('reserved','in->reserved || '),('result-slot','in->result.slot || '),('result-reserved','in->result.reserved ||'),('result-incarnation','in->result.incarnation')]:
  cases.append(('omit-'+tag,'loom_private_node_prepare',old,'false' if tag=='result-incarnation' else '', 'node reserve rejects malformed immutable record'))
 for suffix,want in [('prepare','prepare'),('publish','publish'),('rollback','rollback'),('reap_unused','reap')]:
  cases.append(('omit-'+suffix+'-lock','loom_private_node_'+suffix,'    spin_lock(&l->lock);','    /* lock omitted */',want+' requires owner lock'))
 sdk=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip();results=[]
 paths=['kernel/loom.c','kernel/include/thylacine/loom.h','kernel/loom_service_slots.c','kernel/loom_service_nodes.c','kernel/include/thylacine/loom_service_slots.h','kernel/include/thylacine/loom_service_nodes.h','kernel/test/loom_node_owner_fixture.h',str(Path(__file__).relative_to(ROOT))]
 hashes={p:hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in paths}
 for tag,fn,old,new,want in cases:
  fs=dict(funcs)
  if fn:
   assert fs[fn].count(old)==1,(tag,old);fs[fn]=fs[fn].replace(old,new)
  src=out/(tag+'.c');src.write_text(program(fs));exe=out/tag
  (out/(tag+'.diff')).write_text(''.join(difflib.unified_diff(control.splitlines(True),src.read_text().splitlines(True),fromfile='control',tofile=tag)))
  cmd=['/opt/homebrew/opt/llvm@22/bin/clang','-std=c11','-O1','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-isysroot',sdk,'-I',str(ROOT/'kernel/include'),'-I',str(ROOT/'kernel/test'),str(src),'kernel/loom_service_slots.c','kernel/loom_service_nodes.c','-o',str(exe)]
  (out/(tag+'-command.json')).write_text(json.dumps(cmd)+'\n')
  with (out/(tag+'-build.log')).open('wb') as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
  r=subprocess.run([str(exe.resolve())],capture_output=True,text=True,timeout=15,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
  text=r.stdout+r.stderr;(out/(tag+'.log')).write_text(text)
  expected=want or 'PASS node owner transactions';rc=11 if want else 0
  assert r.returncode==rc and text==expected+'\n',(tag,r.returncode,text)
  results.append(dict(case=tag,rc=rc,expected=expected,generated_sha256=hashlib.sha256(src.read_bytes()).hexdigest()));print('PASS',tag,expected,flush=True)
 assert hashes=={p:hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in paths}
 (out/'result.json').write_text(json.dumps({'scope':__doc__,'source_head':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),'hashes':hashes,'results':results},indent=2)+'\n');print(out)
if __name__=='__main__':main()
