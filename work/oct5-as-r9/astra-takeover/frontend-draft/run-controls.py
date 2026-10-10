from pathlib import Path
import subprocess,json,hashlib,datetime,re,os,difflib,importlib.util
D=Path(__file__).resolve().parent;R=D.parents[3];T=D.parent
W=T/'wire-draft';P=T/'program-draft';O=T/'retirement-draft';S=T/'ready-draft';A=T/'action-draft';U=T/'iounit-draft';E=T/'drain-draft';V=T/'return-draft'
spec=importlib.util.spec_from_file_location('extract',R/'work/oct5-as-r9/final-reclaim-control.py');extract=importlib.util.module_from_spec(spec);spec.loader.exec_module(extract)
M=[('shared_reread', 'frontend', 'frontend_dispatch(l,p,&copy)', 'frontend_dispatch(l,p,(const struct loom_sqe *)(l->ring_kva+l->sqe_off+index*sizeof(copy)))', 'shared mutation cannot alter copied operation or correlation'), ('claim_serialization', 'frontend', 'if(e->submitting || e->rejected ||', 'if(e->rejected ||', 'nested frontend cannot consume a second SQE'), ('pending_reservation', 'frontend', 'if(e->submitting || e->rejected ||', 'if(e->submitting ||', '!frontend lost serialized claim'), ('image', 'frontend', 'p->as!=l->service_as || p->stripes!=l->service_creator', 'p->stripes!=l->service_creator', 'frontend exact creator image'), ('identity', 'frontend', 'p->as!=l->service_as || p->stripes!=l->service_creator', 'p->as!=l->service_as', 'frontend exact creator identity'), ('budget', 'frontend', 'if(budget>l->sq_entries)budget=l->sq_entries;', '/* fault: unbounded caller budget */', 'bad indices cannot defeat per-call submission bound'), ('sq_index', 'frontend', 'bool valid=index<l->sq_entries;', 'bool valid=index<=l->sq_entries;', 'bad SQ indirection bounded and counted without dispatch'), ('kick', 'frontend', '    loom_private_close_changed(l);\n    if(budget>', '    /* fault: omitted ENTER kick */\n    if(budget>', 'zero-submit CQ acknowledgement kicks registered owner'), ('receipt_clear', 'frontend', 'receipts[at]=(struct loom_service_buffer_receipt){0};', '(void)receipts;', 'refusal CQE clears old receipt before publication'), ('consume_rejection', 'frontend', 'e->rejected=false;e->rejected_user_data=0;e->rejected_result=0;', '/* fault: refusal remains eligible */', 'shape refusal consumes and posts one negative completion'), ('publish_deadline', 'frontend', 'loom_private_io_publish(o,timer_now_ns())', 'loom_private_io_publish(o,1)', 'late prepare retains refusal when CQ fills after consumption'), ('close_claim', 'engine', 'if(l->service_engine->submitting){spin_unlock(&l->lock);return 0;}', '/* fault: close outruns constructor */', 'close parks while copied frontend claim is active'), ('refusal_priority', 'engine', 'static int engine_ready_visit(struct Loom *l, u64 now) {', 'static int engine_ready_visit(struct Loom *l, u64 now) {\n    if(loom_frontend_step(l))return 1;', 'retained refusals cannot preempt every runnable scope turn')]

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
 parts['scope_abort']='\n'.join(extract.fun((E/'kernel/loom_service_connect.c').read_text(),n) for n in ['connect_same','connect_scope','connect_request','connect_latch','loom_private_connect_abort','connect_step','loom_private_connect_step','loom_private_connect_step_retiring','loom_private_connect_release_retiring'])
 parts['ready']=stripped((E/'kernel/loom_service_ready.c').read_text())
 parts['engine']=stripped((D/'kernel/loom_service_engine.c').read_text()).replace('int loom_engine_get(', 'int actual_engine_get(').replace('int loom_engine_put(', 'int actual_engine_put(')
 parts['action']=stripped((E/'kernel/loom_service_action.c').read_text())
 parts['control']='\n'.join(extract.fun((V/'kernel/loom_service_control.c').read_text(),n) for n in ['control_owner','control_exact','loom_private_service_return'])
 parts['frontend']=stripped((D/'kernel/loom_service_frontend.c').read_text())
 parts['pool']=stripped((R/'kernel/loom_service_pool.c').read_text())
 parts['allocator']=extract.fun((R/'kernel/9p_client.c').read_text(),'p9_client_alloc_fid')
 linked=[W/'kernel/loom_service_wire.c',P/'kernel/loom_service_program.c']+[R/n for n in ['kernel/perm.c','kernel/9p_session.c','kernel/9p_wire.c','kernel/9p_transport.c','kernel/loom_service_reply.c','kernel/loom_service_slots.c','kernel/loom_service_nodes.c','kernel/loom_service_fids.c','kernel/loom_service_pool_table.c','kernel/loom_service_decode.c']]
 linked=[D/('kernel/'+p.name) if p.name in ['loom_service_nodes.c','loom_service_fids.c'] else p for p in linked]
 linked=[p for p in linked if p.name not in ['loom_service_nodes.c','loom_service_fids.c','loom_service_wire.c']]
 linked.append(O/'kernel/loom_service_requests.c')
 paths=[*U.glob('kernel/**/*.c'),*U.glob('kernel/**/*.h'),W/'kernel/loom_service_wire.c',*A.glob('kernel/**/*.c'),*A.glob('kernel/**/*.h'),*O.glob('kernel/**/*.c'),*O.glob('kernel/**/*.h'),*S.glob('kernel/**/*.c'),*S.glob('kernel/**/*.h'),R/'kernel/loom_service_connect.c',*E.glob('kernel/**/*.c'),*E.glob('kernel/**/*.h'),*V.glob('kernel/**/*.c'),*V.glob('kernel/**/*.h'),*D.glob('kernel/**/*.c'),*D.glob('kernel/**/*.h'),D/'fixture.c',D/'run-controls.py',P/'host/thylacine/spinlock.h',R/'kernel/loom_service_engine.c',R/'kernel/loom_service_pool.c',R/'kernel/loom.c',R/'kernel/9p_client.c',R/'work/oct5-as-r9/final-reclaim-control.py',*linked]
 hashes={str(p.relative_to(R)):sha(p) for p in paths};fixture=(D/'fixture.c').read_text();results=[]
 def assembled(parts):
  s=fixture
  for n,marker in [('frontend','FRONTEND'),('control','CONTROL'),('engine','ENGINE'),('nodes','NODES'),('fids','FIDS'),('wire','WIRE'),('operation','CONSTRUCTOR'),('rpc','RPC'),('admission','ADMISSION'),('io','IO'),('provided','PROVIDED'),('retirement','RETIREMENT'),('abort','ABORT'),('terminal','TERMINAL'),('pool_destroy','POOL_DESTROY'),('pool','POOL'),('fixed','FIXED'),('allocator','FID_ALLOCATOR'),('submit','SUBMIT'),('scope_abort','SCOPE_ABORT'),('ready','READY'),('action','ACTION')]:s=s.replace('/* ACTUAL_'+marker+' */',parts[n])
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
  cmd=['/opt/homebrew/opt/llvm@22/bin/clang','-std=c11','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-g','-fsanitize=address,undefined','-isysroot',sdk,'-I'+str(P/'host'),'-I'+str(D/'kernel/include'),'-I'+str(V/'kernel/include'),'-I'+str(E/'kernel/include'),'-I'+str(U/'kernel/include'),'-I'+str(A/'kernel/include'),'-I'+str(S/'kernel/include'),'-I'+str(O/'kernel/include'),'-I'+str(P/'kernel/include'),'-I'+str(W/'kernel/include'),'-I'+str(R/'kernel/include'),str(p),*[str(p) for p in linked],'-o',str(exe)]
  (out/(name+'-command.json')).write_text(json.dumps(cmd)+'\n')
  build=subprocess.run(cmd,capture_output=True);(out/(name+'-build.log')).write_bytes(build.stdout+build.stderr);assert not build.returncode,(name,build.stderr.decode())
  run=subprocess.run([str(exe)],capture_output=True,timeout=20);(out/(name+'.log')).write_bytes(run.stdout+run.stderr)
  expected_rc=86 if want and want.startswith('!') else 1
  expected_error=('INVARIANT '+want[1:] if expected_rc==86 else 'FAIL '+want)+'\n' if want else ''
  assert (run.returncode==expected_rc and run.stderr==expected_error.encode()) if want else (run.returncode==0 and not run.stderr),(name,run.returncode,run.stdout,run.stderr)
  results.append(dict(case=name,expected=want,rc=run.returncode,source_sha256=sha(p),binary_sha256=sha(exe)));print('PASS',name,want or run.stdout.decode().strip(),flush=True)
 assert hashes=={str(p.relative_to(R)):sha(p) for p in paths}
 (out/'result.json').write_text(json.dumps(dict(inputs=hashes,results=results,limits='Frontend executes actual copied-SQE preparation/publish for fixed and provided paths plus real rejection storage/delivery and engine routing. CONNECT constructor returns controlled NOENT, so its new successful adoption path is not exercised here. Return control fixture additionally executes actual creator authority and copied return validation, exact pool ledger and registered engine-wake path. Pool backing remains controlled as previously stated. Drain fixture composes actual engine get/put/visit/close/wait/collect and actual CONNECT step/release with operation action stack. Handshake install and native poll hooks/protocol destruction remain host boundaries. Earlier cases retain legacy fake engine roots. Action executor uses actual submit, scope abort, owner/program/admission/retirement/selector. Progress step and successful parsed responses remain explicit controlled boundaries in new action cases. Separate external driver pin/root is modeled and asserted. Actual client-mark-dead/progress-abort, transport/session/codec abort, per-owner close and physical operation destructor, final pool-destruction helper, request/FID/pool/CQ transitions. Parsed successful replies remain synthetic; prior preparation regressions never submit. Host native teardown, translation, allocator, image snapshot, ref/engine/wake and physical backing drop boundaries. Earlier preparation/provided cases retain manual fixture destruction; new retirement cases use actual destructor. Final pool helper is called at simulated zero Loom references, not actual retirer. No native READY/frontend/close/concurrency qualification.'),indent=2)+'\n');print(out)
if __name__=='__main__':main()
