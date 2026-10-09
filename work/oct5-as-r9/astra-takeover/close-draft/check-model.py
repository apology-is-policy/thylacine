from pathlib import Path
import subprocess,json,hashlib,re,tempfile
root=Path(__file__).resolve().parent
out=root/'model-1';out.mkdir(exist_ok=False)
results=[]
with tempfile.TemporaryDirectory(prefix='astra-close-model-') as temp:
 for bug,want in [('clean',None),('duplicate','QueueRoot'),('early','QueueRoot'),('restart','NoRestart'),('selfwake','NoInternalWake'),('park','NoLostWake'),('lost','NoLostWake'),('leak','QueueRoot')]:
  cfg=root/'specs'/('loom_service_close'+('' if bug=='clean' else '_buggy_'+bug)+'.cfg')
  cmd=['/opt/homebrew/opt/openjdk/bin/java','-Xmx128m','-XX:+UseSerialGC','-cp','/tmp/tla2tools.jar','tlc2.TLC','-workers','1','-deadlock','-noGenerateSpecTE','-metadir',str(Path(temp)/bug),'-config',str(cfg),str(root/'specs/loom_service_close.tla')]
  (out/(bug+'-command.json')).write_text(json.dumps(cmd)+'\n')
  with (out/(bug+'.log')).open('wb') as f:r=subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT,timeout=120)
  s=(out/(bug+'.log')).read_text();counts=re.findall(r'(\d+) distinct states found',s)
  assert (r.returncode==0 and 'No error has been found' in s) if want is None else (r.returncode==12 and f'Invariant {want} is violated' in s),(bug,r.returncode,s[-2500:])
  results.append(dict(case=bug,expected=want,rc=r.returncode,states=int(counts[-1]),sha256=hashlib.sha256(s.encode()).hexdigest()))
  print('PASS',bug,counts[-1],flush=True)
(out/'result.json').write_text(json.dumps({'results':results,'inputs':{str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in (root/'specs').iterdir()},'limits':'Bounded queue-root/park event model; does not model C memory ordering, protocol retirement or scheduler fairness.'},indent=2)+'\n')
