from pathlib import Path
import subprocess,json,hashlib,datetime,os
R=Path('/Users/northkillpd/projects/thylacine-corona');D=Path(__file__).resolve().parent
out=D/('reviewed-checks-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));out.mkdir()
assert os.statvfs(R).f_bavail*os.statvfs(R).f_frsize>=8*1024**3
subprocess.run(['python3',str(D/'refresh-host-tree.py')],check=True)
paths=['kernel/loom.c','kernel/loom_service_engine.c','kernel/include/thylacine/loom_service_engine.h','kernel/include/thylacine/loom.h','kernel/test/test.c','kernel/test/test_loom.c','work/oct5-as-r9/slot-owner-control.py','work/oct5-as-r9/final-reclaim-control.py']
shadow=D/'host-tree';sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
hashes={p:sha(shadow/p) for p in paths};results=[]
# Actual target compiler; only historical test prototype warnings are nonfatal.
base=json.loads((D/'syntax-command.json').read_text())
base[base.index('-I'+str(D/'kernel/include'))]='-I'+str(shadow/'kernel/include')
for rel in paths[:6]:
 if not rel.endswith('.c'):continue
 cmd=base.copy();cmd[-1]=str(shadow/rel)
 if rel.startswith('kernel/test/'):
  cmd.insert(1,'-I'+str(R/'kernel/test'));cmd.insert(1,'-Wno-error=missing-prototypes')
 tag='syntax-'+Path(rel).name;log=out/(tag+'.log');(out/(tag+'-command.json')).write_text(json.dumps(cmd)+'\n')
 with log.open('wb') as f:ret=subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT)
 results.append(dict(check=tag,rc=ret.returncode,log=str(log)));assert ret.returncode==0,(tag,log)
commands=[('readiness',['python3',str(D/'control.py')]),('worker',['python3',str(D/'worker-control.py')]),('engine',['python3',str(D/'engine-regression.py')])]
commands += [(name,['python3',str(shadow/'work/oct5-as-r9'/script)]) for name,script in [('connect','connect-control.py'),('constructor','slot-owner-control.py'),('final-free','final-reclaim-control.py')]]
for tag,cmd in commands:
 log=out/(tag+'.log');(out/(tag+'-command.json')).write_text(json.dumps(cmd)+'\n')
 with log.open('wb') as f:ret=subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT,cwd=R)
 results.append(dict(check=tag,rc=ret.returncode,log=str(log)));(out/'progress.json').write_text(json.dumps(results,indent=2)+'\n')
 assert ret.returncode==0,(tag,log)
 print('PASS',tag,flush=True)
assert hashes=={p:sha(shadow/p) for p in paths}
manifest=dict(owner='astra',state='host-and-syntax-verified',source_base=subprocess.check_output(['git','rev-parse','HEAD'],cwd=R,text=True).strip(),install_from=str(shadow),files=hashes,checks=results,limits='Draft only; no canonical install, native execution, full Mac qualification or user dispatch activation.')
(out/'result.json').write_text(json.dumps(manifest,indent=2)+'\n');(D/'verified-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n');print(out)
