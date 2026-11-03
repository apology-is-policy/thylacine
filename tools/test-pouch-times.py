#!/usr/bin/env python3
"""Exercise Pouch utimensat, fchmod and fchmodat with a mocked kernel and the series' own __syscall_ret."""
if not __debug__:raise SystemExit(__file__+': run without -O or PYTHONOPTIMIZE: its verdicts are asserts')
import argparse,hashlib,json,platform,re,subprocess
from pathlib import Path
from pouch_seam import stage_syscall_ret
TIMES_CALL='0UL, times), valid);'
MUTANTS={
 'no-omit':[('utimensat.c','\t\t\tif (ns == UTIME_OMIT) continue;\n','')],
 'now-is-explicit':[('utimensat.c','\t\t\tif (ns == UTIME_NOW) valid |= bit;\n','\t\t\tif (ns == UTIME_NOW) valid |= bit | set;\n')],
 'nsec-billion-ok':[('utimensat.c','ns > 999999999L','ns > 1000000000L')],
 'nsec-negative-ok':[('utimensat.c','ns < 0 || ','')],
 'both-omit-sent':[('utimensat.c','\t\tif (!valid) return 0;\n','')],
 'null-not-touch':[('utimensat.c','\tunsigned valid = POUCH_WSTAT_TOUCH;\n','\tunsigned valid = 0;\n')],
 'eperm-on-touch':[('utimensat.c','if (r == -EACCES && valid != POUCH_WSTAT_TOUCH) {','(void)valid; if (r == -EACCES) {')],
 'no-eperm':[('utimensat.c','if (r == -EACCES && valid != POUCH_WSTAT_TOUCH) {','(void)valid; if (0) {')],
 'eperm-via-seam':[('utimensat.c','\t\terrno = EPERM;\n\t\treturn -1;\n','\t\treturn __syscall_ret(-EPERM);\n')],
 'open-error-mapped':[('utimensat.c','\tif (pfd < 0) return __syscall_ret(pfd);\n','\tif (pfd < 0) return answer(pfd, valid);\n')],
 'nofollow-dropped':[('utimensat.c','\tif (flags & AT_SYMLINK_NOFOLLOW) omode |= POUCH_ONOFOLLOW;\n','')],
 'fd-form-any-flag':[('utimensat.c','\t\tif (flags) return __syscall_ret(-EINVAL);\n','')],
 'path-form-any-flag':[('utimensat.c','\tif (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) return __syscall_ret(-EINVAL);\n','')],
 'empty-path-refused':[('utimensat.c','~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)','~AT_SYMLINK_NOFOLLOW')],
 'empty-path-fd-ignored':[('utimensat.c','\t\tif (fd != AT_FDCWD)\n\t\t\treturn answer(','\t\tif (0)\n\t\t\treturn answer(')],
 'empty-path-cwd-not-dot':[('utimensat.c','\t\tpath = ".";\n','')],
 'empty-flag-any-name':[('utimensat.c','(flags & AT_EMPTY_PATH) && !*path','(flags & AT_EMPTY_PATH)')],
 'cwd-null-is-fd-form':[('utimensat.c','\tif (!path && fd != AT_FDCWD) {\n','\tif (!path) {\n')],
 'no-close':[('utimensat.c','\t__syscall(SYS_close, (int)pfd);\n\treturn answer(r, valid);\n','\treturn answer(r, valid);\n')],
 'times-not-sent':[('utimensat.c',TIMES_CALL,'0UL), valid);')],
 'path-times-not-sent':[('utimensat.c','(int)pfd, valid, 0, 0, 0, 0UL, times);','(int)pfd, valid, 0, 0, 0, 0UL);')],
 'fchmod-no-eperm':[('fchmod.c','\tif (r == -EACCES) {\n','\tif (0) {\n')],
 'fchmod-eperm-via-seam':[('fchmod.c','\t\terrno = EPERM;\n\t\treturn -1;\n','\t\treturn __syscall_ret(-EPERM);\n')],
 'fchmodat-no-eperm':[('fchmodat.c','\t\teperm = r == -EACCES;\n','')],
 'fchmodat-fstat-mapped':[('fchmodat.c','\t\tif (fstat((int)pfd, &st)) r = -errno;\n','\t\tif (fstat((int)pfd, &st)) { r = -errno; eperm = r == -EACCES; }\n')],
 'no-bsd-source':[('utimensat.c','#define _BSD_SOURCE\n','')],
}
# Caught by musl's own compile of the patched files, which the mocked build
# below cannot be: it shims the headers, so it cannot see a constant they gate.
ENV_MUTANTS={'no-bsd-source'}
# Each patched object's undefined symbols, all libc's own: a new one (a codegen
# libcall, a helper the series does not build) fails here, not on the device.
UNDEF={'utimensat.c':{'__syscall_ret','___errno_location','strnlen'},'fchmod.c':{'__syscall_ret','___errno_location'},
       'fchmodat.c':{'__syscall_ret','___errno_location','fstat','strnlen'}}
