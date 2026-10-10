from pathlib import Path
import json,subprocess,hashlib,re,os
D=Path(__file__).resolve().parent;R=D.parents[3];O=D.parent/'retirement-draft'
r=subprocess.check_output([str(R/'work/oct5-as-r9/astra-yip.sh'),'resources'],text=True);assert re.search(r'^mac +HELD by you',r,re.M)
st=os.statvfs(R);assert st.f_bavail*st.f_frsize>=8*1024**3
base=json.loads((O/'arm64-result.json').read_text())[0]['command']
base.insert(1,'-I'+str(D.parent/'ready-draft/kernel/include'));base.insert(1,'-I'+str(D/'kernel/include'))
results=[]
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
for n in ['action','ready']:
 cmd=base.copy();cmd[cmd.index('-c')+1]=str(D/f'kernel/loom_service_{n}.c');cmd[cmd.index('-o')+1]=str(D/(n+'.o'));cmd[cmd.index('-MF')+1]=str(D/(n+'.d'))
 r=subprocess.run(cmd,capture_output=True);(D/(n+'-arm64.log')).write_bytes(r.stdout+r.stderr);assert not r.returncode,r.stderr.decode()
 nm=subprocess.check_output(['/opt/homebrew/opt/llvm/bin/llvm-nm','-u',str(D/(n+'.o'))],text=True);(D/(n+'-symbols.log')).write_text(nm);assert not any(n in nm for n in ['memcpy','memset','memmove'])
 deps=(D/(n+'.d')).read_text().replace('\\\n','').split(':',1)[1].split()
 results.append(dict(command=cmd,inputs={str(Path(p).relative_to(R)) if Path(p).is_relative_to(R) else p:sha(p) for p in deps},object_sha256=sha(D/(n+'.o')),rc=0))
(D/'arm64-result.json').write_text(json.dumps(results,indent=2)+'\n');print('ARM64 action and corrected selector objects pass, no libc dependency')
