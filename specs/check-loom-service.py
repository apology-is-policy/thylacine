#!/usr/bin/env python3
"""AS-0 finite lifecycle gate; run while holding the appropriate Yip lease.

No peer fairness is assumed. Local execution/CQ drain are weakly fair. This
model does not prove actual C locking, protocol parsing or authority checks.
"""
from pathlib import Path
import argparse, os, re, shutil, subprocess, tempfile

CASES = {'clean':None, 'earlyfree':'NoEarlyFree', 'late':'NoLateSuccess',
         'double':'NoDoubleTerminal', 'reuse':'NoStaleSlot', 'cqfull':'CqBounded',
         'refund':'CreditsRetained', 'peerwait':'RetirementProgress'}
ROOT=Path(__file__).resolve().parent

def main():
    p=argparse.ArgumentParser();p.add_argument('--logs',type=Path);a=p.parse_args()
    java=os.environ.get('JAVA', '/opt/homebrew/opt/openjdk/bin/java' if Path('/opt/homebrew/opt/openjdk/bin/java').exists() else 'java')
    jar=os.environ.get('TLA_JAR','/tmp/tla2tools.jar')
    if not Path(jar).is_file():raise RuntimeError('TLA_JAR is missing; no model checked')
    with tempfile.TemporaryDirectory(prefix='loom-service-model-') as tmp:
        temp=Path(tmp);logs=a.logs or temp;logs.mkdir(parents=True,exist_ok=True)
        for bug,want in CASES.items():
            cfg='loom_service'+('' if bug=='clean' else '_buggy_'+bug)+'.cfg'
            meta=temp/('states-'+bug);log=logs/(bug+'.log')
            with log.open('wb') as out:
                r=subprocess.run([java,'-Xmx768m','-XX:+UseParallelGC','-cp',jar,'tlc2.TLC','-workers','2','-deadlock','-noGenerateSpecTE','-metadir',str(meta),'-config',str(ROOT/cfg),str(ROOT/'loom_service.tla')],stdout=out,stderr=subprocess.STDOUT,timeout=120)
            text=log.read_text()
            if want is None:
                counts=re.findall(r'(\d+) distinct states found',text)
                assert r.returncode==0 and counts and counts[-1]=='5828' and 'No error has been found' in text,text[-3000:]
            else:
                assert r.returncode in (12,13) and (f'Invariant {want} is violated' in text or f'Temporal property {want} was violated' in text),text[-3000:]
            print(f'PASS {bug}: '+(want or '5828 states, retirement liveness'),flush=True)
            shutil.rmtree(meta,ignore_errors=True)
if __name__=='__main__':main()