MUSL_CFLAGS=['--target=aarch64-thylacine','-std=c99','-nostdinc','-ffreestanding','-fexcess-precision=standard','-frounding-math',
 '-fno-strict-aliasing','-D_XOPEN_SOURCE=700','-O2','-fomit-frame-pointer','-fno-unwind-tables','-fno-asynchronous-unwind-tables',
 '-ffunction-sections','-fdata-sections','-Werror=implicit-function-declaration','-Werror=implicit-int','-Werror=pointer-sign',
 '-Werror=pointer-arith','-Werror=int-conversion','-Werror=incompatible-pointer-types']
p=argparse.ArgumentParser(description=__doc__);p.add_argument('musl',type=Path);p.add_argument('output',type=Path);p.add_argument('--cc',default='/opt/homebrew/opt/llvm/bin/clang');p.add_argument('--mutant',choices=sorted(MUTANTS));a=p.parse_args();r=Path(__file__).resolve().parent.parent
a.output.mkdir(parents=True,exist_ok=False);inputs=[Path(__file__),r/'tools/host-tests/pouch-times.c']
def one(path,pattern,base=10):
 found=re.findall(pattern,path.read_text());assert len(found)==1,(path,pattern,found);return int(found[0],base)
kern=r/'kernel/include/thylacine/syscall.h';inputs.append(kern)
defs=['-DKERNEL_SYS_WSTAT=%d'%one(kern,r'\bSYS_WSTAT\s*=\s*(\d+)'),
      '-DKERNEL_WALK_OPEN_NOFOLLOW=%d'%one(kern,r'(?m)^#define SYS_WALK_OPEN_NOFOLLOW\s+(0x[0-9a-fA-F]+)u',16),'-DKERNEL_WALK_OPEN_OPATH=%d'%one(kern,r'(?m)^#define SYS_WALK_OPEN_OPATH\s+(0x[0-9a-fA-F]+)u',16),
      '-DKERNEL_OPEN_PATH_MAX=%d'%one(kern,r'(?m)^#define SYS_OPEN_PATH_MAX\s+(\d+)u')]
for name in ['MODE','ATIME','MTIME','ATIME_SET','MTIME_SET']:
 defs.append('-DKERNEL_T_WSTAT_%s=%d'%(name,1<<one(kern,r'(?m)^#define T_WSTAT_'+name+r'\s+\(1u << (\d+)\)$')))
fcntl=a.musl/'include/fcntl.h';stat=a.musl/'include/sys/stat.h';inputs+=[fcntl,stat]
defs+=['-DMUSL_AT_FDCWD=(%d)'%one(fcntl,r'(?m)^#define AT_FDCWD \((-\d+)\)$'),'-DMUSL_AT_SYMLINK_NOFOLLOW=%d'%one(fcntl,r'(?m)^#define AT_SYMLINK_NOFOLLOW (0x[0-9a-fA-F]+)$',16),
      '-DMUSL_AT_EMPTY_PATH=%d'%one(fcntl,r'(?m)^#define AT_EMPTY_PATH (0x[0-9a-fA-F]+)$',16)]
for name in ['UTIME_NOW','UTIME_OMIT']:defs.append('-DMUSL_%s=%d'%(name,one(stat,r'(?m)^#define '+name+r'\s+(0x[0-9a-fA-F]+)$',16)))
# The seam's __syscall0..7, each derived: the sentinel guard first, then argument i bound to register xi.
arch=a.musl/'arch/aarch64/syscall_arch.h';inputs.append(arch);t=arch.read_text()
assert re.findall(r'(?m)^static inline long (__syscall\w*)\(',t)==['__syscall%d'%k for k in range(8)],'the seam must define __syscall0..7'
for k in range(8):
 v='abcdefg'[:k];body=['\tif (n == POUCH_SYSCALL_UNIMPL) return -ENOSYS;','\tregister long x8 __asm__("x8") = n;']
 body+=['\tregister long x0 __asm__("x0");'] if not k else ['\tregister long x%d __asm__("x%d") = %s;'%(i,i,c) for i,c in enumerate(v)]
 body.append('\t__asm_syscall(%s);'%', '.join(['"r"(x8)']+(['"0"(x0)'] if k else [])+['"r"(x%d)'%i for i in range(1,k)]))
 assert t.count('static inline long __syscall%d(long n%s)\n{\n%s\n}\n'%(k,''.join(', long '+c for c in v),'\n'.join(body)))==1,('__syscall%d'%k,'not the derived shape')
