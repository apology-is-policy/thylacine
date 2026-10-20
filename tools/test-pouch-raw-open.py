#!/usr/bin/env python3
"""Exercise Pouch's raw opens (musl's sys_open family), __map_file and fopen with a mocked kernel."""
if not __debug__:raise SystemExit(__file__+': run without -O or PYTHONOPTIMIZE: its verdicts are asserts')
import argparse,hashlib,json,platform,re,subprocess
from pathlib import Path
UPSTREAM_OPEN='''#ifdef SYS_open
#define __sys_open2(x,pn,fl) __syscall2(SYS_open, pn, (fl)|O_LARGEFILE)
#define __sys_open3(x,pn,fl,mo) __syscall3(SYS_open, pn, (fl)|O_LARGEFILE, mo)
#define __sys_open_cp2(x,pn,fl) __syscall_cp2(SYS_open, pn, (fl)|O_LARGEFILE)
#define __sys_open_cp3(x,pn,fl,mo) __syscall_cp3(SYS_open, pn, (fl)|O_LARGEFILE, mo)
#else
#define __sys_open2(x,pn,fl) __syscall3(SYS_openat, AT_FDCWD, pn, (fl)|O_LARGEFILE)
#define __sys_open3(x,pn,fl,mo) __syscall4(SYS_openat, AT_FDCWD, pn, (fl)|O_LARGEFILE, mo)
#define __sys_open_cp2(x,pn,fl) __syscall_cp3(SYS_openat, AT_FDCWD, pn, (fl)|O_LARGEFILE)
#define __sys_open_cp3(x,pn,fl,mo) __syscall_cp4(SYS_openat, AT_FDCWD, pn, (fl)|O_LARGEFILE, mo)
#endif
'''
MUTANTS={
 'macro-upstream':[('syscall.h',re.compile(r'(?s)/\* pouch: SYS_open is the native open.*?#define __sys_open_cp3\(x,pn,fl,mo\) __pouch_sys_open\(pn, fl, mo\)\n'),UPSTREAM_OPEN)],
 'macro-cp-upstream':[('syscall.h','#define __sys_open_cp2(x,pn,fl) __pouch_sys_open(pn, fl, 0)\n#define __sys_open_cp3(x,pn,fl,mo) __pouch_sys_open(pn, fl, mo)\n',
                       '#define __sys_open_cp2(x,pn,fl) __syscall_cp2(SYS_open, pn, (fl)|O_LARGEFILE)\n#define __sys_open_cp3(x,pn,fl,mo) __syscall_cp3(SYS_open, pn, (fl)|O_LARGEFILE, mo)\n')],
 'macro-mode-dropped':[('syscall.h','#define __sys_open3(x,pn,fl,mo) __pouch_sys_open(pn, fl, mo)\n','#define __sys_open3(x,pn,fl,mo) __pouch_sys_open(pn, fl, 0)\n')],
 'raw-fstat-errno':[('openat.c','\t\t\terrno = saved;\n','\t\t\t(void)saved;\n')],
 'raw-ret-sets-errno':[('openat.c','\t\t\tif (s != -EBADF) __syscall(SYS_close, r);\n\t\t\treturn s;\n','\t\t\tif (s != -EBADF) __syscall(SYS_close, r);\n\t\t\treturn __syscall_ret(s);\n'),
                       ('openat.c','\t\t__syscall(SYS_lseek, r, 0, SEEK_END);\n\treturn r;\n','\t\t__syscall(SYS_lseek, r, 0, SEEK_END);\n\treturn __syscall_ret(r);\n')],
 'odir-public-fstat':[('openat.c','\t\t\te = __fstat((int)r, &st) ? -errno','\t\t\te = fstat((int)r, &st) ? -errno')],
 'raw-refusal-sets-errno':[('openat.c','\tdefault:       return -EINVAL;\n','\tdefault:       errno = EINVAL; return -EINVAL;\n')],
 'wrapper-raw':[('openat.c','\treturn __syscall_ret(__pouch_sys_open(filename, flags, mode));\n','\treturn __pouch_sys_open(filename, flags, mode);\n')],
 'map-shared':[('__map_file.c','PROT_READ, MAP_PRIVATE, fd, 0);','PROT_READ, MAP_SHARED, fd, 0);')],
 'map-no-close':[('__map_file.c','\t__syscall(SYS_close, fd);\n','')],
 'fopen-open':[('fopen.c','\tfd = sys_open(filename, flags, 0666);\n','\tfd = open(filename, flags, 0666);\n')],
 'rb-ca-open':[('__fopen_rb_ca.c','\tf->fd = sys_open(filename, O_RDONLY|O_CLOEXEC);\n','\tf->fd = open(filename, O_RDONLY|O_CLOEXEC);\n')],
 'tmpfile-open':[('tmpfile.c','\t\tfd = sys_open(s, O_RDWR|O_CREAT|O_EXCL, 0600);\n','\t\tfd = open(s, O_RDWR|O_CREAT|O_EXCL, 0600);\n')],
}
p=argparse.ArgumentParser(description=__doc__);p.add_argument('musl',type=Path);p.add_argument('output',type=Path);p.add_argument('--cc',default='/opt/homebrew/opt/llvm/bin/clang');p.add_argument('--mutant',choices=sorted(MUTANTS));a=p.parse_args();r=Path(__file__).resolve().parent.parent
a.output.mkdir(parents=True,exist_ok=False);inputs=[Path(__file__),r/'tools/host-tests/pouch-raw-open.c']
def one(path,pattern,base=10):
 found=re.findall(pattern,path.read_text());assert len(found)==1,(path,pattern,found);return int(found[0],base)
