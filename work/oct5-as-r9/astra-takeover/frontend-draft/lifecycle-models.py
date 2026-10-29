from pathlib import Path
import datetime,subprocess,tempfile,json,hashlib,re,os
R=Path('/Users/northkillpd/projects/thylacine-corona');D=Path(__file__).resolve().parent
assert re.search(r'^mac +HELD by you',subprocess.check_output([str(R/'work/oct5-as-r9/astra-yip.sh'),'resources'],text=True),re.M)
out=D/('lifecycle-models-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));out.mkdir()
configs=[R/'specs/loom.cfg']+sorted((R/'specs').glob('loom_buggy_*.cfg'))+[R/'specs/loom_service.cfg']+sorted((R/'specs').glob('loom_service_buggy_*.cfg'))
inputs={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in configs+[R/'specs/loom.tla',R/'specs/loom_service.tla',Path(__file__)]}
results=[]
with tempfile.TemporaryDirectory(prefix='astra-lifecycle-models-') as tmp:
 for cfg in configs:
  st=os.statvfs(R);assert st.f_bavail*st.f_frsize>=8*1024**3
  cmd=['/opt/homebrew/opt/openjdk/bin/java','-Xmx512m','-XX:+UseParallelGC','-cp','/tmp/tla2tools.jar','tlc2.TLC','-workers','1','-deadlock','-noGenerateSpecTE','-metadir',str(Path(tmp)/cfg.stem),'-config',str(cfg),str(R/'specs'/('loom_service.tla' if cfg.stem.startswith('loom_service') else 'loom.tla'))]
  (out/(cfg.stem+'-command.json')).write_text(json.dumps(cmd)+'\n')
  log=out/(cfg.stem+'.log')
  with log.open('wb') as f:r=subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT,timeout=180)
  text=log.read_text();clean=cfg.stem in ['loom','loom_service'];prop='PROPERTIES' in cfg.read_text()
  want=re.search(r'(?:INVARIANTS|PROPERTIES)\s+(\w+)',cfg.read_text()).group(1)
  if cfg.stem.startswith('loom_service_buggy_'):
   bug=cfg.stem.removeprefix('loom_service_buggy_')
   want={'cqfull':'CqBounded','double':'NoDoubleTerminal','earlyfree':'NoEarlyFree','late':'NoLateSuccess','peerwait':'RetirementProgress','refund':'CreditsRetained','reuse':'NoStaleSlot'}[bug]
   prop=bug=='peerwait'
  valid=(r.returncode==0 and 'No error has been found' in text) if clean else ((r.returncode==13 and f'Temporal property {want} was violated' in text) if prop else (r.returncode==12 and f'Invariant {want} is violated' in text))
  results.append(dict(config=cfg.name,rc=r.returncode,expected=want,ok=valid,sha256=hashlib.sha256(log.read_bytes()).hexdigest()))
  (out/'progress.json').write_text(json.dumps(results,indent=2)+'\n')
  assert valid,(cfg.name,r.returncode,text[-1500:],out)
  print('PASS',cfg.name,want,flush=True)
assert inputs=={p:hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in inputs}
(out/'result.json').write_text(json.dumps(dict(inputs=inputs,results=results,limits='Unmodified existing Loom and private lifecycle models with configured counterexamples. Does not model the new executor or prove its C/native scheduling.'),indent=2)+'\n');print(out)
