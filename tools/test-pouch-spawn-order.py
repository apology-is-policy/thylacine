#!/usr/bin/env python3
"""Build/run actual Pouch spawn source with a simulated native handle boundary.
Pass a series-applied musl tree and an empty output directory. No sysroot edits.
"""
import argparse
import hashlib
import json
import os
import platform
import shutil
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('musl', type=Path)
p.add_argument('output', type=Path)
p.add_argument('--cc', default=os.environ.get('CC') or shutil.which('/opt/homebrew/opt/llvm/bin/clang') or 'clang')
p.add_argument('--mutant', choices=['close', 'query', 'legacy'])
a = p.parse_args()
r = Path(__file__).resolve().parent.parent
sdk_flags = []
if platform.system() == 'Darwin':
    sdk_flags = ['-isysroot', subprocess.check_output(['xcrun', '--show-sdk-path'], text=True).strip()]
a.output.mkdir(parents=True, exist_ok=False)
source = a.musl / 'src/process/posix_spawn.c'
text = source.read_text()
if a.mutant:
    old, new = {
        'close': ('if (prior && prior->cmd == FDOP_CLOSE)', 'if (0 && prior && prior->cmd == FDOP_CLOSE)'),
        'query': ('if (!prior) {', 'if (0 && !prior) {'),
        'legacy': ('return flags == -1 ? -ENOSYS : flags;', 'return flags;'),
    }[a.mutant]
    assert text.count(old) == 1
    text = text.replace(old, new)
# Use host standard C headers; replace only target ABI declarations and syscall
# shim. The function body is byte-for-byte the series-produced implementation.
for header in ['<spawn.h>', '"syscall.h"', '"fdop.h"', '"_pouch_process.h"']:
    text = text.replace('#include ' + header, '/* supplied by host boundary harness */')
(a.output / 'spawn-under-test.c').write_text(text)
fdop = (a.musl / 'src/process/fdop.h').read_text().split('#define malloc')[0]
(a.output / 'fdop-test.h').write_text(fdop)
header = a.musl / 'src/internal/_pouch_process.h'
(a.output / '_pouch_process.h').write_bytes(header.read_bytes())
inputs = [source, header, a.musl / 'src/process/fdop.h', r / 'tools/host-tests/pouch-spawn-order.c', Path(__file__)]
(a.output / 'inputs.json').write_text(json.dumps({str(x): hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs}, indent=2)+'\n')
for opt in ['0', '2']:
    exe = a.output.resolve() / ('test-O' + opt)
    cmd = [a.cc, *sdk_flags, '-std=c11', '-D_DARWIN_C_SOURCE', '-g', '-O'+opt, '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-Wall', '-Wextra', '-Werror', '-I'+str(a.output.resolve()), str(r/'tools/host-tests/pouch-spawn-order.c'), '-o', str(exe)]
    (a.output / ('compile-O'+opt+'.json')).write_text(json.dumps(cmd)+'\n')
    build = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    (a.output / ('compile-O'+opt+'.log')).write_text(build.stdout)
    build.check_returncode()
    result = subprocess.run([str(exe)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    (a.output / ('run-O'+opt+'.log')).write_text(result.stdout)
    print(result.stdout, end='')
    (a.output / ('result-O'+opt+'.json')).write_text(json.dumps({'exit': result.returncode, 'mutant': a.mutant})+'\n')
    if a.mutant:
        assert result.returncode == 1 and 'FAIL ' in result.stdout and 'failures=0' not in result.stdout, 'mutant did not fail the intended assertions'
    elif result.returncode:
        raise SystemExit(result.returncode)
