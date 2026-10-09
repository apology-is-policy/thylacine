from pathlib import Path
import subprocess,json,hashlib,datetime,re,os,difflib
D=Path(__file__).resolve().parent;R=D.parents[3];W=D.parent/'wire-draft';P=D.parent/'program-draft'
M=[
 ('submit-once','o->rpc.on_complete!=loom_rpc_capture || o->submitted || o->callback_seen ||','o->rpc.on_complete!=loom_rpc_capture || o->callback_seen ||','one submission per attempt'),
 ('advance-once','o->rpc.sending || o->advanced)return','o->rpc.sending)return','one advance per callback'),
 ('advance-early','!o->submitted || !o->callback_seen ||','false ||','no advance before submission'),
 ('advance-sender','o->rpc.sending || o->advanced)return','o->advanced)return','sender blocks advance and settlement'),
 ('fallback','if(rc<0 && !o->callback_seen)loom_rpc_capture','if(false)loom_rpc_capture','invalid client refusal captured locally'),
 ('double-capture','if(rc<0 && !o->callback_seen)loom_rpc_capture','if(rc<0)loom_rpc_capture','synchronous refusal captured once'),
 ('terminal-retry','o->action!=LPROGRAM_NEXT ||','false ||','terminal program never requeues'),
 ('unadvanced-retry','!o->advanced || o->action!=LPROGRAM_NEXT ||','false ||','no unadvanced requeue'),
 ('retry-expiry','if(r->deadline && r->deadline<=now)return -T_E_TIMEDOUT;\n    o->rpc.on_complete=NULL','if(false)return -T_E_TIMEDOUT;\n    o->rpc.on_complete=NULL','deadline bars next stage'),
 ('payload','o->program.wire.payload,o->program.wire.payload_capacity','NULL,0','builder and capture share retained payload'),
 ('success-program','!o->advanced || o->action!=LPROGRAM_TERMINAL ||\n           o->program.phase!=LPROGRAM_DONE || o->program.result<0','false','denied program cannot publish success'),
 ('success-advance','!o->advanced || o->action!=LPROGRAM_TERMINAL ||\n           o->program.phase!=LPROGRAM_DONE || o->program.result<0','(o->advanced && o->program.result<0)','stat cannot bypass program advance'),
 ('reset-attempt','o->submitted=false;o->advanced=false;','o->submitted=false;','new attempt resets consumed edges'),
 ('duplicate-callback','o->phase!=LRPC_INFLIGHT || o->callback_seen','o->phase!=LRPC_INFLIGHT','duplicate callback poisons scope'),
 ('early-settle','(o->phase!=LRPC_INFLIGHT || !o->callback_seen)))','(o->phase!=LRPC_INFLIGHT)))','no early settlement'),
 ('finish-expiry','        if(r->deadline && r->deadline<=now)return -T_E_TIMEDOUT;','        (void)now;','deadline beats success'),
 ('abort-latch','!o->protocol->abort_reason)return -T_E_BUSY;','false)return -T_E_BUSY;','poison cannot finish without scope abort'),
]

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 resources=subprocess.check_output([str(R/'work/oct5-as-r9/astra-yip.sh'),'resources'],text=True);assert re.search(r'^mac +HELD by you',resources,re.M)
 st=os.statvfs(R);assert st.f_bavail*st.f_frsize>=8*1024**3
 out=D/('controls-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));out.mkdir()
 own=[D/'kernel/loom_service_rpc.c',D/'kernel/include/thylacine/loom_service_rpc.h',D/'kernel/include/thylacine/loom_service_operation.h',D/'kernel/include/thylacine/loom_service_protocol.h',D/'fixture.c',D/'run-controls.py']
 linked=[W/'kernel/loom_service_wire.c',P/'kernel/loom_service_program.c']+[R/n for n in ['kernel/perm.c','kernel/9p_session.c','kernel/9p_wire.c','kernel/9p_transport.c','kernel/loom_service_reply.c','kernel/loom_service_requests.c','kernel/loom_service_slots.c','kernel/loom_service_nodes.c']]
 headers=[W/'kernel/include/thylacine/loom_service_wire.h',P/'kernel/include/thylacine/loom_service_program.h',P/'host/thylacine/spinlock.h']+[R/n for n in ['kernel/9p_client.c','kernel/include/thylacine/9p_client.h','kernel/include/thylacine/9p_session.h','kernel/include/thylacine/9p_wire.h','kernel/include/thylacine/9p_transport.h','kernel/include/thylacine/loom_service_reply.h','kernel/include/thylacine/proc.h','kernel/include/thylacine/perm.h','kernel/include/thylacine/caps.h']]
 paths=own+linked+headers;hashes={str(p.relative_to(R)):sha(p) for p in paths}
 original=own[0].read_text();client=(R/'kernel/9p_client.c').read_text();a=client.index('int p9_client_submit_async(');b=client.index('// Private clients reuse',a)
 fixture=(D/'fixture.c').read_text().replace('/* ACTUAL_SUBMIT */',client[a:b])
 sdk=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip();results=[]
 for name,old,new,want in [('control',None,None,None)]+M:
  if old is not None:assert original.count(old)==1,(name,original.count(old))
  source=original if old is None else original.replace(old,new)
  p=out/(name+'.c');p.write_text(fixture.replace('/* ACTUAL_RPC */',re.sub(r'^#include[^\n]*\n','',source,flags=re.M)));exe=out/(name+'.exe')
  (out/(name+'.diff')).write_text(''.join(difflib.unified_diff(original.splitlines(True),source.splitlines(True),fromfile='kernel/loom_service_program.c',tofile=name+'.c')))
  cmd=['/opt/homebrew/opt/llvm@22/bin/clang','-std=c11','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-g','-fsanitize=address,undefined','-isysroot',sdk,'-I'+str(P/'host'),'-I'+str(D/'kernel/include'),'-I'+str(P/'kernel/include'),'-I'+str(W/'kernel/include'),'-I'+str(R/'kernel/include'),str(p),*[str(p) for p in linked],'-o',str(exe)]
  (out/(name+'-command.json')).write_text(json.dumps(cmd)+'\n')
  build=subprocess.run(cmd,capture_output=True);(out/(name+'-build.log')).write_bytes(build.stdout+build.stderr);assert not build.returncode,(name,build.stderr.decode())
  run=subprocess.run([str(exe)],capture_output=True,timeout=20);(out/(name+'.log')).write_bytes(run.stdout+run.stderr)
  assert (run.returncode==1 and run.stderr==f'FAIL {want}\n'.encode()) if want else (run.returncode==0 and not run.stderr),(name,run.returncode,run.stdout,run.stderr)
  results.append(dict(case=name,expected=want,rc=run.returncode,source_sha256=sha(p),binary_sha256=sha(exe)));print('PASS',name,want or run.stdout.decode().strip(),flush=True)
 assert hashes=={str(p.relative_to(R)):sha(p) for p in paths}
 (out/'result.json').write_text(json.dumps(dict(inputs=hashes,results=results,limits='Actual combined RPC/program/wire/perm/request/node/slot/session/codec/transport/capture, real header layouts and extracted real submit. Host minimal Loom and sequential lock/wake/death stand-ins, direct dispatch, already-attached sessions. Physical buffer ownership and actual driver/admission/close/concurrency remain unqualified.'),indent=2)+'\n');print(out)
if __name__=='__main__':main()
