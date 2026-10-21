#!/usr/bin/env python3
"""Exercise Pouch lstat, readlink, lchmod and the open link flags with a mocked kernel and the series' own __syscall_ret."""
if not __debug__:raise SystemExit(__file__+': run without -O or PYTHONOPTIMIZE: its verdicts are asserts')
import argparse,hashlib,json,platform,re,subprocess
from pathlib import Path
from pouch_seam import stage_syscall_ret
MUTANTS={
 'no-nofollow':[('openat.c','\tif (flags & O_NOFOLLOW) omode |= POUCH_ONOFOLLOW;\n','')],
 'no-opath':[('openat.c','\tif (flags & O_PATH) {\n\t\tomode = POUCH_OPATH;\n\t} else switch','\tswitch')],
 'opath-mask-gone':[('openat.c','\tif (flags & O_PATH)\n\t\tflags &= O_PATH | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW;\n','')],
 'opath-mask-late':[('openat.c','\tif (flags & O_PATH)\n\t\tflags &= O_PATH | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW;\n\n',''),
                    ('openat.c','return -ENOTSUP;\n\n\tunsigned long omode;','return -ENOTSUP;\n\tif (flags & O_PATH)\n\t\tflags &= O_PATH | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW;\n\n\tunsigned long omode;')],
 'creat-dir-allowed':[('openat.c','\tif ((flags & (O_CREAT | O_DIRECTORY)) == (O_CREAT | O_DIRECTORY))\n\t\treturn -EINVAL;\n','')],
 'no-odir-check':[('openat.c','\t\tif (filename[len - 1] != \'/\') {\n','\t\tif (0) {\n')],
 'odir-slash-fstat':[('openat.c','\t\tif (filename[len - 1] != \'/\') {\n','\t\tif (1) {\n')],
 'odir-trunc-sent':[('openat.c','((flags & O_TRUNC) && !odir ? POUCH_OTRUNC : 0u)','((flags & O_TRUNC) ? POUCH_OTRUNC : 0u)')],
 'odir-trunc-opens':[('openat.c','\t\tif (!e && (flags & O_TRUNC)) e = -EISDIR;\n','')],
 'odir-ebadf-close':[('openat.c','\t\t\tif (e != -EBADF) __syscall(SYS_close, r);\n','\t\t\t__syscall(SYS_close, r);\n')],
 'odir-no-close':[('openat.c','\t\t\tif (e != -EBADF) __syscall(SYS_close, r);\n','')],
 'no-slash':[('openat.c','\tif (odir && !(flags & O_NOFOLLOW) && filename[len - 1] != \'/\' &&\n','\tif (0 &&\n')],
 'slash-nofollow':[('openat.c','\tif (odir && !(flags & O_NOFOLLOW) && filename[len - 1] != \'/\' &&\n','\tif (odir && filename[len - 1] != \'/\' &&\n')],
 'lstat-follows':[('fstatat.c','(flag & AT_SYMLINK_NOFOLLOW) ? SYS_thyla_lstat : SYS_thyla_stat','SYS_thyla_stat')],
 'fstatat-any-flag':[('fstatat.c','\tif (flag & ~(AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT | AT_EMPTY_PATH |\n\t             AT_STATX_SYNC_TYPE))\n\t\treturn __syscall_ret(-EINVAL);\n','')],
 'stat-no-toolong':[('fstatat.c','\tif (len > POUCH_PATH_MAX) return __syscall_ret(-ENAMETOOLONG);\n','')],
 'lchmod-follows':[('fchmodat.c','\tif (flag & AT_SYMLINK_NOFOLLOW) omode |= POUCH_ONOFOLLOW;\n','')],
 'lchmod-no-check':[('fchmodat.c','\t\telse if (S_ISLNK(st.st_mode)) r = -EOPNOTSUPP;\n','')],
 'fchmodat-any-flag':[('fchmodat.c','\tif (flag & ~AT_SYMLINK_NOFOLLOW) return __syscall_ret(-EINVAL);\n','')],
 'readlink-not-native':[('readlinkat.c','\tr = __syscall(SYS_thyla_readlink, path, len, buf, bufsize);\n','\tr = -EINVAL;\n')],
 'readlink-dummy-count':[('readlinkat.c','\tif (buf == dummy && r > 0) r = 0;\n','')],
}
p=argparse.ArgumentParser(description=__doc__);p.add_argument('musl',type=Path);p.add_argument('output',type=Path);p.add_argument('--cc',default='/opt/homebrew/opt/llvm/bin/clang');p.add_argument('--mutant',choices=sorted(MUTANTS));a=p.parse_args();r=Path(__file__).resolve().parent.parent
a.output.mkdir(parents=True,exist_ok=False);inputs=[Path(__file__),r/'tools/host-tests/pouch-symlinks.c']
def one(path,pattern,base=10):
 found=re.findall(pattern,path.read_text());assert len(found)==1,(path,pattern,found);return int(found[0],base)
