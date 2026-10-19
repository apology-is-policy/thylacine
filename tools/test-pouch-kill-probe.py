#!/usr/bin/env python3
"""Exercise Pouch kill(pid, 0) and getppid with a mocked kernel."""
if not __debug__:raise SystemExit(__file__+': run without -O or PYTHONOPTIMIZE: its verdicts are asserts')
import argparse,hashlib,json,platform,re,subprocess
from pathlib import Path
MUTANTS={
 'probe-posts':('kill.c','__syscall(SYS_postnote, (long)pid, 0L, 0UL)','__syscall(SYS_postnote, (long)pid, "child_exit", 10UL)'),
 'probe-eacces-raw':('kill.c','long r = __syscall(SYS_postnote, (long)pid, 0L, 0UL);\n\t\tif (r == -EACCES) {','long r = __syscall(SYS_postnote, (long)pid, 0L, 0UL);\n\t\tif (0) {'),
 'probe-group':('kill.c','\tif (sig == 0) {\n\t\tif (pid < 0) {\n\t\t\terrno = EPERM;\n\t\t\treturn -1;\n\t\t}\n','\tif (sig == 0) {\n'),
 'signal-group':('kill.c','\tif (pid < 0) {\n\t\terrno = EPERM;\n\t\treturn -1;\n\t}\n\tunsigned long len','\tunsigned long len'),
 'getppid-number':('getppid.c','__syscall(SYS_thyla_getppid)','__syscall(SYS_thyla_getppid - 1)'),
}
p=argparse.ArgumentParser(description=__doc__);p.add_argument('musl',type=Path);p.add_argument('output',type=Path);p.add_argument('--cc',default='/opt/homebrew/opt/llvm/bin/clang');p.add_argument('--mutant',choices=sorted(MUTANTS));a=p.parse_args();r=Path(__file__).resolve().parent.parent
a.output.mkdir(parents=True,exist_ok=False);inputs=[Path(__file__),r/'tools/host-tests/pouch-kill-probe.c']
def one(path,pattern):
 found=re.findall(pattern,path.read_text());assert len(found)==1,(path,pattern,found);return int(found[0])
kern=r/'kernel/include/thylacine/syscall.h';inputs.append(kern)
defs=['-DKERNEL_SYS_POSTNOTE=%d'%one(kern,r'\bSYS_POSTNOTE\s*=\s*(\d+)'),'-DKERNEL_SYS_GETPPID=%d'%one(kern,r'\bSYS_GETPPID\s*=\s*(\d+)')]
bits=a.musl/'arch/aarch64/bits/syscall.h.in';inputs.append(bits)
defs.append('-DSYS_postnote=%d'%one(bits,r'(?m)^#define __NR_postnote\s+(\d+)'))
for source,dest in [('src/internal/_pouch_fs.h','fs-under-test.h'),('src/internal/_pouch_signal.h','signal-under-test.h'),('src/signal/kill.c','kill-under-test.c'),('src/unistd/getppid.c','getppid-under-test.c')]:
 f=a.musl/source;inputs.append(f);s=f.read_text()
 for h in ['"syscall.h"','"_pouch_fs.h"','"_pouch_signal.h"','<features.h>']:s=re.sub(r'(?m)^#include '+re.escape(h),'/* host boundary */',s)
 m=MUTANTS.get(a.mutant)
 if m and source.endswith('/'+m[0]):
  assert s.count(m[1])==1,m;s=s.replace(m[1],m[2])
 (a.output/dest).write_text(s)
(a.output/'inputs.json').write_text(json.dumps({str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs},indent=2)+'\n')
sdk=['-isysroot',subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()] if platform.system()=='Darwin' else []
for opt in ('0','2'):
 exe=a.output.resolve()/('test-O'+opt);cmd=[a.cc,*sdk,'-O'+opt,'-g','-fsanitize=address,undefined','-Wall','-Wextra','-Werror',*defs,'-I'+str(a.output.resolve()),str(inputs[1]),'-o',str(exe)]
 (a.output/f'compile-O{opt}.json').write_text(json.dumps(cmd)+'\n')
 with (a.output/f'compile-O{opt}.log').open('w') as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
 result=subprocess.run([str(exe)],capture_output=True,text=True);(a.output/f'run-O{opt}.log').write_text(result.stdout+result.stderr);print(result.stdout,end='')
 assert 'AddressSanitizer' not in result.stderr and 'runtime error:' not in result.stderr
 assert result.returncode==(1 if a.mutant else 0)
