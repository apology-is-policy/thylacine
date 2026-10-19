#!/usr/bin/env python3
"""Check query ABI mirrors and exercise the actual native handler with mocks.
Usage: test-native-handle-query.py [--mutant ceiling|append] OUTPUT_DIRECTORY
The output must be new. This does not emulate handle locking or Vivarium;
run the native guest witness and Linux collision witness for those boundaries.
A mutant must be refused: ceiling moves VIV_NATIVE_CEILING in the text the pins
read, and append drops the handler's APPEND bit from the code the test runs.
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
p.add_argument('--mutant', choices=['ceiling', 'append'])
a = p.parse_args()
r = Path(__file__).resolve().parent.parent
checks = {
    'kernel/include/thylacine/handle.h': [r'#define PROC_HANDLE_MAX 1024\b'],
    'kernel/include/thylacine/syscall.h': [
        r'\bSYS_HANDLE_FLAGS_GET\s*=\s*129\b',
        r'#define SYS_HANDLE_CLOSE_ON_EXEC 1u\b',
        r'\bSYS_GETPPID\s*=\s*132\b',
        r'\bSYS_UMASK\s*=\s*133\b'],
    'usr/lib/libt/include/thyla/syscall.h': [
        r'#define T_SYS_HANDLE_FLAGS_GET 129\b',
        r'#define T_HANDLE_CLOSE_ON_EXEC 1u\b',
        r'#define T_SYS_GETPPID 132\b',
        r'#define T_SYS_UMASK 133\b'],
    'usr/lib/libthyla-rs/src/lib.rs': [
        r'pub const T_SYS_HANDLE_FLAGS_GET: u64 = 129;',
        r'pub const T_HANDLE_CLOSE_ON_EXEC: u64 = 1;',
        r'pub const T_SYS_GETPPID: u64 = 132;',
        r'pub const T_SYS_UMASK: u64 = 133;'],
    'usr/lib/pouch/patches/0053-pouch-spawn-source-query.patch': [
        r'\+#define __NR_thyla_handle_flags_get 129\b',
        r'\+#define SYS_thyla_handle_flags_get 129\b'],
    'usr/lib/pouch/patches/0057-pouch-kill-probe-getppid.patch': [
        r'\+#define SYS_thyla_getppid\s+132\b'],
    'usr/lib/pouch/patches/0058-pouch-umask.patch': [
        r'\+#define SYS_thyla_umask\s+133\b',
        r'\+\treturn __syscall\(SYS_thyla_umask, mode & 0777\);'],
    'kernel/include/thylacine/vivarium.h': [r'#define VIV_NATIVE_CEILING\s+133\b'],
    'kernel/vivarium.c': [
        r'VIV_LINUX_KILL == SYS_HANDLE_FLAGS_GET',
        r'\{ VIV_LINUX_KILL,\s+VIV_TIER2 \}',
        r'VIV_LINUX_SIGALTSTACK == SYS_GETPPID',
        r'VIV_LINUX_RT_SIGSUSPEND == SYS_UMASK',
        r'\{ VIV_LINUX_UMASK,\s+VIV_TIER2 \}',
        r'VIV_LINUX_RT_SIGACTION > VIV_NATIVE_CEILING'],
}
failed = []
for name, patterns in checks.items():
    source = (r / name).read_text()
    if a.mutant == 'ceiling' and name.endswith('vivarium.h'):
        source, n = re.subn(r'(#define VIV_NATIVE_CEILING\s+)133\b', r'\g<1>132', source)
        assert n == 1
    for pattern in patterns:
        if len(re.findall(pattern, source)) != 1:
            failed.append((name, pattern))
if a.mutant == 'ceiling':
    assert failed == [('kernel/include/thylacine/vivarium.h', r'#define VIV_NATIVE_CEILING\s+133\b')], failed
    print('PASS ceiling mutant refused by the pin')
    raise SystemExit(0)
assert not failed, failed
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
inputs = [r / n for n in checks] + [source, Path(__file__), r / 'tools/host-tests/native-handle-query.c']
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
