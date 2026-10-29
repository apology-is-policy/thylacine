from pathlib import Path
import json,subprocess,hashlib,re,os
D=Path(__file__).resolve().parent;R=D.parents[3];O=D.parent/'retirement-draft'
r=subprocess.check_output([str(R/'work/oct5-as-r9/astra-yip.sh'),'resources'],text=True);assert re.search(r'^mac +HELD by you',r,re.M)
st=os.statvfs(R);assert st.f_bavail*st.f_frsize>=8*1024**3
cmd=json.loads((O/'arm64-result.json').read_text())[0]['command']
cmd.insert(1,'-I'+str(D/'kernel/include'));cmd[cmd.index('-c')+1]=str(D/'kernel/loom_service_ready.c');cmd[cmd.index('-o')+1]=str(D/'ready.o');cmd[cmd.index('-MF')+1]=str(D/'ready.d')
r=subprocess.run(cmd,capture_output=True);(D/'arm64.log').write_bytes(r.stdout+r.stderr);assert not r.returncode,r.stderr.decode()
nm=subprocess.check_output(['/opt/homebrew/opt/llvm/bin/llvm-nm','-u',str(D/'ready.o')],text=True);(D/'symbols.log').write_text(nm);assert not any(n in nm for n in ['memcpy','memset','memmove'])
deps=(D/'ready.d').read_text().replace('\\\n','').split(':',1)[1].split()
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
(D/'arm64-result.json').write_text(json.dumps(dict(command=cmd,inputs={str(Path(p).relative_to(R)) if Path(p).is_relative_to(R) else p:sha(p) for p in deps},object_sha256=sha(D/'ready.o'),rc=0),indent=2)+'\n');print('ARM64 scheduler object passes, no libc dependency')
