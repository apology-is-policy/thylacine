from pathlib import Path
import subprocess,json,hashlib,datetime,re,os,difflib,importlib.util
D=Path(__file__).resolve().parent;R=D.parents[3];T=D.parent
W=T/'wire-draft';P=T/'program-draft';O=T/'retirement-draft';S=T/'ready-draft';A=T/'action-draft'
spec=importlib.util.spec_from_file_location('extract',R/'work/oct5-as-r9/final-reclaim-control.py');extract=importlib.util.module_from_spec(spec);spec.loader.exec_module(extract)
M=[('open-retain', 'admission', 'if(!rc)l->service_nodes->nodes[o->source.slot].fid_iounit=o->reply.iounit;', 'if(!rc)l->service_nodes->nodes[o->source.slot].fid_iounit=0;', 'successful open atomically retains negotiated iounit'), ('write-limit', 'admission', 'if(kind==P9_TREAD || kind==P9_TWRITE)\n        o->program.wire.iounit', 'if(kind==P9_TREAD)\n        o->program.wire.iounit', 'write admission snapshots negotiated iounit'), ('read-limit', 'admission', 'if(kind==P9_TREAD || kind==P9_TWRITE)\n        o->program.wire.iounit', 'if(kind==P9_TWRITE)\n        o->program.wire.iounit', 'read admission snapshots negotiated iounit'), ('expire-publication', 'admission', 'int rc=loom_rpc_check_finish_locked(o,result,now);', 'if(o->fid_claim==P9_TLOPEN)o->owner->service_nodes->nodes[o->source.slot].fid_iounit=o->reply.iounit;\n    int rc=loom_rpc_check_finish_locked(o,result,now);', 'expired open publishes neither access nor iounit'), ('wire-limit', 'wire', 'if(w->iounit && w->iounit<limit)limit=w->iounit;', '/* ignored negotiated limit */', 'write wire respects negotiated limit and zero semantics'), ('zero-unbounded', 'wire', 'if(w->iounit && w->iounit<limit)limit=w->iounit;', 'if(w->iounit<limit)limit=w->iounit;', 'executor commit follows physical fixed drop'), ('msize-bound', 'wire', 'if(w->iounit && w->iounit<limit)limit=w->iounit;', 'if(w->iounit)limit=w->iounit;', 'iounit never widens negotiated frame budget'), ('reap-metadata', 'fids', 'for(size_t i=0;i<sizeof(*n);++i)bytes[i]=0;', 'u32 stale_iounit=n->fid_iounit;for(size_t i=0;i<sizeof(*n);++i)bytes[i]=0;n->fid_iounit=stale_iounit;', 'reap clears negotiated metadata before slot reuse'), ('mutable-wire-limit', 'action', '(void)loom_rpc_submit(o);return 1;', 'o->program.wire.iounit=l->service_nodes->nodes[o->source.slot].fid_iounit;\n        (void)loom_rpc_submit(o);return 1;', 'write wire respects negotiated limit and zero semantics'), ('rearm-limit', 'rpc', 'o->pool_more_delivered=false;\n    o->program.phase', 'o->pool_more_delivered=false;o->program.wire.iounit=0;\n    o->program.phase', 'rearmed provided read retains negotiated iounit'), ('full-unit', 'admission', 'if(!rc)l->service_nodes->nodes[o->source.slot].fid_iounit=o->reply.iounit;', 'if(!rc)l->service_nodes->nodes[o->source.slot].fid_iounit=(u16)o->reply.iounit;', 'successful open atomically retains negotiated iounit')]

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def stripped(s):return re.sub(r'^#include[^\n]*\n','',s,flags=re.M)
def main():
 resources=subprocess.check_output([str(R/'work/oct5-as-r9/astra-yip.sh'),'resources'],text=True);assert re.search(r'^mac +HELD by you',resources,re.M)
 st=os.statvfs(R);assert st.f_bavail*st.f_frsize>=8*1024**3
 out=D/('controls-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));out.mkdir()
 modules=['operation','rpc','admission','io','provided','retirement'];parts={n:stripped((O/f'kernel/loom_service_{n}.c').read_text()) for n in modules}
 parts['admission']=stripped((D/'kernel/loom_service_admission.c').read_text())
 parts['nodes']=stripped((D/'kernel/loom_service_nodes.c').read_text())
 parts['fids']=stripped((D/'kernel/loom_service_fids.c').read_text())
 parts['wire']=stripped((W/'kernel/loom_service_wire.c').read_text())
 parts['fixed']='\n'.join(extract.fun((R/'kernel/loom.c').read_text(),n) for n in ['loom_private_extent_locked','loom_private_fixed_acquire','loom_private_fixed_release'])
 parts['pool_destroy']=extract.fun((R/'kernel/loom_service_engine.c').read_text(),'loom_engine_empty')+'\n'+extract.fun((O/'kernel/loom.c').read_text(),'loom_private_pools_destroy')
 parts['terminal']=extract.fun((R/'kernel/loom.c').read_text(),'loom_private_post_terminal')
 parts['abort']='\n'.join(extract.fun((O/'kernel/9p_client.c').read_text(),n) for n in ['client_mark_dead_locked','p9_client_progress_abort'])
 parts['submit']=extract.fun((O/'kernel/9p_client.c').read_text(),'p9_client_submit_async')
 parts['scope_abort']='\n'.join(extract.fun((R/'kernel/loom_service_connect.c').read_text(),n) for n in ['connect_scope','connect_latch','loom_private_connect_abort'])
 parts['ready']=stripped((A/'kernel/loom_service_ready.c').read_text())
 parts['action']=stripped((A/'kernel/loom_service_action.c').read_text())
 parts['pool']=stripped((R/'kernel/loom_service_pool.c').read_text())
 parts['allocator']=extract.fun((R/'kernel/9p_client.c').read_text(),'p9_client_alloc_fid')
 linked=[W/'kernel/loom_service_wire.c',P/'kernel/loom_service_program.c']+[R/n for n in ['kernel/perm.c','kernel/9p_session.c','kernel/9p_wire.c','kernel/9p_transport.c','kernel/loom_service_reply.c','kernel/loom_service_slots.c','kernel/loom_service_nodes.c','kernel/loom_service_fids.c','kernel/loom_service_pool_table.c','kernel/loom_service_decode.c']]
 linked=[D/('kernel/'+p.name) if p.name in ['loom_service_nodes.c','loom_service_fids.c'] else p for p in linked]
 linked=[p for p in linked if p.name not in ['loom_service_nodes.c','loom_service_fids.c','loom_service_wire.c']]
 linked.append(O/'kernel/loom_service_requests.c')
 paths=[W/'kernel/loom_service_wire.c',*A.glob('kernel/**/*.c'),*A.glob('kernel/**/*.h'),*O.glob('kernel/**/*.c'),*O.glob('kernel/**/*.h'),*S.glob('kernel/**/*.c'),*S.glob('kernel/**/*.h'),R/'kernel/loom_service_connect.c',*D.glob('kernel/**/*.c'),*D.glob('kernel/**/*.h'),D/'fixture.c',D/'run-controls.py',P/'host/thylacine/spinlock.h',R/'kernel/loom_service_engine.c',R/'kernel/loom_service_pool.c',R/'kernel/loom.c',R/'kernel/9p_client.c',R/'work/oct5-as-r9/final-reclaim-control.py',*linked]
 hashes={str(p.relative_to(R)):sha(p) for p in paths};fixture=(D/'fixture.c').read_text();results=[]
 def assembled(parts):
  s=fixture
  for n,marker in [('nodes','NODES'),('fids','FIDS'),('wire','WIRE'),('operation','CONSTRUCTOR'),('rpc','RPC'),('admission','ADMISSION'),('io','IO'),('provided','PROVIDED'),('retirement','RETIREMENT'),('abort','ABORT'),('terminal','TERMINAL'),('pool_destroy','POOL_DESTROY'),('pool','POOL'),('fixed','FIXED'),('allocator','FID_ALLOCATOR'),('submit','SUBMIT'),('scope_abort','SCOPE_ABORT'),('ready','READY'),('action','ACTION')]:s=s.replace('/* ACTUAL_'+marker+' */',parts[n])
  return s
 for name,unit,old,new,want in M:assert parts[unit].count(old)==1,(name,parts[unit].count(old))
 original=assembled(parts);sdk=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()
 for name,unit,old,new,want in [('control',None,None,None,None)]+M:
  variant=dict(parts)
  if unit:
   assert variant[unit].count(old)==1,(name,variant[unit].count(old));variant[unit]=variant[unit].replace(old,new)
  source=assembled(variant);p=out/(name+'.c');p.write_text(source);exe=out/(name+'.exe')
  (out/(name+'.diff')).write_text(''.join(difflib.unified_diff(original.splitlines(True),source.splitlines(True),fromfile='control.c',tofile=name+'.c')))
  cmd=['/opt/homebrew/opt/llvm@22/bin/clang','-std=c11','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-g','-fsanitize=address,undefined','-isysroot',sdk,'-I'+str(P/'host'),'-I'+str(D/'kernel/include'),'-I'+str(A/'kernel/include'),'-I'+str(S/'kernel/include'),'-I'+str(O/'kernel/include'),'-I'+str(P/'kernel/include'),'-I'+str(W/'kernel/include'),'-I'+str(R/'kernel/include'),str(p),*[str(p) for p in linked],'-o',str(exe)]
  (out/(name+'-command.json')).write_text(json.dumps(cmd)+'\n')
  build=subprocess.run(cmd,capture_output=True);(out/(name+'-build.log')).write_bytes(build.stdout+build.stderr);assert not build.returncode,(name,build.stderr.decode())
  run=subprocess.run([str(exe)],capture_output=True,timeout=20);(out/(name+'.log')).write_bytes(run.stdout+run.stderr)
  assert (run.returncode==1 and run.stderr==f'FAIL {want}\n'.encode()) if want else (run.returncode==0 and not run.stderr),(name,run.returncode,run.stdout,run.stderr)
  results.append(dict(case=name,expected=want,rc=run.returncode,source_sha256=sha(p),binary_sha256=sha(exe)));print('PASS',name,want or run.stdout.decode().strip(),flush=True)
 assert hashes=={str(p.relative_to(R)):sha(p) for p in paths}
 (out/'result.json').write_text(json.dumps(dict(inputs=hashes,results=results,limits='Action executor uses actual submit, scope abort, owner/program/admission/retirement/selector. Progress step and successful parsed responses remain explicit controlled boundaries in new action cases. Separate external driver pin/root is modeled and asserted. Actual client-mark-dead/progress-abort, transport/session/codec abort, per-owner close and physical operation destructor, final pool-destruction helper, request/FID/pool/CQ transitions. Parsed successful replies remain synthetic; prior preparation regressions never submit. Host native teardown, translation, allocator, image snapshot, ref/engine/wake and physical backing drop boundaries. Earlier preparation/provided cases retain manual fixture destruction; new retirement cases use actual destructor. Final pool helper is called at simulated zero Loom references, not actual retirer. No native READY/frontend/close/concurrency qualification.'),indent=2)+'\n');print(out)
if __name__=='__main__':main()
