from pathlib import Path
import subprocess,json,hashlib,datetime,re,os,difflib,importlib.util
D=Path(__file__).resolve().parent;R=D.parents[3];T=D.parent
W=T/'wire-draft';P=T/'program-draft';O=T/'retirement-draft';S=T/'ready-draft'
spec=importlib.util.spec_from_file_location('extract',R/'work/oct5-as-r9/final-reclaim-control.py');extract=importlib.util.module_from_spec(spec);spec.loader.exec_module(extract)
M=[('full-cq', 'ready', 'bool wire=room && !reason', 'bool wire=!reason', 'full CQ blocks fresh submission'), ('fresh-begin', 'action', 'rc=o->pool_stream?loom_provided_begin_locked(o,timer_now_ns()):\n                          loom_admission_begin_locked(o,timer_now_ns());', 'rc=o->pool_stream?loom_provided_begin_locked(o,0):\n                          loom_admission_begin_locked(o,0);', 'fresh begin deadline refuses wire'), ('fresh-commit', 'action', 'rc=o->pool_stream?loom_provided_commit_locked(o,timer_now_ns()):\n                          loom_admission_commit_locked(o,timer_now_ns());', 'rc=o->pool_stream?loom_provided_commit_locked(o,0):\n                          loom_admission_commit_locked(o,0);', 'fresh commit deadline after physical drop'), ('fresh-rearm', 'action', 'rc=loom_rpc_rearm_read_locked(o,timer_now_ns());', 'rc=loom_rpc_rearm_read_locked(o,0);', 'fresh rearm deadline bars new shot'), ('consumed-requeue', 'action', 'if(rc && loom_rpc_settle_locked(o))extinction("READY requeue settle lost");', '(void)rc;', 'failed fresh requeue settles consumed attempt'), ('quiescence', 'action', 'rc=loom_retirement_quiesce(p);return rc?rc:1;', 'return 1;', 'executor actually quiesces'), ('commit-fixed-drop', 'action', 'case LREADY_COMMIT:\n        if(o->program.result<0)return action_cancel(o,o->program.result);\n        if(o->fixed.owner || o->fixed_dropping)', 'case LREADY_COMMIT:\n        if(o->program.result<0)return action_cancel(o,o->program.result);\n        if(false)', 'executor commit follows physical fixed drop'), ('cancel-busy', 'action', 'if(!rc && o->pool_busy)rc=loom_provided_release_busy_locked(o);', '(void)rc;', 'remote AGAIN isolated terminal'), ('submit', 'action', '(void)loom_rpc_submit(o);return 1;', '(void)o;return 1;', 'executor submits actual frame'), ('driver-release', 'action', 'rc=action_execute(p,s);', 'p->driving=false;rc=action_execute(p,s);', 'executor submits actual frame'), ('return-wake', 'action', 'loom_private_close_changed(l);\n    return rc==', '/* lost return wake */\n    return rc==', 'transition return wakes executor'), ('clunk-quiescence', 'action', 'if(o->fid_claim==P9_TCLUNK && !p->rpc_quiesced)return action_abort(p,reason);', '/* missing CLUNK quiescence */', 'CLUNK failure cannot retire before scope quiescence'), ('more-order', 'action', 'rc=loom_provided_deliver(o);return rc?rc:1;', 'rc=loom_retirement_post_terminal(o);return rc?rc:1;', 'executor MORE delivered before negative terminal'), ('malformed-scope', 'action', 'if(next==LPROGRAM_ABORT)return action_abort(p,-T_E_IO);', '/* lost malformed scope abort */', 'malformed reply settles and aborts scope'), ('io-abort', 'action', 'return rc<0?action_abort(p,rc):rc>0?1:0;', 'return rc>0?1:0;', 'IO failure latches scope before writer proof')]

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def stripped(s):return re.sub(r'^#include[^\n]*\n','',s,flags=re.M)
def main():
 resources=subprocess.check_output([str(R/'work/oct5-as-r9/astra-yip.sh'),'resources'],text=True);assert re.search(r'^mac +HELD by you',resources,re.M)
 st=os.statvfs(R);assert st.f_bavail*st.f_frsize>=8*1024**3
 out=D/('controls-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));out.mkdir()
 modules=['operation','rpc','admission','io','provided','retirement'];parts={n:stripped((O/f'kernel/loom_service_{n}.c').read_text()) for n in modules}
 parts['fixed']='\n'.join(extract.fun((R/'kernel/loom.c').read_text(),n) for n in ['loom_private_extent_locked','loom_private_fixed_acquire','loom_private_fixed_release'])
 parts['pool_destroy']=extract.fun((R/'kernel/loom_service_engine.c').read_text(),'loom_engine_empty')+'\n'+extract.fun((O/'kernel/loom.c').read_text(),'loom_private_pools_destroy')
 parts['terminal']=extract.fun((R/'kernel/loom.c').read_text(),'loom_private_post_terminal')
 parts['abort']='\n'.join(extract.fun((O/'kernel/9p_client.c').read_text(),n) for n in ['client_mark_dead_locked','p9_client_progress_abort'])
 parts['submit']=extract.fun((O/'kernel/9p_client.c').read_text(),'p9_client_submit_async')
 parts['scope_abort']='\n'.join(extract.fun((R/'kernel/loom_service_connect.c').read_text(),n) for n in ['connect_scope','connect_latch','loom_private_connect_abort'])
 parts['ready']=stripped((D/'kernel/loom_service_ready.c').read_text())
 parts['action']=stripped((D/'kernel/loom_service_action.c').read_text())
 parts['pool']=stripped((R/'kernel/loom_service_pool.c').read_text())
 parts['allocator']=extract.fun((R/'kernel/9p_client.c').read_text(),'p9_client_alloc_fid')
 linked=[W/'kernel/loom_service_wire.c',P/'kernel/loom_service_program.c']+[R/n for n in ['kernel/perm.c','kernel/9p_session.c','kernel/9p_wire.c','kernel/9p_transport.c','kernel/loom_service_reply.c','kernel/loom_service_slots.c','kernel/loom_service_nodes.c','kernel/loom_service_fids.c','kernel/loom_service_pool_table.c','kernel/loom_service_decode.c']]
 linked.append(O/'kernel/loom_service_requests.c')
 paths=[*O.glob('kernel/**/*.c'),*O.glob('kernel/**/*.h'),*S.glob('kernel/**/*.c'),*S.glob('kernel/**/*.h'),R/'kernel/loom_service_connect.c',*D.glob('kernel/**/*.c'),*D.glob('kernel/**/*.h'),D/'fixture.c',D/'run-controls.py',P/'host/thylacine/spinlock.h',R/'kernel/loom_service_engine.c',R/'kernel/loom_service_pool.c',R/'kernel/loom.c',R/'kernel/9p_client.c',R/'work/oct5-as-r9/final-reclaim-control.py',*linked]
 hashes={str(p.relative_to(R)):sha(p) for p in paths};fixture=(D/'fixture.c').read_text();results=[]
 def assembled(parts):
  s=fixture
  for n,marker in [('operation','CONSTRUCTOR'),('rpc','RPC'),('admission','ADMISSION'),('io','IO'),('provided','PROVIDED'),('retirement','RETIREMENT'),('abort','ABORT'),('terminal','TERMINAL'),('pool_destroy','POOL_DESTROY'),('pool','POOL'),('fixed','FIXED'),('allocator','FID_ALLOCATOR'),('submit','SUBMIT'),('scope_abort','SCOPE_ABORT'),('ready','READY'),('action','ACTION')]:s=s.replace('/* ACTUAL_'+marker+' */',parts[n])
  return s
 for name,unit,old,new,want in M:assert parts[unit].count(old)==1,(name,parts[unit].count(old))
 original=assembled(parts);sdk=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()
 for name,unit,old,new,want in [('control',None,None,None,None)]+M:
  variant=dict(parts)
  if unit:
   assert variant[unit].count(old)==1,(name,variant[unit].count(old));variant[unit]=variant[unit].replace(old,new)
  source=assembled(variant);p=out/(name+'.c');p.write_text(source);exe=out/(name+'.exe')
  (out/(name+'.diff')).write_text(''.join(difflib.unified_diff(original.splitlines(True),source.splitlines(True),fromfile='control.c',tofile=name+'.c')))
  cmd=['/opt/homebrew/opt/llvm@22/bin/clang','-std=c11','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-g','-fsanitize=address,undefined','-isysroot',sdk,'-I'+str(P/'host'),'-I'+str(D/'kernel/include'),'-I'+str(S/'kernel/include'),'-I'+str(O/'kernel/include'),'-I'+str(P/'kernel/include'),'-I'+str(W/'kernel/include'),'-I'+str(R/'kernel/include'),str(p),*[str(p) for p in linked],'-o',str(exe)]
  (out/(name+'-command.json')).write_text(json.dumps(cmd)+'\n')
  build=subprocess.run(cmd,capture_output=True);(out/(name+'-build.log')).write_bytes(build.stdout+build.stderr);assert not build.returncode,(name,build.stderr.decode())
  run=subprocess.run([str(exe)],capture_output=True,timeout=20);(out/(name+'.log')).write_bytes(run.stdout+run.stderr)
  assert (run.returncode==1 and run.stderr==f'FAIL {want}\n'.encode()) if want else (run.returncode==0 and not run.stderr),(name,run.returncode,run.stdout,run.stderr)
  results.append(dict(case=name,expected=want,rc=run.returncode,source_sha256=sha(p),binary_sha256=sha(exe)));print('PASS',name,want or run.stdout.decode().strip(),flush=True)
 assert hashes=={str(p.relative_to(R)):sha(p) for p in paths}
 (out/'result.json').write_text(json.dumps(dict(inputs=hashes,results=results,limits='Action executor uses actual submit, scope abort, owner/program/admission/retirement/selector. Progress step and successful parsed responses remain explicit controlled boundaries in new action cases. Separate external driver pin/root is modeled and asserted. Actual client-mark-dead/progress-abort, transport/session/codec abort, per-owner close and physical operation destructor, final pool-destruction helper, request/FID/pool/CQ transitions. Parsed successful replies remain synthetic; prior preparation regressions never submit. Host native teardown, translation, allocator, image snapshot, ref/engine/wake and physical backing drop boundaries. Earlier preparation/provided cases retain manual fixture destruction; new retirement cases use actual destructor. Final pool helper is called at simulated zero Loom references, not actual retirer. No native READY/frontend/close/concurrency qualification.'),indent=2)+'\n');print(out)
if __name__=='__main__':main()
