#!/usr/bin/env python3
"""Exercise Pouch link and linkat with a mocked kernel and the series' own __syscall_ret."""
if not __debug__:raise SystemExit(__file__+': run without -O or PYTHONOPTIMIZE: its verdicts are asserts')
import argparse,hashlib,json,platform,re,subprocess
from pathlib import Path
from pouch_seam import stage_syscall_ret
MUTANTS={
 'link-traps':('link.c','\terrno = EPERM;\n\treturn -1;','return syscall(SYS_linkat, AT_FDCWD, existing, AT_FDCWD, new, 0);'),
 'linkat-traps':('linkat.c','\terrno = EPERM;\n\treturn -1;','return syscall(SYS_linkat, fd1, existing, fd2, new, flag);'),
 'link-emlink':('link.c','\terrno = EPERM;\n\treturn -1;','return __syscall_ret(-EMLINK);'),
 'linkat-any-flag':('linkat.c','\tif (flag & ~(AT_SYMLINK_FOLLOW | AT_EMPTY_PATH))\n\t\treturn __syscall_ret(-EINVAL);\n',''),
 'link-eperm-via-seam':('link.c','\terrno = EPERM;\n\treturn -1;','return __syscall_ret(-EPERM);'),
 'linkat-eperm-via-seam':('linkat.c','\terrno = EPERM;\n\treturn -1;','return __syscall_ret(-EPERM);'),
 'linkat-no-empty-path':('linkat.c','flag & ~(AT_SYMLINK_FOLLOW | AT_EMPTY_PATH)','flag & ~AT_SYMLINK_FOLLOW'),
}
p=argparse.ArgumentParser(description=__doc__);p.add_argument('musl',type=Path);p.add_argument('output',type=Path);p.add_argument('--cc',default='/opt/homebrew/opt/llvm/bin/clang');p.add_argument('--mutant',choices=sorted(MUTANTS));a=p.parse_args();r=Path(__file__).resolve().parent.parent
a.output.mkdir(parents=True,exist_ok=False);inputs=[Path(__file__),r/'tools/host-tests/pouch-link.c']
def one(path,pattern,base=10):
 found=re.findall(pattern,path.read_text());assert len(found)==1,(path,pattern,found);return int(found[0],base)
fcntl=a.musl/'include/fcntl.h';inputs.append(fcntl)
defs=['-DAT_FDCWD=(%d)'%one(fcntl,r'(?m)^#define AT_FDCWD \((-\d+)\)$')]
for name in ['AT_SYMLINK_NOFOLLOW','AT_REMOVEDIR','AT_SYMLINK_FOLLOW','AT_NO_AUTOMOUNT','AT_EMPTY_PATH']:
 defs.append('-D%s=%d'%(name,one(fcntl,r'(?m)^#define '+name+r' (0x[0-9a-fA-F]+)$',16)))
bits=a.musl/'arch/aarch64/bits/syscall.h.in';inputs.append(bits)
defs.append('-DSYS_linkat=%d'%one(bits,r'(?m)^#define __NR_linkat\s+(0x[0-9a-fA-F]+|\d+)$',0))
for source,dest in [('src/unistd/link.c','link-under-test.c'),('src/unistd/linkat.c','linkat-under-test.c')]:
 f=a.musl/source;inputs.append(f);s=f.read_text()
 for h in ['"syscall.h"','<fcntl.h>']:s=re.sub(r'(?m)^#include '+re.escape(h),'/* host boundary */',s)
 s=re.sub(r'(?m)^#define _BSD_SOURCE$','/* host boundary */',s)
 m=MUTANTS.get(a.mutant)
 if m and source.endswith('/'+m[0]):
  assert s.count(m[1])==1,m;s=s.replace(m[1],m[2])
 (a.output/dest).write_text(s)
stage_syscall_ret(a.musl,a.output,a.cc,inputs)
(a.output/'inputs.json').write_text(json.dumps({str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs},indent=2)+'\n')
sdk=['-isysroot',subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()] if platform.system()=='Darwin' else []
for opt in ('0','2'):
 exe=a.output.resolve()/('test-O'+opt);cmd=[a.cc,*sdk,'-O'+opt,'-g','-fsanitize=address,undefined','-Wall','-Wextra','-Wno-unused-parameter','-Werror',*defs,'-I'+str(a.output.resolve()),str(inputs[1]),'-o',str(exe)]
 (a.output/f'compile-O{opt}.json').write_text(json.dumps(cmd)+'\n')
 with (a.output/f'compile-O{opt}.log').open('w') as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
 result=subprocess.run([str(exe)],capture_output=True,text=True);(a.output/f'run-O{opt}.log').write_text(result.stdout+result.stderr);print(result.stdout,end='')
 assert 'AddressSanitizer' not in result.stderr and 'runtime error:' not in result.stderr
 assert result.returncode==(1 if a.mutant else 0)
