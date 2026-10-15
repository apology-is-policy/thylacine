#!/usr/bin/env python3
"""Exercise Pouch openat's close-on-exec arms with a mocked kernel."""
import argparse,hashlib,json,platform,re,subprocess
from pathlib import Path
MUTANTS={
 'no-set':[('if (r >= 0 && (flags & O_CLOEXEC)) {','if (0) {')],
 'close-gone':[('if (s != -EBADF) __syscall(SYS_close, r);','__syscall(SYS_close, r);')],
 'no-close':[('if (s != -EBADF) __syscall(SYS_close, r);','(void)s;')],
 'seek-first':[('\tif (r >= 0 && (flags & O_APPEND))\n\t\t__syscall(SYS_lseek, r, 0, SEEK_END);\n',''),
               ('{\n\tif (r >= 0 && (flags & O_CLOEXEC)) {','{\n\tif (r >= 0 && (flags & O_APPEND))\n\t\t__syscall(SYS_lseek, r, 0, SEEK_END);\n\tif (r >= 0 && (flags & O_CLOEXEC)) {')],
 'create-bypass':[('if (r >= 0) { __syscall(SYS_close, p); return pouch_open_ret(r, flags); }','if (r >= 0) { __syscall(SYS_close, p); return __syscall_ret(r); }')],
 'eexist-bypass':[('\t\t\treturn pouch_open_ret(o, flags);','\t\t\treturn __syscall_ret(o);')],
}
p=argparse.ArgumentParser(description=__doc__);p.add_argument('musl',type=Path);p.add_argument('output',type=Path);p.add_argument('--mutant',choices=sorted(MUTANTS));a=p.parse_args();r=Path(__file__).resolve().parent.parent
a.output.mkdir(parents=True,exist_ok=False);inputs=[Path(__file__),r/'tools/host-tests/pouch-open-cloexec.c']
for source,dest in [('src/internal/_pouch_fs.h','fs-under-test.h'),('src/internal/_pouch_descriptor.h','descriptor-under-test.h'),('src/fcntl/openat.c','openat-under-test.c')]:
 f=a.musl/source;inputs.append(f);s=f.read_text()
 for h in ['syscall.h','_pouch_descriptor.h','_pouch_socket.h','_pouch_fs.h']:s=re.sub(r'(?m)^#include "'+re.escape(h)+'"','/* host boundary */',s)
 if a.mutant and source.endswith('openat.c'):
  for old,new in MUTANTS[a.mutant]:
   assert s.count(old)==1,(a.mutant,old);s=s.replace(old,new)
 (a.output/dest).write_text(s)
(a.output/'inputs.json').write_text(json.dumps({str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs},indent=2)+'\n')
sdk=['-isysroot',subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()] if platform.system()=='Darwin' else []
for opt in ('0','2'):
 exe=a.output.resolve()/('test-O'+opt);cmd=['/opt/homebrew/opt/llvm/bin/clang',*sdk,'-O'+opt,'-g','-fsanitize=address,undefined','-Wall','-Wextra','-Werror','-I'+str(a.output.resolve()),str(inputs[1]),'-o',str(exe)]
 (a.output/f'compile-O{opt}.json').write_text(json.dumps(cmd)+'\n')
 with (a.output/f'compile-O{opt}.log').open('w') as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
 result=subprocess.run([str(exe)],capture_output=True,text=True);(a.output/f'run-O{opt}.log').write_text(result.stdout+result.stderr);print(result.stdout,end='')
 assert 'AddressSanitizer' not in result.stderr and 'runtime error:' not in result.stderr
 assert result.returncode==(1 if a.mutant else 0)