def text(source):
 f=a.musl/source;inputs.append(f);s=f.read_text()
 for file,old,new in MUTANTS.get(a.mutant,[]):
  if source.endswith('/'+file):
   if isinstance(old,str):assert s.count(old)==1,(a.mutant,old);s=s.replace(old,new)
   else:s,k=old.subn(new,s);assert k==1,(a.mutant,old.pattern)
 return s
bits=a.musl/'arch/aarch64/bits/syscall.h.in';inputs.append(bits)
defs=['-DSYS_%s=%d'%(name,one(bits,r'(?m)^#define __NR_'+name+r'\s+(\d+)$')) for name in ['close','lseek','open']]
defs.append('-DSYS_thyla_handle_flags_set=%d'%one(bits,r'(?m)^#define SYS_thyla_handle_flags_set\s+(\d+)$'))
# The open block of internal/syscall.h, between the blocks either side of it, with the dispatch it expands through.
sh=text('src/internal/syscall.h')
disp=re.findall(r'(?m)^#define __SYSCALL_(?:NARGS_X|NARGS|CONCAT_X|CONCAT|DISP)\b.*$',sh);assert len(disp)==5,disp
block=re.findall(r'(?s)\n#define SIOCGSTAMPNS_OLD 0x8907\n#endif\n(.*?)\n#ifdef SYS_wait4\n',sh);assert len(block)==1,'open block not found'
assert block[0].count('#define sys_open(...)')==1 and block[0].count('#define sys_open_cp(...)')==1,block[0]
(a.output/'rawopen-under-test.h').write_text('\n'.join(disp)+'\n'+block[0]+'\n')
for source,dest in [('src/internal/_pouch_fs.h','fs-under-test.h'),('src/internal/_pouch_descriptor.h','descriptor-under-test.h'),('src/fcntl/openat.c','openat-under-test.c'),('src/time/__map_file.c','map-file-under-test.c'),('src/stdio/fopen.c','fopen-under-test.c')]:
 s=text(source)
 for h in ['syscall.h','_pouch_descriptor.h','_pouch_socket.h','_pouch_fs.h','stdio_impl.h']:s=re.sub(r'(?m)^#include "'+re.escape(h)+'"','/* host boundary */',s)
 (a.output/dest).write_text(s)
# The two openers the harness cannot host (musl's FILE internals): each makes one raw open and calls no open().
pin_failures=0
for source in ['src/stdio/__fopen_rb_ca.c','src/stdio/tmpfile.c']:
 code=re.sub(r'(?s)/\*.*?\*/','',text(source))
 ok=code.count('sys_open(')==1 and not re.findall(r'(?<![\w])open\s*\(',code)
 print('%s source-%s'%('PASS' if ok else 'FAIL',Path(source).stem));pin_failures+=not ok
(a.output/'inputs.json').write_text(json.dumps({str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs},indent=2)+'\n')
sdk=['-isysroot',subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()] if platform.system()=='Darwin' else []
for opt in ('0','2'):
 exe=a.output.resolve()/('test-O'+opt);cmd=[a.cc,*sdk,'-O'+opt,'-g','-fsanitize=address,undefined','-Wall','-Wextra','-Werror',*defs,'-I'+str(a.output.resolve()),str(inputs[1]),'-o',str(exe)]
 (a.output/f'compile-O{opt}.json').write_text(json.dumps(cmd)+'\n')
 with (a.output/f'compile-O{opt}.log').open('w') as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
 result=subprocess.run([str(exe)],capture_output=True,text=True);(a.output/f'run-O{opt}.log').write_text(result.stdout+result.stderr);print(result.stdout,end='')
 assert 'AddressSanitizer' not in result.stderr and 'runtime error:' not in result.stderr
 assert result.returncode in (0,1),result.returncode
 assert (result.returncode==1 or pin_failures>0)==bool(a.mutant)
