#!/usr/bin/env python3
"""Exercise Pouch's fstat, ioctl and isatty against a mocked kernel SYS_fstat: EBADF for a dead descriptor."""
if not __debug__:raise SystemExit(__file__+': run without -O or PYTHONOPTIMIZE: its verdicts are asserts')
import argparse
import hashlib
import json
import platform
import re
import subprocess
from pathlib import Path

MUTANTS = {
    'refuse-enotty': [('ioctl.c', '\terrno = dead ? EBADF : ENOTTY;\n', '\terrno = dead ? ENOTTY : ENOTTY;\n')],
    'refuse-ebadf': [('ioctl.c', '\terrno = dead ? EBADF : ENOTTY;\n', '\terrno = dead ? EBADF : EBADF;\n')],
    'tag-asks-kernel': [('ioctl.c', '\telse if (fd & POUCH_SOCK_TAG) dead = !pouch_sock_resolve(fd);\n', '')],
    'negative-asks-kernel': [('ioctl.c', '\tif (fd < 0) dead = 1;\n\telse if', '\tif (0) dead = 1;\n\telse if')],
    'winsz-site-enotty': [('ioctl.c', '\t\t\t\treturn 0;\n\t\t\t}\n\t\t\treturn refuse_fd(fd);\n',
                           '\t\t\t\treturn 0;\n\t\t\t}\n\t\t\terrno = ENOTTY; return -1;\n')],
    'null-before-fd': [('ioctl.c', '\t\tif (pts_resolve(fd, &n, &fk) != 0) return refuse_fd(fd);\n\t\tif (!tio) { errno = EFAULT; return -1; }\n\t\tif (pts_ctl_read',
                        '\t\tif (!tio) { errno = EINVAL; return -1; }\n\t\tif (pts_resolve(fd, &n, &fk) != 0) return refuse_fd(fd);\n\t\tif (pts_ctl_read')],
    'cons-null-eperm': [('ioctl.c', 'errno = ws ? EPERM : EFAULT;', 'errno = EPERM;')],
    'fstat-flat': [('fstat.c', '\tif (rc < 0)    return __syscall_ret(rc);\n', '\tif (rc < 0)    return __syscall_ret(-1);\n')],
}
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('musl', type=Path)
p.add_argument('output', type=Path)
p.add_argument('--cc', default='/opt/homebrew/opt/llvm/bin/clang')
p.add_argument('--mutant', choices=sorted(MUTANTS))
a = p.parse_args()
r = Path(__file__).resolve().parent.parent
a.output.mkdir(parents=True, exist_ok=False)
inputs = [Path(__file__), r/'tools/host-tests/pouch-fstat-ebadf.c']

def read(rel):
    f = a.musl/rel
    inputs.append(f)
    return f.read_text()

def under_test(rel, dest):
    s = read(rel)
    for file, old, new in MUTANTS.get(a.mutant, []):
        if rel.endswith('/'+file):
            assert s.count(old) == 1, (a.mutant, old)
            s = s.replace(old, new)
    s = re.sub(r'(?m)^#include "syscall\.h"$', '/* host boundary: harness dispatch */', s)
    s = re.sub(r'(?m)^#include <sys/stat\.h>$', '#include "linux-stat.h"', s)
    s = re.sub(r'(?m)^#include <(?:sys/ioctl|termios)\.h>$', '#include "linux-tty.h"', s)
    (a.output/dest).write_text(s)

def bits(name):
    """The aarch64 header musl's include order picks: the arch's own, else generic."""
    own = a.musl/'arch/aarch64/bits'/name
    return read(str((own if own.exists() else a.musl/'arch/generic/bits'/name).relative_to(a.musl)))

