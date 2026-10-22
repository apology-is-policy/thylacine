#!/usr/bin/env python3
"""Actual Loom constructors and paired publication (loom_provided_deliver, the
live path) with the real pool, slot and request ledgers and the shared guest
fixture. Host doubles: spinlocks, allocations, arm64 dsb and poll wakes. Store
observer checks publication state at the release-tail boundary; not an ARM
memory model.
"""
from pathlib import Path
import argparse,json,os,shlex,subprocess,tempfile,importlib.util
ROOT=Path(__file__).resolve().parent.parent
spec=importlib.util.spec_from_file_location('progress_fixture',ROOT/'tools/test-9p-client-progress.py');helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
spec=importlib.util.spec_from_file_location('host_sanitizer_cc',ROOT/'tools/host-sanitizer-cc.py');sanitizer_cc=importlib.util.module_from_spec(spec);spec.loader.exec_module(sanitizer_cc)
NAMES=['align_up_u32','is_pow2_u32','loom_measure','loom_create_layout','loom_create','loom_create_with_receipts','loom_drive_moved_locked','loom_post_cqe']
PROVIDED=['provided_same','provided_pool','provided_cell','loom_provided_deliver']
LEDGERS=['kernel/loom_service_pool.c','kernel/loom_service_pool_table.c','kernel/loom_service_slots.c','kernel/loom_service_requests.c']
OBSERVED='publication pairs payload receipt and lease before tail'
MUTANTS=[
 ('early-tail','    receipts[idx]=o->receipt;','    __atomic_store_n(&h->cq_tail,tail+1u,__ATOMIC_RELEASE);\n    receipts[idx]=o->receipt;',OBSERVED),
 ('wrong-receipt','    receipts[idx]=o->receipt;','    receipts[idx]=(struct loom_service_buffer_receipt){0};',OBSERVED),
 ('not-leased','    if(loom_pool_deliver(&l->service_pools->bank,p,&o->receipt))extinction("provided delivery changed inside Ll");\n','',OBSERVED),
 ('shared-tail','    u32 tail=l->cq_tail,head=','    u32 tail=h->cq_tail,head=','paired payload published'),
 ('no-backpressure','if((u32)(tail-head)>=l->cq_entries ||','if((u32)(tail-head)>l->cq_entries ||','full CQ retains pending payload'),
 ('no-detached-refusal','    if(l->service_requests.detached){rc=-T_E_CANCELED;goto done;}\n','','detached consumer refuses publication'),
 ('no-geometry-guard','    if(!l->receipt_size){rc=-T_E_OPNOTSUPP;goto done;}\n','','legacy ring refuses paired publication'),
 ('no-wake','spin_unlock(&l->lock);if(!rc)poll_waiter_list_wake(&l->cq_waiters);return rc;','spin_unlock(&l->lock);return rc;','publication wakes the consumer'),
 ('refusal-wakes','spin_unlock(&l->lock);if(!rc)poll_waiter_list_wake(&l->cq_waiters);return rc;','spin_unlock(&l->lock);poll_waiter_list_wake(&l->cq_waiters);return rc;','refusals wake no consumer'),
 ('shared-mask','u32 idx=tail&(l->cq_entries-1u);','u32 idx=tail&h->cq_mask;','publication wrote a slot the private tail did not select'),
 ('settling-only','(o->phase!=LRPC_SETTLING && o->phase!=LRPC_TERMINAL)','o->phase!=LRPC_SETTLING',"a cancelled multishot's committed shot still publishes"),
 ('terminal-arm-dead','    if(o->pool_terminal) {','    if(false) {','terminal must match its request'),
 ('no-request-room','       (o->pool_terminal && !loom_request_deliver_room(&l->service_requests))){','       false){','provided terminal delivery lost'),
 ('no-terminal-deliver','    if(o->pool_terminal && loom_request_deliver(&l->service_requests,o->request))extinction("provided terminal delivery lost");\n','','single-shot terminal and payload share one CQE'),
 ('always-more','(c->more?LOOM_CQE_MORE:0)','LOOM_CQE_MORE','single-shot terminal and payload share one CQE'),
 ('missing-clear','        r[idx] = (struct loom_service_buffer_receipt){0};','        (void)r;','nonleased CQE clears old receipt'),
 ('false-buffer','    if (flags & LOOM_CQE_SERVICE_BUFFER) return -1;','    (void)flags;','untyped publication cannot mint buffer flag'),
 ('short-receipts','cq_entries * (u32)sizeof(struct loom_service_buffer_receipt)','cq_entries * 16u','receipt geometry bounded aligned'),
]
def main():
 p=argparse.ArgumentParser();p.add_argument('--logs',type=Path,required=True);p.add_argument('--mutants',action='store_true');a=p.parse_args();a.logs.mkdir(parents=True,exist_ok=True)
 source=(ROOT/'kernel/loom.c').read_text();actual='\n\n'.join(helper.function(source,n) for n in NAMES)
 # loom_measure writes through a struct loom_layout, so the type has to come
 # across with it or the extracted translation unit does not compile.
 _s=source.index('struct loom_layout {');actual=source[_s:source.index('};',_s)+2]+'\n'+actual
 actual=actual.replace('__asm__ __volatile__("dsb ish" ::: "memory");','__atomic_thread_fence(__ATOMIC_SEQ_CST);')
 provided=(ROOT/'kernel/loom_service_provided.c').read_text()
 actual+='\n\n'+'\n\n'.join(helper.function(provided,n) for n in PROVIDED)
 template=(ROOT/'tools/host-tests/loom-receipts.c').read_text();assert template.count('/* ACTUAL_LOOM */')==1
 cc,cc_extra=sanitizer_cc.select(a.logs)
 with tempfile.TemporaryDirectory(prefix='loom-receipts-') as tmp:
  for name,old,new,want in [('clean',None,None,None)]+(MUTANTS if a.mutants else []):
   s=actual
   if old:assert s.count(old)==1,name;s=s.replace(old,new)
   f=Path(tmp)/(name+'.c');f.write_text(template.replace('/* ACTUAL_LOOM */',s));exe=f.with_suffix('')
   cmd=[cc,'-std=c11','-O1','-Wall','-Wextra','-Werror',*sanitizer_cc.SANITIZE,'-I',str(ROOT/'kernel/include'),'-I',str(ROOT/'kernel/test'),str(f)]+[str(ROOT/x) for x in LEDGERS]+['-o',str(exe)]+shlex.split(os.environ.get('CFLAGS',''))
   if '-isysroot' not in cmd:cmd+=cc_extra
   (a.logs/(name+'-command.json')).write_text(json.dumps(cmd)+'\n')
   sanitizer_cc.build(cmd,a.logs/(name+'-build.log'))
   r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=20);(a.logs/(name+'.log')).write_text(r.stdout+r.stderr)
   sanitizer_cc.check(name,r,want,any_failure=True)
   print('PASS '+name+(': '+want if want else ''),flush=True)
if __name__=='__main__':main()
