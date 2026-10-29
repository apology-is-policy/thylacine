#!/usr/bin/env python3
"""Exercise actual Pouch descriptor adapters with mocked native/socket owners."""
if not __debug__:raise SystemExit(__file__+': run without -O or PYTHONOPTIMIZE: its verdicts are asserts')
import argparse,hashlib,json,platform,re,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('musl',type=Path);p.add_argument('output',type=Path);p.add_argument('--cc',default='/opt/homebrew/opt/llvm/bin/clang');p.add_argument('--mutant',choices=['flags','tag','legacy','dup-floor','append-flip','omode-shift','oexec','pipe2-cloexec','pipe2-leak','pipe2-cancellable','pipe2-close-gone','pipe2-ungated-nonblock','setfl-fd','dupfd-source','cloexec-tag','lock-proc','lock-ofd-pid','lock-neg-len','lock-whence-end','lock-getlk-unchanged','lock-cp','lock-refuse-probe','lock-getlk-len','lock-overflow','lock-type-check','flock-nb']);a=p.parse_args();r=Path(__file__).resolve().parent.parent
a.output.mkdir(parents=True,exist_ok=False);inputs=[Path(__file__),r/'tools/host-tests/pouch-descriptor-ops.c']
for source,dest in [('src/internal/_pouch_descriptor.h','descriptor-under-test.h'),('src/fcntl/fcntl.c','fcntl-under-test.c'),('src/unistd/dup.c','dup-under-test.c'),('src/unistd/dup2.c','dup2-under-test.c'),('src/unistd/dup3.c','dup3-under-test.c'),('src/unistd/pipe2.c','pipe2-under-test.c'),('src/linux/flock.c','flock-under-test.c')]:
 f=a.musl/source;inputs.append(f);s=f.read_text()
 for h in ['syscall.h','_pouch_descriptor.h','_pouch_socket.h','_pouch_process.h']:s=s.replace('#include "'+h+'"','/* host boundary */')
 if a.mutant=='flags' and 'dup3.c' in source:s=s.replace('old, new, flags ? 1 : 0','old, new, 0')
 if a.mutant=='tag' and source.endswith('.h'):s=s.replace('if (fd & POUCH_SOCK_TAG)','if (0 && (fd & POUCH_SOCK_TAG))')
 if a.mutant=='legacy' and source.endswith('.h'):s=s.replace('result == -1 ? -ENOSYS : result','result')
 m={'dup-floor':('dup.c','fd, 0, POUCH_HANDLE_DUP_LOWEST','fd, 0, 0'),'append-flip':('fcntl.c','!(arg & O_APPEND) != !(ret & POUCH_HANDLE_APPEND)','0'),'omode-shift':('fcntl.c','>> POUCH_HANDLE_OMODE_SHIFT','>> 0'),'oexec':('fcntl.c','mode == 2 ? O_RDWR : O_RDONLY','mode == 2 ? O_RDWR : mode'),'setfl-fd':('fcntl.c','__syscall2(SYS_thyla_set_nonblock, fd,','__syscall2(SYS_thyla_set_nonblock, fd + 1,'),'dupfd-source':('fcntl.c','__syscall3(SYS_thyla_handle_dup_to, fd, (long)arg,','__syscall3(SYS_thyla_handle_dup_to, 0, (long)arg,'),'pipe2-cloexec':('pipe2.c','if (flag & O_CLOEXEC)\n','if (0)\n'),'pipe2-leak':('pipe2.c','\t\t\tif (r != -EBADF || i != 0) __syscall(SYS_close, p[0]);\n\t\t\tif (r != -EBADF || i != 1) __syscall(SYS_close, p[1]);\n',''),'pipe2-close-gone':('pipe2.c','if (r != -EBADF || i != 0) __syscall(SYS_close, p[0]);\n\t\t\tif (r != -EBADF || i != 1) __syscall(SYS_close, p[1]);','__syscall(SYS_close, p[0]);\n\t\t\t__syscall(SYS_close, p[1]);'),'cloexec-tag':('_pouch_descriptor.h','    int e = pouch_descriptor_check(fd);\n    if (e) return -e;\n',''),'pipe2-cancellable':('pipe2.c','__syscall(SYS_close, p[0]);','close(p[0]);'),'pipe2-ungated-nonblock':('pipe2.c','if (!r && (flag & O_NONBLOCK))','if ((flag & O_NONBLOCK))'),'lock-proc':('fcntl.c','(ofd ? 0 : POUCH_FILE_LOCK_PROC)','0'),'lock-ofd-pid':('fcntl.c','if (ofd && fl->l_pid != 0) {','if (0) {'),'lock-neg-len':('fcntl.c','\t\tstart += fl->l_len;\n',''),'lock-whence-end':('fcntl.c','base = st.st_size;','base = 0;'),'lock-getlk-unchanged':('fcntl.c','fl->l_type = F_UNLCK;\n\t\t\treturn 0;','t = F_UNLCK; break;'),'lock-cp':('fcntl.c','ret = __syscall_cp(SYS_thyla_file_lock,','ret = __syscall5(SYS_thyla_file_lock,'),'lock-refuse-probe':('_pouch_descriptor.h','    if (__syscall1(SYS_thyla_handle_flags_get, fd) == -EBADF) e = EBADF;\n','    (void)fd;\n'),'lock-getlk-len':('fcntl.c','ans.len == 0 || ans.len - 1 >= LLONG_MAX - ans.start','ans.len == 0'),'lock-overflow':('fcntl.c','if (fl->l_start > LLONG_MAX - base) return pouch_lock_refuse(fd, EOVERFLOW);\n',''),'lock-type-check':('fcntl.c','\tif (type < 0) return pouch_lock_refuse(fd, EINVAL);\n\tif (ofd','\tif (ofd'),'flock-nb':('flock.c','if (!(op & LOCK_NB) && type','if (type')}.get(a.mutant)
 if m and source.endswith('/'+m[0]):
  assert s.count(m[1])==1,m;s=s.replace(m[1],m[2])
 (a.output/dest).write_text(s)
