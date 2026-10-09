from pathlib import Path
import json,subprocess,hashlib,re,os
D=Path(__file__).resolve().parent;R=D.parents[3]
resources=subprocess.check_output([str(R/'work/oct5-as-r9/astra-yip.sh'),'resources'],text=True);assert re.search(r'^mac +HELD by you',resources,re.M)
st=os.statvfs(R);assert st.f_bavail*st.f_frsize>=8*1024**3
base=json.loads((D.parent/'program-draft/arm64-command.json').read_text());base.insert(1,'-I'+str(D/'kernel/include'));base.insert(2,'-I'+str(R))
results=[]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
for n in ['rpc','operation','admission','io','provided']:
 cmd=base.copy();cmd[cmd.index('-c')+1]=str(D/f'kernel/loom_service_{n}.c');cmd[cmd.index('-o')+1]=str(D/(n+'.o'));cmd+=['-MD','-MF',str(D/(n+'.d'))]
 p=subprocess.run(cmd,capture_output=True);(D/(n+'-arm64.log')).write_bytes(p.stdout+p.stderr)
 assert p.returncode==0,p.stderr.decode()
 nm=subprocess.run(['/opt/homebrew/opt/llvm/bin/llvm-nm','-u',str(D/(n+'.o'))],capture_output=True,text=True);(D/(n+'-symbols.log')).write_text(nm.stdout)
 assert not any(x in nm.stdout for x in ['memcpy','memset','memmove']),nm.stdout
 deps=(D/(n+'.d')).read_text().replace('\\\n','').split(':',1)[1].split();hashes={str(Path(p).relative_to(R)) if Path(p).is_relative_to(R) else p:sha(Path(p)) for p in deps}
 results.append(dict(command=cmd,rc=p.returncode,inputs=hashes,object_sha256=sha(D/(n+'.o'))))
layout=D/'layout.c';layout.write_text('#include <thylacine/loom_service_operation.h>\nchar astra_operation_size[sizeof(struct loom_service_operation)];\nchar astra_protocol_size[sizeof(struct loom_service_protocol)];\n')
cmd=base.copy();cmd[cmd.index('-c')+1]=str(layout);cmd[cmd.index('-o')+1]=str(D/'layout.o');p=subprocess.run(cmd,capture_output=True);assert not p.returncode,p.stderr
out=subprocess.check_output(['/opt/homebrew/opt/llvm/bin/llvm-nm','-S',str(D/'layout.o')],text=True);(D/'layout.log').write_text(out)
results.append(dict(command=cmd,rc=0,inputs={str(layout.relative_to(R)):sha(layout)},object_sha256=sha(D/'layout.o')))
(D/'arm64-result.json').write_text(json.dumps(results,indent=2)+'\n');print(out)
