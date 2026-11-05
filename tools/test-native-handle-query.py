#!/usr/bin/env python3
"""Check query ABI mirrors and exercise the actual native handler with mocks.
Usage: test-native-handle-query.py [--mutant ceiling|append|mirror] OUTPUT_DIRECTORY
The output must be new. This does not emulate handle locking or Vivarium;
run the native guest witness and Linux collision witness for those boundaries.
A mutant must be refused: ceiling moves VIV_NATIVE_CEILING in the text the pins
read, append drops the handler's APPEND bit from the code the test runs, and
mirror moves libthyla-rs's NOTE_WAIT in the text the mirror check reads.
"""
if not __debug__:raise SystemExit(__file__+': run without -O or PYTHONOPTIMIZE: its verdicts are asserts')
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--cc', default='/opt/homebrew/opt/llvm/bin/clang')
p.add_argument('--mutant', choices=['ceiling', 'append', 'mirror'])
a = p.parse_args()
r = Path(__file__).resolve().parent.parent
checks = {
    'kernel/include/thylacine/handle.h': [r'#define PROC_HANDLE_MAX 1024\b'],
    'kernel/include/thylacine/syscall.h': [
        r'\bSYS_HANDLE_FLAGS_GET\s*=\s*129\b',
        r'#define SYS_HANDLE_CLOSE_ON_EXEC 1u\b',
        r'\bSYS_GETPPID\s*=\s*132\b',
        r'\bSYS_UMASK\s*=\s*133\b',
        r'\bSYS_READLINK\s*=\s*135\b',
        r'\bSYS_LSTAT\s*=\s*136\b',
        r'\bSYS_FILE_LOCK\s*=\s*137\b'],
    'usr/lib/libt/include/thyla/syscall.h': [
        r'#define T_SYS_HANDLE_FLAGS_GET 129\b',
        r'#define T_HANDLE_CLOSE_ON_EXEC 1u\b',
        r'#define T_SYS_GETPPID 132\b',
        r'#define T_SYS_UMASK 133\b',
        r'#define T_SYS_FILE_LOCK 137\b'],
    'usr/lib/libthyla-rs/src/lib.rs': [
        r'pub const T_SYS_HANDLE_FLAGS_GET: u64 = 129;',
        r'pub const T_HANDLE_CLOSE_ON_EXEC: u64 = 1;',
        r'pub const T_SYS_GETPPID: u64 = 132;',
        r'pub const T_SYS_UMASK: u64 = 133;',
        r'pub const T_SYS_FILE_LOCK: u64 = 137;'],
    'usr/lib/pouch/patches/0053-pouch-spawn-source-query.patch': [
        r'\+#define __NR_thyla_handle_flags_get 129\b',
        r'\+#define SYS_thyla_handle_flags_get 129\b'],
    'usr/lib/pouch/patches/0057-pouch-kill-probe-getppid.patch': [
        r'\+#define SYS_thyla_getppid\s+132\b'],
    'usr/lib/pouch/patches/0058-pouch-umask.patch': [
        r'\+#define SYS_thyla_umask\s+133\b',
        r'\+\treturn __syscall\(SYS_thyla_umask, mode & 0777\);'],
    'usr/lib/pouch/patches/0059-pouch-symlinks.patch': [
        r'\+#define SYS_thyla_readlink\s+135\b',
        r'\+#define SYS_thyla_lstat\s+136\b'],
    'usr/lib/pouch/patches/0065-pouch-file-locks.patch': [
        r'\+#define SYS_thyla_file_lock\s+137\b'],
    'kernel/include/thylacine/vivarium.h': [r'#define VIV_NATIVE_CEILING\s+141\b'],
    'kernel/vivarium.c': [
        r'VIV_LINUX_KILL == SYS_HANDLE_FLAGS_GET',
        r'\{ VIV_LINUX_KILL,\s+VIV_TIER2 \}',
        r'VIV_LINUX_SIGALTSTACK == SYS_GETPPID',
        r'VIV_LINUX_RT_SIGSUSPEND == SYS_UMASK',
        r'\{ VIV_LINUX_UMASK,\s+VIV_TIER2 \}',
        r'VIV_LINUX_RT_SIGPROCMASK == SYS_READLINK',
        r'VIV_LINUX_RT_SIGPENDING == SYS_LSTAT',
        r'VIV_LINUX_RT_SIGACTION == SYS_SRV_REGISTRY_NEW',
        r'VIV_LINUX_RT_SIGTIMEDWAIT == SYS_FILE_LOCK',
        r'VIV_LINUX_RT_SIGQUEUEINFO == SYS_POSTNOTE_TID',
        r'VIV_LINUX_RT_SIGRETURN == SYS_NOTE_DISPOSE',
        r'_Static_assert\(SYS_ACCOUNT == 140,',
        r'_Static_assert\(SYS_NOTE_WAIT == 141,'],
}
failed = []
for name, patterns in checks.items():
    source = (r / name).read_text()
    if a.mutant == 'ceiling' and name.endswith('vivarium.h'):
        source, n = re.subn(r'(#define VIV_NATIVE_CEILING\s+)141\b', r'\g<1>140', source)
        assert n == 1
    for pattern in patterns:
        if len(re.findall(pattern, source)) != 1:
            failed.append((name, pattern))
if a.mutant == 'ceiling':
    assert failed == [('kernel/include/thylacine/vivarium.h', r'#define VIV_NATIVE_CEILING\s+141\b')], failed
    print('PASS ceiling mutant refused by the pin')
    raise SystemExit(0)
