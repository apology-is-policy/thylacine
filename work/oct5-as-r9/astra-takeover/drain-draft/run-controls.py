from pathlib import Path
import subprocess,json,hashlib,datetime,re,os,difflib,importlib.util
D=Path(__file__).resolve().parent;R=D.parents[3];T=D.parent
W=T/'wire-draft';P=T/'program-draft';O=T/'retirement-draft';S=T/'ready-draft';A=T/'action-draft';U=T/'iounit-draft'
spec=importlib.util.spec_from_file_location('extract',R/'work/oct5-as-r9/final-reclaim-control.py');extract=importlib.util.module_from_spec(spec);spec.loader.exec_module(extract)
M=[('fresh_io_deadline', 'action', 'if(!reason && o->phase!=LRPC_TERMINAL && r->deadline && r->deadline<=now)', 'if(false && !reason && o->phase!=LRPC_TERMINAL && r->deadline && r->deadline<=now)', 'fresh IO deadline prevents another bounded wire copy'), ('ready_root', 'engine', 'loom_ref(l);int rc=engine_ready_visit(l,now);loom_unref(l);return rc;', 'int rc=engine_ready_visit(l,now);return rc;', 'engine driver retains independent pin and root through free'), ('close_root', 'engine', 'loom_ref(l);int rc=engine_close_visit(l);loom_unref(l);return rc;', 'int rc=engine_close_visit(l);return rc;', 'engine driver retains independent pin and root through free'), ('close_pin', 'engine', '++e->pins;p=e->protocol;ref=p->scope;handshake=a.handshake;break;', 'p=e->protocol;ref=p->scope;handshake=a.handshake;break;', '!close driver lost entry pin'), ('wait_epoch', 'engine', 'return t->wait_work || t->wait_change!=__atomic_load_n(&t->change,__ATOMIC_ACQUIRE);', 'return t->wait_work;', 'change during registration forces recheck'), ('general_deadline', 'engine', 'a.step=ready.work;a.events=ready.events;a.deadline=ready.deadline;', 'a.step=ready.work;a.events=ready.events;a.deadline=0;', 'active operation deadline replaces expired CONNECT deadline'), ('close_retired', 'engine', 'if(!a.step && !a.retire)continue;', 'if(e->protocol->locally_retired || (!a.step && !a.retire))continue;', 'close revisits retired scope to discard pending owner'), ('early_retirement', 'engine', '    u32 count=0;\n    for(struct loom_service_operation *o=p->op_head;o;o=o->next)', '    if(p->rpc_quiesced)return true;\n    u32 count=0;\n    for(struct loom_service_operation *o=p->op_head;o;o=o->next)', 'quiescence alone cannot retire local operation borrow'), ('clunk_cancel_spin', 'ready', '                action=LREADY_CANCEL;', '                action=LREADY_CANCEL;\n            else action=LREADY_CANCEL;', 'known CLUNK borrower shortage parks cancellation'), ('clunk_close_spin', 'ready', '                action=LREADY_DISCARD;', '                action=LREADY_DISCARD;\n            else action=LREADY_DISCARD;', 'known CLUNK borrower shortage parks close'), ('handshake_quiescence', 'scope_abort', 'p->rpc_quiesced=true; // actual cursor abort returned; no operation escaped this handshake', 'p->rpc_quiesced=false; // fault: lost actual quiescence', 'active CONNECT still routes deadline abort and collection'), ('completed_handshake_deadline', 'engine', 'a.step=ready.work;a.events=ready.events;a.deadline=ready.deadline;', 'a.step=ready.work;a.events=ready.events;a.deadline=p->connect_deadline;', 'active operation deadline replaces expired CONNECT deadline')]

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def stripped(s):return re.sub(r'^#include[^\n]*\n','',s,flags=re.M)
def main():
 resources=subprocess.check_output([str(R/'work/oct5-as-r9/astra-yip.sh'),'resources'],text=True);assert re.search(r'^mac +HELD by you',resources,re.M)
 st=os.statvfs(R);assert st.f_bavail*st.f_frsize>=8*1024**3
 out=D/('controls-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));out.mkdir()
 modules=['operation','rpc','admission','io','provided','retirement'];parts={n:stripped((O/f'kernel/loom_service_{n}.c').read_text()) for n in modules}
 parts['admission']=stripped((U/'kernel/loom_service_admission.c').read_text())
 parts['nodes']=stripped((U/'kernel/loom_service_nodes.c').read_text())
 parts['fids']=stripped((U/'kernel/loom_service_fids.c').read_text())
 parts['wire']=stripped((W/'kernel/loom_service_wire.c').read_text())
 parts['fixed']='\n'.join(extract.fun((R/'kernel/loom.c').read_text(),n) for n in ['loom_private_extent_locked','loom_private_fixed_acquire','loom_private_fixed_release'])
 parts['pool_destroy']=extract.fun((O/'kernel/loom.c').read_text(),'loom_private_pools_destroy')
 parts['terminal']=extract.fun((R/'kernel/loom.c').read_text(),'loom_private_post_terminal')
 parts['abort']='\n'.join(extract.fun((O/'kernel/9p_client.c').read_text(),n) for n in ['client_mark_dead_locked','p9_client_progress_abort'])
 parts['submit']=extract.fun((O/'kernel/9p_client.c').read_text(),'p9_client_submit_async')
 parts['scope_abort']='\n'.join(extract.fun((D/'kernel/loom_service_connect.c').read_text(),n) for n in ['connect_same','connect_scope','connect_request','connect_latch','loom_private_connect_abort','connect_step','loom_private_connect_step','loom_private_connect_step_retiring','loom_private_connect_release_retiring'])
 parts['ready']=stripped((D/'kernel/loom_service_ready.c').read_text())
 parts['engine']=stripped((D/'kernel/loom_service_engine.c').read_text()).replace('int loom_engine_get(', 'int actual_engine_get(').replace('int loom_engine_put(', 'int actual_engine_put(')
 parts['action']=stripped((D/'kernel/loom_service_action.c').read_text())
 parts['pool']=stripped((R/'kernel/loom_service_pool.c').read_text())
 parts['allocator']=extract.fun((R/'kernel/9p_client.c').read_text(),'p9_client_alloc_fid')
 linked=[W/'kernel/loom_service_wire.c',P/'kernel/loom_service_program.c']+[R/n for n in ['kernel/perm.c','kernel/9p_session.c','kernel/9p_wire.c','kernel/9p_transport.c','kernel/loom_service_reply.c','kernel/loom_service_slots.c','kernel/loom_service_nodes.c','kernel/loom_service_fids.c','kernel/loom_service_pool_table.c','kernel/loom_service_decode.c']]
 linked=[D/('kernel/'+p.name) if p.name in ['loom_service_nodes.c','loom_service_fids.c'] else p for p in linked]
 linked=[p for p in linked if p.name not in ['loom_service_nodes.c','loom_service_fids.c','loom_service_wire.c']]
 linked.append(O/'kernel/loom_service_requests.c')
 paths=[*U.glob('kernel/**/*.c'),*U.glob('kernel/**/*.h'),W/'kernel/loom_service_wire.c',*A.glob('kernel/**/*.c'),*A.glob('kernel/**/*.h'),*O.glob('kernel/**/*.c'),*O.glob('kernel/**/*.h'),*S.glob('kernel/**/*.c'),*S.glob('kernel/**/*.h'),R/'kernel/loom_service_connect.c',*D.glob('kernel/**/*.c'),*D.glob('kernel/**/*.h'),D/'fixture.c',D/'run-controls.py',P/'host/thylacine/spinlock.h',R/'kernel/loom_service_engine.c',R/'kernel/loom_service_pool.c',R/'kernel/loom.c',R/'kernel/9p_client.c',R/'work/oct5-as-r9/final-reclaim-control.py',*linked]
 hashes={str(p.relative_to(R)):sha(p) for p in paths};fixture=(D/'fixture.c').read_text();results=[]
 def assembled(parts):
  s=fixture
  for n,marker in [('engine','ENGINE'),('nodes','NODES'),('fids','FIDS'),('wire','WIRE'),('operation','CONSTRUCTOR'),('rpc','RPC'),('admission','ADMISSION'),('io','IO'),('provided','PROVIDED'),('retirement','RETIREMENT'),('abort','ABORT'),('terminal','TERMINAL'),('pool_destroy','POOL_DESTROY'),('pool','POOL'),('fixed','FIXED'),('allocator','FID_ALLOCATOR'),('submit','SUBMIT'),('scope_abort','SCOPE_ABORT'),('ready','READY'),('action','ACTION')]:s=s.replace('/* ACTUAL_'+marker+' */',parts[n])
  return s
 for name,unit,old,new,want in M:assert parts[unit].count(old)==1,(name,parts[unit].count(old))
 original=assembled(parts);sdk=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()
 for name,unit,old,new,want in [('control',None,None,None,None)]+M:
  st=os.statvfs(R);assert st.f_bavail*st.f_frsize>=8*1024**3
  variant=dict(parts)
  if unit:
   assert variant[unit].count(old)==1,(name,variant[unit].count(old));variant[unit]=variant[unit].replace(old,new)
  source=assembled(variant);p=out/(name+'.c');p.write_text(source);exe=out/(name+'.exe')
  (out/(name+'.diff')).write_text(''.join(difflib.unified_diff(original.splitlines(True),source.splitlines(True),fromfile='control.c',tofile=name+'.c')))
  cmd=['/opt/homebrew/opt/llvm@22/bin/clang','-std=c11','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-g','-fsanitize=address,undefined','-isysroot',sdk,'-I'+str(P/'host'),'-I'+str(D/'kernel/include'),'-I'+str(U/'kernel/include'),'-I'+str(A/'kernel/include'),'-I'+str(S/'kernel/include'),'-I'+str(O/'kernel/include'),'-I'+str(P/'kernel/include'),'-I'+str(W/'kernel/include'),'-I'+str(R/'kernel/include'),str(p),*[str(p) for p in linked],'-o',str(exe)]
  (out/(name+'-command.json')).write_text(json.dumps(cmd)+'\n')
  build=subprocess.run(cmd,capture_output=True);(out/(name+'-build.log')).write_bytes(build.stdout+build.stderr);assert not build.returncode,(name,build.stderr.decode())
  run=subprocess.run([str(exe)],capture_output=True,timeout=20);(out/(name+'.log')).write_bytes(run.stdout+run.stderr)
  expected_rc=86 if want and want.startswith('!') else 1
  expected_error=('INVARIANT '+want[1:] if expected_rc==86 else 'FAIL '+want)+'\n' if want else ''
  assert (run.returncode==expected_rc and run.stderr==expected_error.encode()) if want else (run.returncode==0 and not run.stderr),(name,run.returncode,run.stdout,run.stderr)
  results.append(dict(case=name,expected=want,rc=run.returncode,source_sha256=sha(p),binary_sha256=sha(exe)));print('PASS',name,want or run.stdout.decode().strip(),flush=True)
 assert hashes=={str(p.relative_to(R)):sha(p) for p in paths}
 (out/'result.json').write_text(json.dumps(dict(inputs=hashes,results=results,limits='Drain fixture composes actual engine get/put/visit/close/wait/collect and actual CONNECT step/release with operation action stack. Handshake install and native poll hooks/protocol destruction remain host boundaries. Earlier cases retain legacy fake engine roots. Action executor uses actual submit, scope abort, owner/program/admission/retirement/selector. Progress step and successful parsed responses remain explicit controlled boundaries in new action cases. Separate external driver pin/root is modeled and asserted. Actual client-mark-dead/progress-abort, transport/session/codec abort, per-owner close and physical operation destructor, final pool-destruction helper, request/FID/pool/CQ transitions. Parsed successful replies remain synthetic; prior preparation regressions never submit. Host native teardown, translation, allocator, image snapshot, ref/engine/wake and physical backing drop boundaries. Earlier preparation/provided cases retain manual fixture destruction; new retirement cases use actual destructor. Final pool helper is called at simulated zero Loom references, not actual retirer. No native READY/frontend/close/concurrency qualification.'),indent=2)+'\n');print(out)
if __name__=='__main__':main()
