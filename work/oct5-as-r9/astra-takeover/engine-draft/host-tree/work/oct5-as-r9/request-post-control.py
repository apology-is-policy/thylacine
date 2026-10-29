#!/usr/bin/env python3
"""Actual terminal-post C with real request ledger; checked sequential CQ/lock doubles.
Native owner fixture separately tests actual allocation and ring geometry.
"""
from pathlib import Path
import datetime,difflib,hashlib,importlib.util,json,os,re,subprocess
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('extract',Path(__file__).with_name('final-reclaim-control.py'))
extract=importlib.util.module_from_spec(spec);spec.loader.exec_module(extract)
PRE=r'''
#include <stdio.h>
#include <stdlib.h>
#include <thylacine/loom_service_requests.h>
#include <thylacine/errno.h>
#define LOOM_MAGIC 123
#define CHECK(c,s) do { if(!(c)) { puts(s);exit(11); } } while(0)
typedef struct { int held; } spin_lock_t;
static int held,wakes;
static void spin_lock(spin_lock_t *l) {CHECK(!l->held && !held,"lock reentry");l->held=1;held++;}
static void spin_unlock(spin_lock_t *l) {CHECK(l->held && held==1,"lock release");l->held=0;held--;}
struct loom_ring_hdr {u32 cq_head,cq_tail,cq_mask;};
struct loom_cqe {u64 user_data;s32 result;u32 flags;};
struct Loom {u32 magic;spin_lock_t lock;void *service_as;bool service_closing;
 struct loom_request_table service_requests;u8 *ring_kva;u32 hdr_off,cqe_off,receipt_off,receipt_size,cq_tail,cq_entries;int cq_waiters;};
static bool loom_is_private(struct Loom *l) {return l && l->service_as;}
static void poll_waiter_list_wake(int *p) {(void)p;CHECK(!held,"wake outside owner lock");wakes++;}
'''
MAIN=r'''
int main(void) {
 struct {struct loom_ring_hdr hdr;struct loom_cqe cq[2];struct loom_service_buffer_receipt receipts[2];} ring={0};
 struct loom_service_request records[2]={0};
 struct Loom l={.magic=LOOM_MAGIC,.service_as=&ring,.ring_kva=(u8 *)&ring,.cq_entries=2,.cq_tail=2,
 .cqe_off=(u32)((u8 *)ring.cq-(u8 *)&ring),.receipt_off=(u32)((u8 *)ring.receipts-(u8 *)&ring),.receipt_size=sizeof(ring.receipts),
 .service_requests={.records=records,.capacity=2,.occupied=1}};
 struct loom_request_ref ref={.index=0,.identity=3};
 records[0]=(struct loom_service_request){.identity=3,.user_data=77,.result=9,.phase=LREQ_TERMINAL};
 ring.cq[0].user_data=999;ring.receipts[0].lease=999;
 CHECK(loom_private_post_terminal(&l,ref)==-T_E_AGAIN && records[0].phase==LREQ_TERMINAL &&
       l.cq_tail==2 && ring.cq[0].user_data==999 && ring.receipts[0].lease==999 && !wakes,"full CQ leaves terminal and output unchanged");
 ring.hdr.cq_head=1;ring.hdr.cq_tail=0xFFFFFFFFu;ring.hdr.cq_mask=0xFFFFFFFFu;l.service_closing=true;
 CHECK(!loom_private_post_terminal(&l,ref) && l.cq_tail==3 && ring.hdr.cq_tail==3 && wakes==1,
       "prior committed terminal posts after close using private tail");
 CHECK(ring.cq[0].user_data==77 && ring.cq[0].result==9 && !ring.cq[0].flags && !ring.receipts[0].lease,
       "exact scalar terminal clears old receipt");
 CHECK(records[0].phase==LREQ_DELIVERED && loom_private_post_terminal(&l,ref)==-T_E_BUSY && wakes==1 && l.cq_tail==3,
       "terminal delivered exactly once");
 records[1]=(struct loom_service_request){.identity=4,.user_data=88,.result=-T_E_CANCELED,.phase=LREQ_TERMINAL};
 l.service_requests.detached=true;ring.hdr.cq_head=2;ref=(struct loom_request_ref){.index=1,.identity=4};
 CHECK(loom_private_post_terminal(&l,ref)==-T_E_CANCELED && l.cq_tail==3 && records[1].phase==LREQ_TERMINAL && wakes==1,
       "detached consumer receives no new CQE");
 ref.identity=99;
 CHECK(loom_private_post_terminal(&l,ref)==-T_E_NOENT,"stale terminal refused");
 CHECK(!held,"lock balanced");puts("PASS actual request terminal post");return 0;
}
'''
def main():
 os.chdir(ROOT)
 assert re.search(r'^mac +HELD by you',subprocess.check_output(['work/oct5-as-r9/astra-yip.sh','resources'],text=True),re.M)
 st=os.statvfs('.');assert st.f_bavail*st.f_frsize>=8*1024**3
 out=Path('work/oct5-as-r9/astra-takeover')/('request-post-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));out.mkdir()
 source=Path('kernel/loom.c').read_text();fn=extract.fun(source,'loom_private_post_terminal')
 control=PRE+fn+MAIN
 cases=[('control',None,None,None),
 ('overwrite-full','if ((u32)(tail-head) >= l->cq_entries)','if ((u32)(tail-head) >= l->cq_entries && false)','full CQ leaves terminal and output unchanged'),
 ('shared-tail','u32 tail = l->cq_tail;','u32 tail = h->cq_tail;','full CQ leaves terminal and output unchanged'),
 ('omit-deliver','rc = loom_request_deliver(t,ref);','rc = 0;','terminal delivered exactly once'),
 ('omit-receipt','receipts[idx] = (struct loom_service_buffer_receipt){0};','(void)receipts;','exact scalar terminal clears old receipt'),
 ('close-reject','if (t->detached) {','if (t->detached || l->service_closing) {','prior committed terminal posts after close using private tail'),
 ('wake-under-lock','    spin_unlock(&l->lock);\n    poll_waiter_list_wake(&l->cq_waiters);','    poll_waiter_list_wake(&l->cq_waiters);\n    spin_unlock(&l->lock);','wake outside owner lock')]
 sdk=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip();results=[]
 paths=['kernel/loom.c','kernel/include/thylacine/loom.h','kernel/loom_service_requests.c','kernel/include/thylacine/loom_service_requests.h','kernel/loom_service_slots.c',str(Path(__file__).relative_to(ROOT))]
 hashes={p:hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in paths}
 for tag,old,new,want in cases:
  f=fn
  if old:assert f.count(old)==1,(tag,old);f=f.replace(old,new)
  src=out/(tag+'.c');src.write_text(PRE+f+MAIN);exe=out/tag
  (out/(tag+'.diff')).write_text(''.join(difflib.unified_diff(control.splitlines(True),src.read_text().splitlines(True),fromfile='control',tofile=tag)))
  cmd=['/opt/homebrew/opt/llvm@22/bin/clang','-std=c11','-O1','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-isysroot',sdk,'-I',str(ROOT/'kernel/include'),str(src),'kernel/loom_service_requests.c','kernel/loom_service_slots.c','-o',str(exe)]
  (out/(tag+'-command.json')).write_text(json.dumps(cmd)+'\n')
  with (out/(tag+'-build.log')).open('wb') as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
  r=subprocess.run([str(exe.resolve())],capture_output=True,text=True,timeout=15,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
  text=r.stdout+r.stderr;(out/(tag+'.log')).write_text(text)
  expected=want or 'PASS actual request terminal post';rc=11 if want else 0
  assert r.returncode==rc and text==expected+'\n',(tag,r.returncode,text)
  results.append(dict(case=tag,rc=rc,expected=expected,generated_sha256=hashlib.sha256(src.read_bytes()).hexdigest()));print('PASS',tag,expected,flush=True)
 assert hashes=={p:hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in paths}
 (out/'result.json').write_text(json.dumps({'scope':__doc__,'source_head':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),'hashes':hashes,'results':results},indent=2)+'\n');print(out)
if __name__=='__main__':main()