kern=r/'kernel/include/thylacine/syscall.h';inputs.append(kern)
defs=['-DKERNEL_SYS_STAT=%d'%one(kern,r'\bSYS_STAT\s*=\s*(\d+)'),'-DKERNEL_SYS_LSTAT=%d'%one(kern,r'\bSYS_LSTAT\s*=\s*(\d+)'),'-DKERNEL_SYS_READLINK=%d'%one(kern,r'\bSYS_READLINK\s*=\s*(\d+)'),
      '-DKERNEL_WALK_OPEN_NOFOLLOW=%d'%one(kern,r'(?m)^#define SYS_WALK_OPEN_NOFOLLOW\s+(0x[0-9a-fA-F]+)u',16),'-DKERNEL_WALK_OPEN_OPATH=%d'%one(kern,r'(?m)^#define SYS_WALK_OPEN_OPATH\s+(0x[0-9a-fA-F]+)u',16),
      '-DKERNEL_OPEN_PATH_MAX=%d'%one(kern,r'(?m)^#define SYS_OPEN_PATH_MAX\s+(\d+)u')]
bits=a.musl/'arch/aarch64/bits/syscall.h.in';inputs.append(bits)
for name in ['close','read','lseek']:defs.append('-DSYS_%s=%d'%(name,one(bits,r'(?m)^#define __NR_'+name+r'\s+(\d+)$')))
defs.append('-DSYS_thyla_handle_flags_set=%d'%one(bits,r'(?m)^#define SYS_thyla_handle_flags_set\s+(\d+)$'))
for source,dest in [('src/internal/_pouch_fs.h','fs-under-test.h'),('src/internal/_pouch_descriptor.h','descriptor-under-test.h'),('src/fcntl/openat.c','openat-under-test.c'),('src/stat/fstatat.c','fstatat-under-test.c'),('src/stat/fchmodat.c','fchmodat-under-test.c'),('src/unistd/readlinkat.c','readlinkat-under-test.c')]:
 f=a.musl/source;inputs.append(f);s=f.read_text()
 for h in ['syscall.h','_pouch_descriptor.h','_pouch_socket.h','_pouch_fs.h']:s=re.sub(r'(?m)^#include "'+re.escape(h)+'"','/* host boundary */',s)
 for file,old,new in MUTANTS.get(a.mutant,[]):
  if source.endswith('/'+file):
   assert s.count(old)==1,(a.mutant,old);s=s.replace(old,new)
 (a.output/dest).write_text(s)
stage_syscall_ret(a.musl,a.output,a.cc,inputs)
(a.output/'inputs.json').write_text(json.dumps({str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs},indent=2)+'\n')
sdk=['-isysroot',subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()] if platform.system()=='Darwin' else []
for opt in ('0','2'):
 exe=a.output.resolve()/('test-O'+opt);cmd=[a.cc,*sdk,'-O'+opt,'-g','-fsanitize=address,undefined','-Wall','-Wextra','-Werror',*defs,'-I'+str(a.output.resolve()),str(inputs[1]),'-o',str(exe)]
 (a.output/f'compile-O{opt}.json').write_text(json.dumps(cmd)+'\n')
 with (a.output/f'compile-O{opt}.log').open('w') as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
 result=subprocess.run([str(exe)],capture_output=True,text=True);(a.output/f'run-O{opt}.log').write_text(result.stdout+result.stderr);print(result.stdout,end='')
 assert 'AddressSanitizer' not in result.stderr and 'runtime error:' not in result.stderr
 assert result.returncode==(1 if a.mutant else 0)
