#!/usr/bin/env python3
"""Run the series' own __pouch_env_init (0025) over a mocked /env: what reaches environ, which 0064 sends as a spawned child's whole env."""
if not __debug__:raise SystemExit(__file__+': run without -O or PYTHONOPTIMIZE: its verdicts are asserts')
import argparse,hashlib,json,platform,re,subprocess
from pathlib import Path
# Each mutant must fail exactly its own cases: a FAIL elsewhere means the harness, not the filter, is what broke.
MUTANTS={
 'eq':("\t\tif (memchr(name, '=', nl)) continue;\n",'',{'eq-name-skipped','eq-name-ends-skipped','both-only-keeps-exec-envp'}),
 'nul':('if (vl < 0 || memchr(val, 0, (size_t)vl)) continue;','if (vl < 0) continue;',{'nul-value-skipped','nul-value-ends-skipped','both-only-keeps-exec-envp'}),
}
p=argparse.ArgumentParser(description=__doc__);p.add_argument('musl',type=Path);p.add_argument('output',type=Path);p.add_argument('--cc',default='/opt/homebrew/opt/llvm/bin/clang');p.add_argument('--mutant',choices=sorted(MUTANTS));a=p.parse_args();r=Path(__file__).resolve().parent.parent
a.output.mkdir(parents=True,exist_ok=False);src=a.musl/'src/env/_pouch_env.c';inputs=[Path(__file__),r/'tools/host-tests/pouch-env-init.c',src]
s=src.read_text()
for h in ['<dirent.h>','<fcntl.h>','<unistd.h>','<stdlib.h>','<string.h>','"syscall.h"']:
 assert len(re.findall(r'(?m)^#include '+re.escape(h)+'$',s))==1,(src,h);s=re.sub(r'(?m)^#include '+re.escape(h)+'$','/* host boundary */',s)
if a.mutant:
 m=MUTANTS[a.mutant];assert s.count(m[0])==1,m;s=s.replace(m[0],m[1])
(a.output/'env-under-test.c').write_text(s)
(a.output/'inputs.json').write_text(json.dumps({str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs},indent=2)+'\n')
sdk=['-isysroot',subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()] if platform.system()=='Darwin' else []
for opt in ('0','2'):
 exe=a.output.resolve()/('test-O'+opt);cmd=[a.cc,*sdk,'-std=c11','-O'+opt,'-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-Wall','-Wextra','-Werror','-I'+str(a.output.resolve()),str(inputs[1]),'-o',str(exe)]
 (a.output/f'compile-O{opt}.json').write_text(json.dumps(cmd)+'\n')
 with (a.output/f'compile-O{opt}.log').open('w') as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
 result=subprocess.run([str(exe)],capture_output=True,text=True);(a.output/f'run-O{opt}.log').write_text(result.stdout+result.stderr);print(result.stdout,end='')
 assert 'AddressSanitizer' not in result.stderr and 'runtime error:' not in result.stderr
 lines=result.stdout.splitlines();assert lines and lines[-1].startswith('tests='),'the run died before its tally'
 failed={l.split()[1] for l in lines if l.startswith('FAIL ')}
 assert failed==(MUTANTS[a.mutant][2] if a.mutant else set()),failed
 assert result.returncode==(1 if a.mutant else 0)
