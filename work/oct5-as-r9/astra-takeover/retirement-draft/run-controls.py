from pathlib import Path
import subprocess,json,hashlib,datetime,re,os,difflib,importlib.util
D=Path(__file__).resolve().parent;R=D.parents[3];T=D.parent
W=T/'wire-draft';P=T/'program-draft'
spec=importlib.util.spec_from_file_location('extract',R/'work/oct5-as-r9/final-reclaim-control.py');extract=importlib.util.module_from_spec(spec);spec.loader.exec_module(extract)
M=[
 ('abort-tx-owner','abort','if (sending) sending->sending = false;','(void)sending;','TX owner ends before abort callback'),
 ('abort-early-reply','abort','if (sending) sending->sending = false;','if (sending && c->inflight[0]==sending) sending->sending = false;','abort releases early-replied TX owner too'),
 ('premature-quiescence','retirement','p9_client_progress_abort(&p->client);','p->rpc_quiesced=true; p9_client_progress_abort(&p->client);','transport abort precedes quiescence'),
 ('queued-quiescence','retirement','!p->rpc_quiesced || p->abort_reason>=0 || o->phase==LRPC_NEW','p->abort_reason>=0 || o->phase==LRPC_NEW','queued close independently requires client quiescence'),
 ('ring-close','retirement','!l->service_closing || !l->service_slots->closing || !p->driving ||','!l->service_slots->closing || !p->driving ||','ring close latch required'),
 ('slot-close','retirement','!l->service_closing || !l->service_slots->closing || !p->driving ||','!l->service_closing || !p->driving ||','slot close latch required'),
 ('scope-is-not-ring','provided','!l->service_closing || !l->service_slots->closing || !p->driving ||','!l || !p->driving ||','scope abort cannot discard pending success'),
 ('pending-borrow-cycle','admission','if(!o || reason>=0)return -T_E_INVAL;\n    if(o->fixed.owner || o->fixed_dropping || o->pool_busy)return -T_E_BUSY;','if(!o || reason>=0)return -T_E_INVAL;\n    if(o->fixed.owner || o->fixed_dropping || o->pool_busy || o->pool_pending)return -T_E_BUSY;','abort commits terminal without CQ-dependent local borrow'),
 ('terminal-overtakes-more','retirement','o->phase!=LRPC_TERMINAL || o->pool_pending ||','o->phase!=LRPC_TERMINAL ||','terminal waits behind committed MORE'),
 ('more-after-terminal','provided','} else if(!o->multishot || (o->phase!=LRPC_SETTLING && o->phase!=LRPC_TERMINAL))','} else if(!o->multishot || o->phase!=LRPC_SETTLING)','settled abort delivers MORE but cannot rearm'),
 ('refund-before-free','operation','kfree(op);\n    operation_refund(l);\n    spin_lock(&l->lock);','operation_refund(l);\n    kfree(op);\n    spin_lock(&l->lock);','refund after actual storage free'),
 ('owner-before-free','operation','kfree(op);\n    operation_refund(l);\n    spin_lock(&l->lock);','loom_request_drop_owner(&l->service_requests,request);\n    kfree(op);\n    operation_refund(l);\n    spin_lock(&l->lock);','physical owner retained through kfree'),
 ('owner-before-refund','operation','kfree(op);\n    operation_refund(l);\n    spin_lock(&l->lock);','kfree(op);\n    loom_request_drop_owner(&l->service_requests,request);\n    operation_refund(l);\n    spin_lock(&l->lock);','physical owner retained through refund'),
 ('final-live-reference','pool_destroy','__atomic_load_n(&l->refcount,__ATOMIC_ACQUIRE) || !l->service_closing ||','!l->service_closing ||','live ring cannot discard pool leases'),
 ('final-physical-pin','pool_destroy','for(u32 i=0;i<count;++i)loom_private_drop_buffer(l->service_as,pins[i]);','for(u32 i=0;i<count;++i)(void)pins[i];','final exclusive destruction physically returns every member pin'),
]

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def stripped(s):return re.sub(r'^#include[^\n]*\n','',s,flags=re.M)
def main():
 resources=subprocess.check_output([str(R/'work/oct5-as-r9/astra-yip.sh'),'resources'],text=True);assert re.search(r'^mac +HELD by you',resources,re.M)
 st=os.statvfs(R);assert st.f_bavail*st.f_frsize>=8*1024**3
 out=D/('controls-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));out.mkdir()
 modules=['operation','rpc','admission','io','provided','retirement'];parts={n:stripped((D/f'kernel/loom_service_{n}.c').read_text()) for n in modules}
 parts['fixed']='\n'.join(extract.fun((R/'kernel/loom.c').read_text(),n) for n in ['loom_private_extent_locked','loom_private_fixed_acquire','loom_private_fixed_release'])
 parts['pool_destroy']=extract.fun((R/'kernel/loom_service_engine.c').read_text(),'loom_engine_empty')+'\n'+extract.fun((D/'kernel/loom.c').read_text(),'loom_private_pools_destroy')
 parts['terminal']=extract.fun((R/'kernel/loom.c').read_text(),'loom_private_post_terminal')
 parts['abort']='\n'.join(extract.fun((D/'kernel/9p_client.c').read_text(),n) for n in ['client_mark_dead_locked','p9_client_progress_abort'])
 parts['pool']=stripped((R/'kernel/loom_service_pool.c').read_text())
 parts['allocator']=extract.fun((R/'kernel/9p_client.c').read_text(),'p9_client_alloc_fid')
 linked=[W/'kernel/loom_service_wire.c',P/'kernel/loom_service_program.c']+[R/n for n in ['kernel/perm.c','kernel/9p_session.c','kernel/9p_wire.c','kernel/9p_transport.c','kernel/loom_service_reply.c','kernel/loom_service_slots.c','kernel/loom_service_nodes.c','kernel/loom_service_fids.c','kernel/loom_service_pool_table.c','kernel/loom_service_decode.c']]
 linked.append(D/'kernel/loom_service_requests.c')
 paths=[*D.glob('kernel/**/*.c'),*D.glob('kernel/**/*.h'),D/'fixture.c',D/'run-controls.py',P/'host/thylacine/spinlock.h',R/'kernel/loom_service_engine.c',R/'kernel/loom_service_pool.c',R/'kernel/loom.c',R/'kernel/9p_client.c',R/'work/oct5-as-r9/final-reclaim-control.py',*linked]
 hashes={str(p.relative_to(R)):sha(p) for p in paths};fixture=(D/'fixture.c').read_text();results=[]
 def assembled(parts):
  s=fixture
  for n,marker in [('operation','CONSTRUCTOR'),('rpc','RPC'),('admission','ADMISSION'),('io','IO'),('provided','PROVIDED'),('retirement','RETIREMENT'),('abort','ABORT'),('terminal','TERMINAL'),('pool_destroy','POOL_DESTROY'),('pool','POOL'),('fixed','FIXED'),('allocator','FID_ALLOCATOR')]:s=s.replace('/* ACTUAL_'+marker+' */',parts[n])
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
 (out/'result.json').write_text(json.dumps(dict(inputs=hashes,results=results,limits='Actual client-mark-dead/progress-abort, transport/session/codec abort, per-owner close and physical operation destructor, final pool-destruction helper, request/FID/pool/CQ transitions. Parsed successful replies remain synthetic; submission forbidden. Host native teardown, translation, allocator, image snapshot, ref/engine/wake and physical backing drop boundaries. Earlier preparation/provided cases retain manual fixture destruction; new retirement cases use actual destructor. Final pool helper is called at simulated zero Loom references, not actual retirer. No native READY/frontend/close/concurrency qualification.'),indent=2)+'\n');print(out)
if __name__=='__main__':main()
