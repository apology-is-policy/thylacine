from pathlib import Path
import subprocess,json,hashlib,datetime,re,os,difflib
D=Path(__file__).resolve().parent;R=D.parents[3];W=D.parent/'wire-draft'
M=[
 ('stat-before-open','bool stat=kind==P9_TLOPEN ||','bool stat=false ||','open starts with stat'),
 ('identity-copy','dst[i]=src[i];','dst[i]=src[i]&0;','admission snapshot detached'),
 ('remote-again','if(loom_private_wire_retry(&p->wire,0,r))','if(r->result==-T_E_AGAIN)','remote shortage terminal no replay'),
 ('malformed',' || capture_error)',')','malformed capture poisons scope'),
 ('mask','(r->attr.valid&needed)!=needed','(r->attr.valid&(needed&~P9_GETATTR_GID))!=(needed&~P9_GETATTR_GID)','missing stat field poisons scope'),
 ('symlink','if(r->attr.qid.type&P9_QTSYMLINK)','if(false)','symlink open refused before DAC'),
 ('directory','if(!(r->attr.qid.type&P9_QTDIR))','if(false)','directory type before search'),
 ('search','want=PERM_X;','want=PERM_R;','search permits walk stage'),
 ('deny-open','if(perm_check_identity(&p->identity,&st,want))','if(false && perm_check_identity(&p->identity,&st,want))','denied open sends no effect'),
 ('effect-type','p->wire.kind=p->operation;','p->wire.kind=P9_TREAD;','authorized stat advances open'),
 ('built','if(!p->wire.built ||','if(false ||','uncaptured success rejected'),
 ('expectation','p->wire.expect.kind!=p->wire.kind','false','wrong frame expectation rejected'),
 ('result','return program_end(p,r->result,LPROGRAM_TERMINAL);\n}','return program_end(p,0,LPROGRAM_TERMINAL);\n}','short read preserved'),
 ('open-flags','kind==P9_TLOPEN && p->wire.mode>2','false','unallocated open mode refused'),
 ('readwrite','want=perm_want_for_omode(p->wire.mode);','want=PERM_R;','denied open sends no effect'),
]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 resources=subprocess.check_output([str(R/'work/oct5-as-r9/astra-yip.sh'),'resources'],text=True);assert re.search(r'^mac +HELD by you',resources,re.M)
 st=os.statvfs(R);assert st.f_bavail*st.f_frsize>=8*1024**3
 out=D/('controls-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));out.mkdir()
 own=[D/'kernel/loom_service_program.c',D/'kernel/include/thylacine/loom_service_program.h',D/'fixture.c',D/'run-controls.py']
 linked=[W/'kernel/loom_service_wire.c']+[R/n for n in ['kernel/perm.c','kernel/9p_session.c','kernel/9p_wire.c','kernel/9p_transport.c','kernel/loom_service_reply.c']]
 headers=[W/'kernel/include/thylacine/loom_service_wire.h',D/'host/thylacine/spinlock.h',W/'fixture.c']+[R/n for n in ['kernel/9p_client.c','kernel/include/thylacine/9p_client.h','kernel/include/thylacine/9p_session.h','kernel/include/thylacine/9p_wire.h','kernel/include/thylacine/9p_transport.h','kernel/include/thylacine/loom_service_reply.h','kernel/include/thylacine/proc.h','kernel/include/thylacine/perm.h','kernel/include/thylacine/caps.h']]
 paths=own+linked+headers;hashes={str(p.relative_to(R)):sha(p) for p in paths}
 original=own[0].read_text();client=(R/'kernel/9p_client.c').read_text();a=client.index('int p9_client_submit_async(');b=client.index('// Private clients reuse',a)
 fixture=(W/'fixture.c').read_text().replace('/* ACTUAL_SUBMIT */',client[a:b]).replace('#include <thylacine/loom_service_wire.h>','#include <thylacine/loom_service_program.h>')
 fixture=fixture.replace('static struct loom_service_wire w;','static struct loom_service_program program;\n#define w program.wire').replace('memset(&w,0,sizeof(w));','memset(&program,0,sizeof(program));').replace('int main(void)','static int wire_baseline(void)')+own[2].read_text()
 (out/'fixture.c').write_text(fixture)
 sdk=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip();results=[]
 for name,old,new,want in [('control',None,None,None)]+M:
  if old is not None:assert original.count(old)==1,(name,original.count(old))
  source=original if old is None else original.replace(old,new)
  p=out/(name+'.c');p.write_text(source);exe=out/(name+'.exe')
  (out/(name+'.diff')).write_text(''.join(difflib.unified_diff(original.splitlines(True),source.splitlines(True),fromfile='kernel/loom_service_program.c',tofile=name+'.c')))
  cmd=['/opt/homebrew/opt/llvm@22/bin/clang','-std=c11','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-g','-fsanitize=address,undefined','-isysroot',sdk,'-I'+str(D/'host'),'-I'+str(D/'kernel/include'),'-I'+str(W/'kernel/include'),'-I'+str(R/'kernel/include'),str(p),str(out/'fixture.c'),*[str(p) for p in linked],'-o',str(exe)]
  (out/(name+'-command.json')).write_text(json.dumps(cmd)+'\n')
  build=subprocess.run(cmd,capture_output=True);(out/(name+'-build.log')).write_bytes(build.stdout+build.stderr);assert not build.returncode,(name,build.stderr.decode())
  run=subprocess.run([str(exe)],capture_output=True,timeout=20);(out/(name+'.log')).write_bytes(run.stdout+run.stderr)
  assert (run.returncode==1 and run.stderr==f'FAIL {want}\n'.encode()) if want else (run.returncode==0 and not run.stderr),(name,run.returncode,run.stdout,run.stderr)
  results.append(dict(case=name,expected=want,rc=run.returncode,source_sha256=sha(p),binary_sha256=sha(exe)));print('PASS',name,want or run.stdout.decode().strip(),flush=True)
 assert hashes=={str(p.relative_to(R)):sha(p) for p in paths}
 (out/'result.json').write_text(json.dumps(dict(inputs=hashes,fixture_sha256=sha(out/'fixture.c'),results=results,limits='Actual staged program/wire/perm/session/codec/transport/capture, extracted real submit function. Host sequential lock/wake/death stand-ins and direct dispatch; already-attached sessions; direct synthetic captured attrs for exhaustive DAC oracle. No native engine/admission/buffer/concurrency proof.'),indent=2)+'\n');print(out)
if __name__=='__main__':main()