# The lock ABI the adapter must match, read from the kernel's own header.
k=r/'kernel/include/thylacine/syscall.h';inputs.append(k);ks=k.read_text();kd={}
kd['K_SYS_FILE_LOCK']=int(re.findall(r'^\s*SYS_FILE_LOCK\s*=\s*(\d+),',ks,re.M)[0])
simple=dict(re.findall(r'^#define SYS_FILE_LOCK_(RD|WR|UN|TYPE_MASK|WAIT|TEST|PROC)\s+(0x[0-9a-fA-F]+|\d+)u\b',ks,re.M));assert len(simple)==7,simple
for n,v in simple.items():kd['K_'+n]=int(v,0)
fa=re.search(r'^#define SYS_FILE_LOCK_FLAGS_ALL\s+\(([^)]*)\)',ks,re.M|re.S).group(1).replace('\\',' ').replace('\n',' ')
kd['K_FLAGS_ALL']=eval(re.sub(r'SYS_FILE_LOCK_(\w+)',lambda m:str(kd['K_'+m.group(1)]),fa),{'__builtins__':{}})
kd['K_SIZE']=int(re.findall(r'sizeof\(struct t_file_lock\)\s*==\s*(\d+)',ks)[0])
offs=dict(re.findall(r'offsetof\(struct t_file_lock,\s*(\w+)\)\s*==\s*(\d+)',ks));assert set(offs)=={'type','start','len','pid'},offs
for n,v in offs.items():kd['K_OFF_'+n]=int(v)
assert kd['K_FLAGS_ALL']==kd['K_WAIT']|kd['K_TEST']|kd['K_PROC'],kd
(a.output/'inputs.json').write_text(json.dumps({str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs},indent=2)+'\n')
sdk=['-isysroot',subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()] if platform.system()=='Darwin' else []
for opt in ('0','2'):
 exe=a.output.resolve()/('test-O'+opt);cmd=[a.cc,*sdk,'-O'+opt,'-g','-fsanitize=address,undefined','-Wall','-Wextra','-Werror',*['-D%s=%d'%kv for kv in sorted(kd.items())],'-I'+str(a.output.resolve()),str(inputs[1]),'-o',str(exe)]
 (a.output/f'compile-O{opt}.json').write_text(json.dumps(cmd)+'\n')
 with (a.output/f'compile-O{opt}.log').open('w') as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
 result=subprocess.run([str(exe)],capture_output=True,text=True);(a.output/f'run-O{opt}.log').write_text(result.stdout+result.stderr);print(result.stdout,end='')
 assert 'AddressSanitizer' not in result.stderr and 'runtime error:' not in result.stderr
 assert result.returncode==(1 if a.mutant else 0)
