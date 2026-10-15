#!/usr/bin/env python3
"""Exercise actual Pouch descriptor adapters with mocked native/socket owners."""
import argparse,hashlib,json,platform,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('musl',type=Path);p.add_argument('output',type=Path);p.add_argument('--mutant',choices=['flags','tag','legacy','dup-floor','append-flip','omode-shift','oexec','pipe2-cloexec','pipe2-leak','pipe2-cancellable','setfl-fd','dupfd-source']);a=p.parse_args();r=Path(__file__).resolve().parent.parent
a.output.mkdir(parents=True,exist_ok=False);inputs=[Path(__file__),r/'tools/host-tests/pouch-descriptor-ops.c']
for source,dest in [('src/internal/_pouch_descriptor.h','descriptor-under-test.h'),('src/fcntl/fcntl.c','fcntl-under-test.c'),('src/unistd/dup.c','dup-under-test.c'),('src/unistd/dup2.c','dup2-under-test.c'),('src/unistd/dup3.c','dup3-under-test.c'),('src/unistd/pipe2.c','pipe2-under-test.c')]:
 f=a.musl/source;inputs.append(f);s=f.read_text()
 for h in ['syscall.h','_pouch_descriptor.h','_pouch_socket.h','_pouch_process.h']:s=s.replace('#include "'+h+'"','/* host boundary */')
 if a.mutant=='flags' and 'dup3.c' in source:s=s.replace('old, new, flags ? 1 : 0','old, new, 0')
 if a.mutant=='tag' and source.endswith('.h'):s=s.replace('if (fd & POUCH_SOCK_TAG)','if (0 && (fd & POUCH_SOCK_TAG))')
 if a.mutant=='legacy' and source.endswith('.h'):s=s.replace('result == -1 ? -ENOSYS : result','result')
 m={'dup-floor':('dup.c','fd, 0, POUCH_HANDLE_DUP_LOWEST','fd, 0, 0'),'append-flip':('fcntl.c','!(arg & O_APPEND) != !(ret & POUCH_HANDLE_APPEND)','0'),'omode-shift':('fcntl.c','>> POUCH_HANDLE_OMODE_SHIFT','>> 0'),'oexec':('fcntl.c','mode == 2 ? O_RDWR : O_RDONLY','mode == 2 ? O_RDWR : mode'),'setfl-fd':('fcntl.c','__syscall2(SYS_thyla_set_nonblock, fd,','__syscall2(SYS_thyla_set_nonblock, fd + 1,'),'dupfd-source':('fcntl.c','__syscall3(SYS_thyla_handle_dup_to, fd, (long)arg,','__syscall3(SYS_thyla_handle_dup_to, 0, (long)arg,'),'pipe2-cloexec':('pipe2.c','if (flag & O_CLOEXEC)\n','if (0)\n'),'pipe2-leak':('pipe2.c','__syscall(SYS_close, p[0]);\n\t\t\t__syscall(SYS_close, p[1]);\n',''),'pipe2-cancellable':('pipe2.c','__syscall(SYS_close, p[0]);','close(p[0]);')}.get(a.mutant)
 if m and source.endswith('/'+m[0]):
  assert s.count(m[1])==1,m;s=s.replace(m[1],m[2])
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
