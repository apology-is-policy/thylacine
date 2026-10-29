from pathlib import Path
import datetime,subprocess,tempfile,json,hashlib,re
R=Path('/Users/northkillpd/projects/thylacine-corona');D=Path(__file__).resolve().parent
assert re.search(r'^mac +HELD by you',subprocess.check_output([str(R/'work/oct5-as-r9/astra-yip.sh'),'resources'],text=True),re.M)
out=D/('poll-models-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));out.mkdir()
configs=[R/'specs/poll_local.cfg']+sorted((R/'specs').glob('poll_buggy_*.cfg'))
inputs={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in configs+[R/'specs/poll.tla',Path(__file__)]}
results=[]
with tempfile.TemporaryDirectory(prefix='astra-poll-models-') as tmp:
 for cfg in configs:
  cmd=['/opt/homebrew/opt/openjdk/bin/java','-Xmx512m','-XX:+UseParallelGC','-cp','/tmp/tla2tools.jar','tlc2.TLC','-workers','1','-deadlock','-noGenerateSpecTE','-metadir',str(Path(tmp)/cfg.stem),'-config',str(cfg),str(R/'specs/poll.tla')]
  (out/(cfg.stem+'-command.json')).write_text(json.dumps(cmd)+'\n')
  log=out/(cfg.stem+'.log')
  with log.open('wb') as f:r=subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT,timeout=180)
  text=log.read_text();clean=cfg.stem=='poll_local';prop='PROPERTIES' in cfg.read_text()
  want=re.search(r'(?:INVARIANTS|PROPERTIES)\s+(\w+)',cfg.read_text()).group(1)
  valid=(r.returncode==0 and 'No error has been found' in text) if clean else ((r.returncode==13 and f'Temporal property {want} was violated' in text) if prop else (r.returncode==12 and f'Invariant {want} is violated' in text))
  results.append(dict(config=cfg.name,rc=r.returncode,expected=want,ok=valid,sha256=hashlib.sha256(log.read_bytes()).hexdigest()))
  (out/'progress.json').write_text(json.dumps(results,indent=2)+'\n')
  assert valid,(cfg.name,r.returncode,text[-1500:],out)
  print('PASS',cfg.name,want,flush=True)
assert inputs=={p:hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in inputs}
(out/'result.json').write_text(json.dumps(dict(inputs=inputs,results=results,limits='Unmodified existing poll model: local clean plus all configured mutants. Does not model new private executor or prove its C/native scheduling.'),indent=2)+'\n');print(out)
