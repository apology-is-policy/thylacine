#!/usr/bin/env python3
"""Bounded index lifetime model; no wire, C memory model or scheduler proof."""
from pathlib import Path
import argparse,hashlib,json,os,re,subprocess,tempfile
ROOT=Path(__file__).resolve().parent.parent
CASES={'clean':None,'installer':'InstallerPinned','expose':'NoHalfPublished',
       'late':'NoLatePublication','pin':'EntryRetainsStorage',
       'terminal':'TerminalRetained','stale':'NoStaleEntry'}
def main():
 p=argparse.ArgumentParser();p.add_argument('--logs',type=Path,required=True);a=p.parse_args();a.logs.mkdir(parents=True,exist_ok=False)
 files=[ROOT/'specs/loom_service_index.tla',Path(__file__)]+sorted((ROOT/'specs').glob('loom_service_index*.cfg'))
 sha=lambda x:hashlib.sha256(x.read_bytes()).hexdigest()
 hashes={str(x.relative_to(ROOT)):sha(x) for x in files};results=[]
 with tempfile.TemporaryDirectory(prefix='loom-index-model-') as temp:
  for bug,want in CASES.items():
   cfg=ROOT/'specs'/('loom_service_index'+('' if bug=='clean' else '_buggy_'+bug)+'.cfg')
   cmd=[os.environ.get('JAVA','/opt/homebrew/opt/openjdk/bin/java'),'-Xmx512m','-XX:+UseParallelGC','-cp',os.environ.get('TLA_JAR','/tmp/tla2tools.jar'),'tlc2.TLC','-workers','1','-deadlock','-noGenerateSpecTE','-metadir',str(Path(temp)/bug),'-config',str(cfg),str(ROOT/'specs/loom_service_index.tla')]
   (a.logs/(bug+'-command.json')).write_text(json.dumps(cmd)+'\n');log=a.logs/(bug+'.log')
   with log.open('wb') as f:r=subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT,timeout=120)
   s=log.read_text();counts=re.findall(r'(\d+) distinct states found',s)
   assert (r.returncode==0 and 'No error has been found' in s and counts and int(counts[-1])==89) if want is None else (r.returncode==12 and f'Invariant {want} is violated' in s),(bug,r.returncode,s[-2500:])
   results.append({'case':bug,'expected':want,'rc':r.returncode,'states':int(counts[-1]) if counts else None,'log_sha256':sha(log)})
   print('PASS',bug,want or counts[-1]+' states',flush=True)
 assert hashes=={str(x.relative_to(ROOT)):sha(x) for x in files}
 (a.logs/'result.json').write_text(json.dumps({'inputs':hashes,'results':results,'limits':'finite lifetime/publication model, not actual C/wire/host memory model or fairness proof'},indent=2)+'\n')
if __name__=='__main__':main()
