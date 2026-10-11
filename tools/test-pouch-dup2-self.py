#!/usr/bin/env python3
"""Exercise the series-applied Pouch dup2 with host metadata-boundary mocks."""
import argparse
import hashlib
import json
import platform
import subprocess
from pathlib import Path
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('musl', type=Path)
p.add_argument('output', type=Path)
p.add_argument('--cc', default='/opt/homebrew/opt/llvm/bin/clang')
p.add_argument('--expect-failure', action='store_true')
p.add_argument('--mutant', choices=['query', 'socket', 'legacy'])
a = p.parse_args()
r = Path(__file__).resolve().parent.parent
source = a.musl/'src/unistd/dup2.c'
s = source.read_text()
if a.mutant:
    old, new = {
        'query': ('long flags = __syscall1(SYS_thyla_handle_flags_get, old);', 'long flags = 0;'),
        'socket': ('if (!pouch_sock_resolve(old))', 'if (0 && !pouch_sock_resolve(old))'),
        'legacy': ('flags == -1 ? ENOSYS : (int)-flags', '(int)-flags'),
    }[a.mutant]
    assert s.count(old) == 1
    s = s.replace(old, new)
for header in ('"syscall.h"', '"_pouch_process.h"', '"_pouch_socket.h"', '"_pouch_descriptor.h"'):
    s = s.replace('#include '+header, '/* supplied by host boundary harness */')
a.output.mkdir(parents=True, exist_ok=False)
(a.output/'dup2-under-test.c').write_text(s)
common = a.musl/'src/internal/_pouch_descriptor.h'
if common.exists():
    body=common.read_text()
    for h in ('syscall.h','_pouch_socket.h'):
        body=body.replace('#include "'+h+'"','/* host boundary */')
    (a.output/'descriptor-under-test.h').write_text(body)
else:
    (a.output/'descriptor-under-test.h').write_text('/* older source has no common helper */\n')

inputs = [source, r/'tools/host-tests/pouch-dup2-self.c', Path(__file__)]
if common.exists():
    inputs.append(common)
(a.output/'inputs.json').write_text(json.dumps({str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs},indent=2)+'\n')
sdk = ['-isysroot', subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()] if platform.system() == 'Darwin' else []
for opt in ('0','2'):
    exe = a.output.resolve()/('test-O'+opt)
    cmd = [a.cc,*sdk,'-std=c11','-O'+opt,'-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-Wall','-Wextra','-Werror','-I'+str(a.output.resolve()),str(inputs[1]),'-o',str(exe)]
    (a.output/f'compile-O{opt}.json').write_text(json.dumps(cmd)+'\n')
    with (a.output/f'compile-O{opt}.log').open('w') as log:
        subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
    out = subprocess.run([str(exe)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
    (a.output/f'run-O{opt}.log').write_text(out.stdout)
    print(out.stdout,end='')
    assert 'AddressSanitizer' not in out.stdout and 'runtime error:' not in out.stdout
    if a.expect_failure or a.mutant:
        assert out.returncode == 1 and 'FAIL ' in out.stdout
    else:
        out.check_returncode()