# Syscall numbers as the preprocessor leaves them: syscall.h.in redefines some (fstat 0xFFFF, then 50).
sysin = a.musl/'arch/aarch64/bits/syscall.h.in'
inputs.append(sysin)
macros = subprocess.run([a.cc, '-E', '-dM', '-x', 'c', str(sysin)], capture_output=True, text=True, check=True).stdout
final = dict(re.findall(r'(?m)^#define __NR_(\w+) (\S+)$', macros))
unimpl = re.findall(r'(?m)^#define POUCH_SYSCALL_UNIMPL\s+(\S+)$', read('arch/aarch64/syscall_arch.h'))
assert len(unimpl) == 1, unimpl
nums = ['#define POUCH_SYSCALL_UNIMPL %s' % unimpl[0]]
for name in ('fstat', 'write', 'io_setup', 'tty_acquire', 'tty_set_fg', 'tty_get_fg'):
    assert name in final, name
    nums.append('#define SYS_%s %s' % (name, final[name]))
assert int(final['fstat'], 0) != int(unimpl[0], 0), 'SYS_fstat is still the unimplemented sentinel'
(a.output/'sysnums.h').write_text('\n'.join(nums)+'\n')

sh = read('src/internal/syscall.h')
disp = re.findall(r'(?m)^#define (?:__SYSCALL_(?:NARGS_X|NARGS|CONCAT_X|CONCAT|DISP)\b|__syscall\(\.\.\.\)).*$', sh)
assert len(disp) == 6, disp
(a.output/'dispatch-under-test.h').write_text('\n'.join(disp)+'\n')

# musl's aarch64 struct stat over the host's member types; macOS spells __unused as a macro.
stat = re.sub(r'\b__unused\b', '__unused_pad', bits('stat.h'))
(a.output/'linux-stat.h').write_text('#ifndef LINUX_STAT_H\n#define LINUX_STAT_H\n'+stat+'#endif\n')
termios = read('include/termios.h')
types = re.findall(r'(?m)^typedef unsigned (?:char|int) (?:cc_t|speed_t|tcflag_t);$|^#define NCCS \d+$', termios)
assert len(types) == 4, types
winsize = re.findall(r'(?m)^STRUCT winsize \{.*\};$', read('include/alltypes.h.in'))
assert len(winsize) == 1, winsize
ioctl = bits('ioctl.h')
assert ioctl.count('#include <bits/ioctl_fix.h>\n') == 1
ioctl = ioctl.replace('#include <bits/ioctl_fix.h>\n', bits('ioctl_fix.h'))
(a.output/'linux-tty.h').write_text('#ifndef LINUX_TTY_H\n#define LINUX_TTY_H\n'+'\n'.join(types)+'\n'+bits('termios.h')+
                                    ioctl+winsize[0].replace('STRUCT', 'struct', 1)+'\n#endif\n')

for rel, dest in [('src/stat/fstat.c', 'fstat-under-test.c'), ('src/misc/ioctl.c', 'ioctl-under-test.c'),
                  ('src/unistd/isatty.c', 'isatty-under-test.c'), ('src/internal/syscall_ret.c', 'syscall-ret-under-test.c')]:
    under_test(rel, dest)
for rel in ('src/internal/_pouch_fd.h', 'src/internal/_pouch_socket.h'):
    (a.output/Path(rel).name).write_text(read(rel))

(a.output/'inputs.json').write_text(json.dumps({str(x): hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs}, indent=2)+'\n')
sdk = ['-isysroot', subprocess.check_output(['xcrun', '--show-sdk-path'], text=True).strip()] if platform.system() == 'Darwin' else []
for opt in ('0', '2'):
    exe = a.output.resolve()/('test-O'+opt)
    cmd = [a.cc, *sdk, '-std=c11', '-O'+opt, '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-Wall', '-Wextra', '-Werror',
           '-I'+str(a.output.resolve()), str(inputs[1]), '-o', str(exe)]
    (a.output/f'compile-O{opt}.json').write_text(json.dumps(cmd)+'\n')
    with (a.output/f'compile-O{opt}.log').open('w') as log:
        subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, check=True)
    result = subprocess.run([str(exe)], capture_output=True, text=True)
    (a.output/f'run-O{opt}.log').write_text(result.stdout+result.stderr)
    print(result.stdout, end='')
    assert 'AddressSanitizer' not in result.stderr and 'runtime error:' not in result.stderr
    assert result.returncode in (0, 1), result.returncode
    if a.mutant:
        assert result.returncode == 1 and 'FAIL ' in result.stdout, a.mutant
    else:
        assert result.returncode == 0 and 'FAIL ' not in result.stdout
