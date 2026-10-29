from pathlib import Path
import subprocess,json,hashlib,datetime,re,os,difflib,importlib.util
D=Path(__file__).resolve().parent;R=D.parents[3];T=D.parent
W=T/'wire-draft';P=T/'program-draft'
spec=importlib.util.spec_from_file_location('extract',R/'work/oct5-as-r9/final-reclaim-control.py');extract=importlib.util.module_from_spec(spec);spec.loader.exec_module(extract)
M=[
 ('provided-creator-image','provided','p->as!=l->service_as || ','','provided creator image exact'),
 ('provided-creator-incarnation','provided',' || p->stripes!=l->service_creator','','provided creator incarnation exact'),
 ('all-member-bounds','provided','maximum>e.length || ','','all member bounds precede stream ownership'),
 ('receipt-copy','provided','receipts[idx]=o->receipt;','receipts[idx]=(struct loom_service_buffer_receipt){0};','single CQE and receipt share publication'),
 ('consumer-detached','provided','if(l->service_requests.detached)','if(false && l->service_requests.detached)','detached consumer cannot publish'),
 ('pool-stream-free','operation','!op->pool_stream && ','','storage free after physical holds'),
 ('pool-detach-pending','provided','o->pool_busy || o->pool_pending ||\n       (o->phase!=LRPC_NEW','o->pool_busy ||\n       (o->phase!=LRPC_NEW', 'terminal bookkeeping retains stream'),
 ('pool-claim-required','rpc','if(o->pool_stream && !o->pool_busy)return -T_E_BUSY;','/* omit pool guard */','raw begin refuses unclaimed pool'),
 ('canonical-member','provided','+e.offset;',';','selected canonical member before read'),
 ('pending-terminal-order','admission',' || o->pool_pending)return -T_E_BUSY;',' )return -T_E_BUSY;','cancellation cannot overtake pending MORE'),
 ('delivered-more-proof','rpc',' || !o->pool_more_delivered','','new shot requires delivered MORE proof'),
 ('fresh-rearm','rpc','loom_rpc_check_finish_locked(o,o->program.result,now);if(rc)return rc;','loom_rpc_check_finish_locked(o,o->program.result,now);if(rc && rc!=-T_E_CANCELED)return rc;','abort preserves MORE but bars new shot'),
 ('cq-full','provided','if((u32)(tail-head)>=l->cq_entries)','if(false && (u32)(tail-head)>=l->cq_entries)','full CQ preserves committed payload'),
 ('single-delivery','provided','if(o->pool_terminal && loom_request_deliver','if(false && o->pool_terminal && loom_request_deliver','committed single survives scope abort and delivers once'),
 ('more-flag','provided','(c->more?LOOM_CQE_MORE:0)','(c->more?0:0)','MORE delivery has explicit receipt'),
 ('member-view-clear','provided','o->program.wire.payload=NULL;','/* keep stale pointer */','single positive commits terminal and pending payload'),
 ('nonce-reuse','pool','c->nonce = ++b->next_nonce;','c->nonce = b->next_nonce?b->next_nonce:++b->next_nonce;','return enables fresh monotone lease'),
 ('pending-return','pool','receipt_cell(b, p, r, LMEMBER_LEASED, &c)','receipt_cell(b, p, r, LMEMBER_PENDING, &c)','pending payload cannot return'),
]

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def stripped(s):return re.sub(r'^#include[^\n]*\n','',s,flags=re.M)
def main():
 resources=subprocess.check_output([str(R/'work/oct5-as-r9/astra-yip.sh'),'resources'],text=True);assert re.search(r'^mac +HELD by you',resources,re.M)
 st=os.statvfs(R);assert st.f_bavail*st.f_frsize>=8*1024**3
 out=D/('controls-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));out.mkdir()
 modules=['operation','rpc','admission','io','provided'];parts={n:stripped((D/f'kernel/loom_service_{n}.c').read_text()) for n in modules}
 parts['fixed']='\n'.join(extract.fun((R/'kernel/loom.c').read_text(),n) for n in ['loom_private_extent_locked','loom_private_fixed_acquire','loom_private_fixed_release'])
 parts['pool']=stripped((R/'kernel/loom_service_pool.c').read_text())
 parts['allocator']=extract.fun((R/'kernel/9p_client.c').read_text(),'p9_client_alloc_fid')
 linked=[W/'kernel/loom_service_wire.c',P/'kernel/loom_service_program.c']+[R/n for n in ['kernel/perm.c','kernel/9p_session.c','kernel/9p_wire.c','kernel/9p_transport.c','kernel/loom_service_reply.c','kernel/loom_service_requests.c','kernel/loom_service_slots.c','kernel/loom_service_nodes.c','kernel/loom_service_fids.c','kernel/loom_service_pool_table.c','kernel/loom_service_decode.c']]
 paths=[*D.glob('kernel/**/*.c'),*D.glob('kernel/**/*.h'),D/'fixture.c',D/'run-controls.py',P/'host/thylacine/spinlock.h',R/'kernel/loom_service_pool.c',R/'kernel/loom.c',R/'kernel/9p_client.c',R/'work/oct5-as-r9/final-reclaim-control.py',*linked]
 hashes={str(p.relative_to(R)):sha(p) for p in paths};fixture=(D/'fixture.c').read_text();results=[]
 def assembled(parts):
  s=fixture
  for n,marker in [('operation','CONSTRUCTOR'),('rpc','RPC'),('admission','ADMISSION'),('io','IO'),('provided','PROVIDED'),('pool','POOL'),('fixed','FIXED'),('allocator','FID_ALLOCATOR')]:s=s.replace('/* ACTUAL_'+marker+' */',parts[n])
  return s
 original=assembled(parts);sdk=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()
 for name,unit,old,new,want in [('control',None,None,None,None)]+M:
  variant=dict(parts)
  if unit:
   assert variant[unit].count(old)==1,(name,variant[unit].count(old));variant[unit]=variant[unit].replace(old,new)
  source=assembled(variant);p=out/(name+'.c');p.write_text(source);exe=out/(name+'.exe')
  (out/(name+'.diff')).write_text(''.join(difflib.unified_diff(original.splitlines(True),source.splitlines(True),fromfile='control.c',tofile=name+'.c')))
  cmd=['/opt/homebrew/opt/llvm@22/bin/clang','-std=c11','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-g','-fsanitize=address,undefined','-isysroot',sdk,'-I'+str(P/'host'),'-I'+str(D/'kernel/include'),'-I'+str(P/'kernel/include'),'-I'+str(W/'kernel/include'),'-I'+str(R/'kernel/include'),str(p),*[str(p) for p in linked],'-o',str(exe)]
  (out/(name+'-command.json')).write_text(json.dumps(cmd)+'\n')
  build=subprocess.run(cmd,capture_output=True);(out/(name+'-build.log')).write_bytes(build.stdout+build.stderr);assert not build.returncode,(name,build.stderr.decode())
  run=subprocess.run([str(exe)],capture_output=True,timeout=20);(out/(name+'.log')).write_bytes(run.stdout+run.stderr)
  assert (run.returncode==1 and run.stderr==f'FAIL {want}\n'.encode()) if want else (run.returncode==0 and not run.stderr),(name,run.returncode,run.stdout,run.stderr)
  results.append(dict(case=name,expected=want,rc=run.returncode,source_sha256=sha(p),binary_sha256=sha(exe)));print('PASS',name,want or run.stdout.decode().strip(),flush=True)
 assert hashes=={str(p.relative_to(R)):sha(p) for p in paths}
 (out/'result.json').write_text(json.dumps(dict(inputs=hashes,results=results,limits='Synthetic parsed dispatch replies; no real submit/wire. Actual CQ/receipt publication, pool/slot/request/FID ledger, capture and program transitions. Registration physical pins are fixture boundaries. Whole-ring pending discard and real READY driver remain pending. Real data headers and actual SQE preparation, fixed helper, constructor, FID allocator and ledger/program code. Host address translation, allocation, Proc snapshot, refs/wake/engine and physical backing drop boundaries. No wire dispatch; admitted destructor is manual fixture cleanup. No native frontend/driver/close/concurrency proof.'),indent=2)+'\n');print(out)
if __name__=='__main__':main()
