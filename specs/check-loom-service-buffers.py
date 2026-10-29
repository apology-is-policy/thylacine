#!/usr/bin/env python3
"""Provided-buffer model; hold the host lease. No peer/CQ/return fairness.

Each sabotage must violate its named property, not merely return nonzero.
Temporary TLC state trees are bounded by timeout/heap and removed on every exit.
"""
from pathlib import Path
import argparse, hashlib, json, os, re, subprocess, tempfile
ROOT = Path(__file__).resolve().parent
PROPS = ['TypeOK', 'ExactReturn', 'PayloadIntact', 'PairedPublication',
         'CqBounded', 'PendingBounded', 'NoLateSuccess', 'NoRetiredWriter',
         'NoDuplicate', 'MoreBeforeFinal']
BUGS = {'ackfree':'PayloadIntact', 'stale':'ExactReturn',
        'pair':'PairedPublication', 'cqfull':'CqBounded',
        'late':'NoLateSuccess', 'recycle':'PayloadIntact',
        'order':'MoreBeforeFinal', 'double':'NoDuplicate',
        'peerwait':'RetirementProgress', 'returnwait':'RetirementProgress',
        'cqwait':'RetirementProgress'}
def main():
    p=argparse.ArgumentParser(); p.add_argument('--logs',type=Path,required=True)
    p.add_argument('--case'); a=p.parse_args(); a.logs.mkdir(parents=True,exist_ok=True)
    java=os.environ.get('JAVA','/opt/homebrew/opt/openjdk/bin/java')
    jar=os.environ.get('TLA_JAR','/tmp/tla2tools.jar')
    cases=[('clean-one',1,2,2,None),('clean-two',2,3,2,None)]
    cases += [(bug,2,3,2,want) for bug,want in BUGS.items()]
    results=[]
    for name,m,n,g,want in cases:
        if a.case and name != a.case: continue
        with tempfile.TemporaryDirectory(prefix='loom-payload-model-') as tmp:
            temp=Path(tmp); cfg=temp/'case.cfg'
            cfg.write_text('SPECIFICATION Spec\nCONSTANTS\n'
                f' Bug = "{name if want else "clean"}"\n Members = {m}\n Shots = {n}\n Generations = {g}\n'
                +'INVARIANTS\n '+'\n '.join(PROPS)+'\nPROPERTY RetirementProgress\n')
            (a.logs/(name+'.cfg')).write_bytes(cfg.read_bytes())
            log=a.logs/(name+'.log')
            with log.open('wb') as out:
                r=subprocess.run([java,'-Xmx768m','-XX:+UseParallelGC','-cp',jar,'tlc2.TLC',
                    '-workers','2','-deadlock','-noGenerateSpecTE','-metadir',str(temp/'states'),
                    '-config',str(cfg),str(ROOT/'loom_service_buffers.tla')],
                    stdout=out,stderr=subprocess.STDOUT,timeout=120)
            text=log.read_text(); counts=re.findall(r'(\d+) distinct states found',text)
            if want:
                assert r.returncode in (12,13) and (f'Invariant {want} is violated' in text or
                    f'Temporal property {want} was violated' in text),text[-4000:]
            else:
                assert r.returncode==0 and counts and 'No error has been found' in text,text[-4000:]
                assert int(counts[-1]) == {'clean-one':464,'clean-two':6416}[name],counts
            results.append(dict(case=name,expected=want,states=int(counts[-1]) if counts else None))
            print('PASS',results[-1],flush=True)
    assert results, 'No matching case'
    (a.logs/'result.json').write_text(json.dumps(dict(results=results,model_sha256=
        hashlib.sha256((ROOT/'loom_service_buffers.tla').read_bytes()).hexdigest()),indent=2)+'\n')
if __name__=='__main__':main()