bits=a.musl/'arch/aarch64/bits/syscall.h.in';inputs.append(bits)
defs.append('-DSYS_close=%d'%one(bits,r'(?m)^#define __NR_close\s+(\d+)$'))
seen=set();menv=a.output/'musl-env';(menv/'include/bits').mkdir(parents=True);(menv/'src/internal').mkdir(parents=True)
for source,dest in [('src/internal/_pouch_fs.h','fs-under-test.h'),('src/stat/utimensat.c','utimensat-under-test.c'),('src/stat/fchmod.c','fchmod-under-test.c'),('src/stat/fchmodat.c','fchmodat-under-test.c')]:
 f=a.musl/source;inputs.append(f);s=f.read_text()
 for file,old,new in MUTANTS.get(a.mutant,[]):
  if source.endswith('/'+file) and (file,old) not in seen:
   assert s.count(old)>=1,(a.mutant,old);s=s.replace(old,new);seen.add((file,old))
 if source.endswith('.c'):(menv/Path(source).name).write_text(s)
 for h in ['"syscall.h"','"_pouch_fs.h"','<sys/stat.h>','<fcntl.h>']:s=re.sub(r'(?m)^#include '+re.escape(h),'/* host boundary */',s)
 (a.output/dest).write_text(s)
assert not a.mutant or len(seen)==len({(f,o) for f,o,_ in MUTANTS[a.mutant]}),(a.mutant,'a mutation did not apply')
# The patched files as musl builds them for the device: its headers (bits/ generated
# as its Makefile does), its feature-test macros and flags. The behaviour mutants
# skip it; they are the mocked build's to catch.
if not a.mutant or a.mutant in ENV_MUTANTS:
 mk=[a.musl/'tools/mkalltypes.sed',a.musl/'arch/aarch64/bits/alltypes.h.in',a.musl/'include/alltypes.h.in'];inputs+=mk
 (menv/'include/bits/alltypes.h').write_text(subprocess.run(['sed','-f',*map(str,mk)],capture_output=True,text=True,check=True).stdout)
 t=bits.read_text();(menv/'include/bits/syscall.h').write_text(t+''.join(l.replace('__NR_','SYS_',1)+'\n' for l in t.splitlines() if '__NR_' in l))
 (menv/'src/internal/version.h').write_text('#define VERSION "pouch-host-test"\n')
 inc=[a.musl/'arch/aarch64',a.musl/'arch/generic',menv/'src/internal',a.musl/'src/include',a.musl/'src/internal',menv/'include',a.musl/'include']
 tool=lambda n:str(Path(a.cc).with_name(n));refused=[]
 for name,want in UNDEF.items():
  src=menv/name;obj=src.with_suffix('.o')
  c=subprocess.run([a.cc,*MUSL_CFLAGS,*['-I'+str(i) for i in inc],'-c',str(src),'-o',str(obj)],capture_output=True,text=True)
  (menv/(name+'.log')).write_text(c.stdout+c.stderr)
  if c.returncode:refused.append(name);continue
  got={l.split()[-1] for l in subprocess.run([tool('llvm-nm'),'-u',str(obj)],capture_output=True,text=True,check=True).stdout.splitlines() if l.strip()}
  assert got==want,(name,'undefined symbols',sorted(got),'want',sorted(want))
  # SYS_WSTAT must be a trap: a number the seam retargets to its sentinel folds to -ENOSYS with none.
  assert name!='utimensat.c' or re.search(r'\bsvc\b',subprocess.run([tool('llvm-objdump'),'-d',str(obj)],capture_output=True,text=True,check=True).stdout),'utimensat issues no svc'
 if a.mutant:
  assert refused,(a.mutant,"musl's own compile accepted it")
  print('musl-env: mutant %s refused by musl\'s own compile (%s)'%(a.mutant,', '.join(refused)));raise SystemExit(0)
 assert not refused,(refused,"musl's own compile refused the series; see musl-env/*.log")
 print('musl-env: %d patched files compile as musl builds them; undefined symbols libc\'s own; utimensat traps'%len(UNDEF))
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
