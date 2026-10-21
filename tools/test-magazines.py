#!/usr/bin/env python3
"""Actual per-CPU magazine bodies (mm/magazines.c, structs and sizes from
mm/magazines.h) under ThreadSanitizer: three CPUs churn their own sets while a
fourth runs magazines_drain_all. Clean: no race report, no page handed to two
owners, every page back in the zone exactly once. Each mutant drops one side's
lock and must draw a TSan race report. --source runs the same model on another
copy of magazines.c (the pre-lock tree must fail). Host doubles: zone, CPU id,
spinlocks; an IRQ mask excludes nothing on a peer CPU. Not an ARM memory model.
"""
from pathlib import Path
import argparse,json,os,re,shutil,subprocess,tempfile,importlib.util
ROOT=Path(__file__).resolve().parent.parent
spec=importlib.util.spec_from_file_location('progress_fixture',ROOT/'tools/test-9p-client-progress.py');helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
LLVM_CC=Path(os.environ.get('LLVM_PREFIX','/opt/homebrew/opt/llvm'))/'bin/clang'
NAMES=['magazines_init','order_to_mag_idx','mag_idx_to_order','my_cpu','mag_refill','mag_drain','mag_alloc','mag_free','magazines_drain_all']
DEFINES=['MAGAZINE_SIZE','MAG_IDX_ORDER_4K','MAG_IDX_ORDER_2M','NUM_MAG_ORDERS']
TSAN_EXIT=66
# (label, function, old, new): each removes one side of the exclusion.
MUTANTS=[
 ('drain-unlocked','magazines_drain_all','spin_lock_irqsave(&pc->lock)','spin_lock_irqsave(NULL)'),
 ('alloc-unlocked','mag_alloc','    spin_lock(&pc->lock);\n','\n'),
 ('free-unlocked','mag_free','    spin_lock(&pc->lock);\n','\n'),
]
def unlock_too(label,body):
    # The matching releases go with the dropped acquire.
    if label=='drain-unlocked':return body.replace('spin_unlock_irqrestore(&pc->lock, s)','spin_unlock_irqrestore(NULL, s)')
    if label in('alloc-unlocked','free-unlocked'):return body.replace('spin_unlock(&pc->lock);','')
    return body
def model(source,header,mutant=None):
    bodies={n:helper.function(source,n) for n in NAMES}
    if mutant:
        label,fn,old,new=mutant
        assert bodies[fn].count(old)==1,(label,old)
        bodies[fn]=unlock_too(label,bodies[fn].replace(old,new))
    defs=[]
    for d in DEFINES:
        m=re.search(r'^#define\s+'+d+r'\s+(\S+)',header,re.M);assert m,d;defs.append(f'#define {d} {m.group(1)}')
    structs=[]
    for s in ('struct magazine {','struct percpu_data {'):
        i=header.index(s);structs.append(header[i:header.index('};',i)+2])
    t=(ROOT/'tools/host-tests/magazines.c').read_text()
    for k,v in (('/* MAGAZINE_DEFINES */','\n'.join(defs)),('/* MAGAZINE_STRUCTS */','\n'.join(structs)),
                ('/* MAGAZINE_BODIES */','\n\n'.join(bodies[n] for n in NAMES))):
        assert t.count(k)==1,k;t=t.replace(k,v)
    return t
def main():
    p=argparse.ArgumentParser();p.add_argument('--logs',type=Path,required=True);p.add_argument('--mutants',action='store_true')
    p.add_argument('--source',type=Path,help='a magazines.c to model instead of the tree\'s; the run must then FAIL');p.add_argument('--iters',default='4000')
    a=p.parse_args();a.logs.mkdir(parents=True,exist_ok=True)
    source=(a.source or ROOT/'mm/magazines.c').read_text();header=(ROOT/'mm/magazines.h').read_text()
    cc=os.environ.get('CC') or (str(LLVM_CC) if LLVM_CC.exists() else 'clang')
    sdk=subprocess.run(['xcrun','--show-sdk-path'],capture_output=True,text=True) if shutil.which('xcrun') else None
    env=dict(os.environ,TSAN_OPTIONS=f'abort_on_error=0 halt_on_error=0 exitcode={TSAN_EXIT}')
    legs=[('source' if a.source else 'clean',None)]+([(m[0],m) for m in MUTANTS] if a.mutants and not a.source else [])
    with tempfile.TemporaryDirectory(prefix='magazines-') as tmp:
        for name,mut in legs:
            f=Path(tmp)/(name+'.c');f.write_text(model(source,header,mut));exe=f.with_suffix('')
            cmd=[cc,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=thread',str(f),'-o',str(exe)]
            if sdk and sdk.returncode==0:cmd+=['-isysroot',sdk.stdout.strip()]
            (a.logs/(name+'-command.json')).write_text(json.dumps(cmd)+'\n')
            with (a.logs/(name+'-build.log')).open('wb') as out:subprocess.run(cmd,stdout=out,stderr=subprocess.STDOUT,check=True)
            # A binary TSan never instrumented reports nothing; that silence is not a pass.
            nm=subprocess.run(['nm',str(exe)],capture_output=True,text=True).stdout
            assert '__tsan_' in nm,(name,'not instrumented')
            r=subprocess.run([str(exe),a.iters],capture_output=True,text=True,timeout=600,env=env)
            out=r.stdout+r.stderr;(a.logs/(name+'.log')).write_text(out)
            races=out.count('WARNING: ThreadSanitizer: data race');summary=[l for l in out.splitlines() if l.startswith('summary ')]
            assert summary,(name,'no summary: the run did not finish',r.returncode)
            if mut or a.source:
                assert races and r.returncode in (TSAN_EXIT,3),(name,'expected a race report',r.returncode,summary)
                print(f'PASS {name}: {races} race report(s); {summary[0]}',flush=True)
            else:
                assert r.returncode==0 and not races and 'ThreadSanitizer' not in out,(name,r.returncode,races,summary)
                print(f'PASS {name}: no race; {summary[0]}',flush=True)
if __name__=='__main__':main()
