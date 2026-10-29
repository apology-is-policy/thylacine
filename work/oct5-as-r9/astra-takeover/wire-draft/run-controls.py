from pathlib import Path
import subprocess,json,hashlib,datetime,re,os,difflib
D=Path(__file__).resolve().parent;R=D.parents[3]
M=[
 ('negotiated','s->negotiated_msize?s->negotiated_msize:s->msize','s->msize','read negotiated bound'),
 ('read-overhead','P9_HDR_LEN+4u','P9_HDR_LEN+3u','read negotiated bound'),
 ('write-overhead','P9_HDR_LEN+16u','P9_HDR_LEN+17u','write negotiated bound'),
 ('output-cap','if(cap>msize)cap=msize;','cap=msize;','write output capacity clamp'),
 ('iounit','if(w->iounit && w->iounit<limit)limit=w->iounit;','','iounit bound'),
 ('capacity','count>w->payload_capacity ||','false ||','capacity refusal before effect'),
 ('overflow','if(w->offset>UINT64_MAX-count)return -T_E_INVAL;','','offset overflow refusal'),
 ('open-mode','if(w->mode>2)return -T_E_INVAL;','','open mode refusal'),
 ('dac-mask','P9_GETATTR_MODE|P9_GETATTR_UID|P9_GETATTR_GID','P9_GETATTR_MODE|P9_GETATTR_UID','complete DAC attribute mask'),
 ('expect-fid','u32 new_fid=w->fid;','u32 new_fid=0;','nonwalk expectation identity'),
 ('walk-arity','names=n?1:0','names=0','copied component frame'),
 ('local-proof','w->local_again=(rc==-P9_E_AGAIN);','w->local_again=true;','actual accepted owner'),
 ('remote-retry','w->local_again && !error','!error','remote EAGAIN never replayed'),
 ('capture-error','&& !error &&','&& (error || true) &&','malformed capture bars retry'),
]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 resources=subprocess.check_output([str(R/'work/oct5-as-r9/astra-yip.sh'),'resources'],text=True)
 assert re.search(r'^mac +HELD by you',resources,re.M)
 st=os.statvfs(R);assert st.f_bavail*st.f_frsize>=8*1024**3
 out=D/('controls-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));out.mkdir()
 paths=[D/'kernel/loom_service_wire.c',D/'kernel/include/thylacine/loom_service_wire.h',D/'fixture.c',D/'run-controls.py',D/'host/thylacine/spinlock.h']+[R/n for n in ['kernel/9p_client.c','kernel/9p_session.c','kernel/9p_wire.c','kernel/9p_transport.c','kernel/loom_service_reply.c','kernel/include/thylacine/9p_client.h','kernel/include/thylacine/9p_session.h','kernel/include/thylacine/9p_wire.h','kernel/include/thylacine/9p_transport.h','kernel/include/thylacine/loom_service_reply.h']]
 hashes={str(p.relative_to(R)):sha(p) for p in paths}
 original=paths[0].read_text();client=paths[5].read_text();a=client.index('int p9_client_submit_async(');b=client.index('// Private clients reuse',a)
 fixture=paths[2].read_text().replace('/* ACTUAL_SUBMIT */',client[a:b])
 (out/'fixture.c').write_text(fixture)
 sdk=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip();results=[]
 for name,old,new,want in [('control',None,None,None)]+M:
  if old is not None:assert original.count(old)==1,(name,original.count(old))
  source=original if old is None else original.replace(old,new)
  p=out/(name+'.c');p.write_text(source);exe=out/(name+'.exe')
  (out/(name+'.diff')).write_text(''.join(difflib.unified_diff(original.splitlines(True),source.splitlines(True),fromfile='kernel/loom_service_wire.c',tofile=name+'.c')))
  cmd=['/opt/homebrew/opt/llvm@22/bin/clang','-std=c11','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-g','-fsanitize=address,undefined','-isysroot',sdk,'-I'+str(D/'host'),'-I'+str(D/'kernel/include'),'-I'+str(R/'kernel/include'),str(p),str(out/'fixture.c'),*[str(p) for p in paths[6:10]],'-o',str(exe)]
  (out/(name+'-command.json')).write_text(json.dumps(cmd)+'\n')
  build=subprocess.run(cmd,capture_output=True);(out/(name+'-build.log')).write_bytes(build.stdout+build.stderr);assert not build.returncode,(name,build.stderr.decode())
  run=subprocess.run([str(exe)],capture_output=True,timeout=20);(out/(name+'.log')).write_bytes(run.stdout+run.stderr)
  assert (run.returncode==1 and run.stderr==f'FAIL {want}\n'.encode()) if want else (run.returncode==0 and not run.stderr),(name,run.returncode,run.stdout,run.stderr)
  results.append(dict(case=name,expected=want,rc=run.returncode,source_sha256=sha(p),binary_sha256=sha(exe)));print('PASS',name,want or run.stdout.decode().strip(),flush=True)
 assert hashes=={str(p.relative_to(R)):sha(p) for p in paths}
 (out/'result.json').write_text(json.dumps(dict(inputs=hashes,fixture_sha256=sha(out/'fixture.c'),results=results,limits='Actual submit_async function, real full headers/session/codec/transport/reply capture; host checked-lock, wake and death-completion stand-ins. Already-attached fixture; replies dispatched directly, no real demux/engine/DAC/concurrency/native proof.'),indent=2)+'\n');print(out)
if __name__=='__main__':main()