assert not failed, failed

# Every syscall number a mirror names, against the kernel enum, by name: a pin per
# number is re-pointed by hand at each renumber, and a merge that has two branches
# take one number moves a mirror alone. A floor and the renumbered region prove
# each search read its mirror.
kernel_numbers = {}
for m in re.finditer(r'^\s*SYS_([A-Z0-9_]+)\s*=\s*(\d+)\s*,',
                     (r / 'kernel/include/thylacine/syscall.h').read_text(), re.M):
    assert m.group(1) not in kernel_numbers, m.group(1)
    kernel_numbers[m.group(1)] = int(m.group(2))
POUCH_ALIASES = {'PREAD64': 'PREAD', 'PWRITE64': 'PWRITE', 'EXIT': 'EXITS',
                 'MMAP': 'BURROW_ATTACH_LAZY', 'MUNMAP': 'BURROW_DETACH'}
pouch_patches = sorted((r / 'usr/lib/pouch/patches').glob('*.patch'))
libt = (r / 'usr/lib/libt/include/thyla/syscall.h').read_text()
rust = (r / 'usr/lib/libthyla-rs/src/lib.rs').read_text()
if a.mutant == 'mirror':
    rust, n = re.subn(r'^(pub const T_SYS_NOTE_WAIT:\s*u64\s*=\s*)141;', r'\g<1>142;', rust, flags=re.M)
    assert n == 1
mirrors = {
    'libt': [(m.group(1), int(m.group(2)))
             for pattern in (r'^\s*T_SYS_([A-Z0-9_]+)\s*=\s*(\d+)\s*,', r'^#define T_SYS_([A-Z0-9_]+)\s+(\d+)\s*$')
             for m in re.finditer(pattern, libt, re.M)],
    'libthyla-rs': [(m.group(1), int(m.group(2))) for m in re.finditer(
        r'^pub const T_SYS_([A-Z0-9_]+):\s*u64\s*=\s*(\d+)\s*;', rust, re.M)],
    'pouch': [(POUCH_ALIASES.get(m.group(1).upper(), m.group(1).upper()), int(m.group(2)))
              for patch in pouch_patches for m in re.finditer(
        r'^\+#define\s+(?:__NR_|SYS_)(?:thyla_)?([a-z0-9_]+)\s+(\d+)\s*$', patch.read_text(), re.M)],
}
floor = {'libt': 100, 'libthyla-rs': 120, 'pouch': 50}
region = {'POSTNOTE_TID', 'NOTE_DISPOSE', 'NOTE_WAIT'}
drift = []
for mirror, pairs in mirrors.items():
    names = {name for name, _ in pairs}
    assert len(pairs) >= floor[mirror] and region <= names, (mirror, len(pairs), region - names)
    drift += [(mirror, name, number, kernel_numbers.get(name))
              for name, number in pairs if kernel_numbers.get(name) != number]
if a.mutant == 'mirror':
    assert drift == [('libthyla-rs', 'NOTE_WAIT', 142, 141)], drift
    print('PASS mirror mutant refused by the mirror check')
    raise SystemExit(0)
assert not drift, drift
print('mirror check: ' + ', '.join(f'{k} {len(v)}' for k, v in mirrors.items()) + ' numbers match the kernel')

source = r / 'kernel/syscall.c'
s = source.read_text()
assert 'sys_handle_flags_get_handler(ctx->regs[0])' in s
parts = []
for name in ['sys_handle_flags_get_in', 'sys_handle_flags_get_handler']:
    start = s.index('static s64 ' + name + '(')
    end = s.index('\n}', start) + 2
    parts.append(s[start:end])
handler = '\n'.join(parts)
if a.mutant == 'append':
    old = '    if (st.omode & SYS_WALK_OPEN_OAPPEND)  word |= SYS_HANDLE_STATUS_APPEND;\n'
    assert handler.count(old) == 1
    handler = handler.replace(old, '')
a.output.mkdir(parents=True, exist_ok=False)
(a.output / 'handler.c').write_text(handler+'\n')
inputs = [r / n for n in checks] + pouch_patches + [source, Path(__file__), r / 'tools/host-tests/native-handle-query.c']
(a.output / 'inputs.json').write_text(json.dumps({str(x): hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs}, indent=2)+'\n')
import platform
sdk = ['-isysroot', subprocess.check_output(['xcrun', '--show-sdk-path'], text=True).strip()] if platform.system() == 'Darwin' else []
for opt in ['0', '2']:
    exe = a.output.resolve() / ('test-O'+opt)
    cmd = [a.cc, *sdk, '-std=c11', '-g', '-O'+opt, '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-Wall', '-Wextra', '-Werror', '-I'+str(a.output.resolve()), str(inputs[-1]), '-o', str(exe)]
    (a.output / ('command-O'+opt+'.json')).write_text(json.dumps(cmd)+'\n')
    with (a.output / ('compile-O'+opt+'.log')).open('w') as log:
        subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, check=True)
    run = subprocess.run([str(exe)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    (a.output / ('run-O'+opt+'.log')).write_text(run.stdout)
    print(run.stdout, end='')
    if a.mutant:
        assert run.returncode != 0 and re.search(r'Assertion.*failed', run.stdout), 'the append mutant was not refused'
    else:
        run.check_returncode()
print('PASS append mutant refused by the handler test' if a.mutant else
      'PASS query ABI mirrors and collision declarations (runtime collision gate separate)')
