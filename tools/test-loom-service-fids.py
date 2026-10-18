#!/usr/bin/env python3
"""Actual FID module REAP of a walked FID, on the shared guest/host fixture. Hold a Yip host lease.

Sequential state/identity checks only: no proof of caller locking, protocol
quiescence or wire effects. Retain every mutant and require its named
assertion, never an arbitrary crash or sanitizer diagnostic.
"""
import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent.parent
# Apple clang's ASan runtime hangs at startup on macOS 26 (before main); the
# Homebrew LLVM toolchain test-loom-client.sh uses does not.
LLVM_CC = Path(os.environ.get('LLVM_PREFIX', '/opt/homebrew/opt/llvm')) / 'bin/clang'
MUTANTS = [
    ('reap-aborting', 'p->state!=LOOM_SERVICE_RETIRED ||',
     '(p->state!=LOOM_SERVICE_RETIRED && p->state!=LOOM_SERVICE_ABORTING) ||',
     'an aborting scope has not proven quiescence, and the refusal changes nothing'),
    ('reap-pinned', ' || t->slots[source.slot].pins)return -T_E_BUSY;\n        int rc=loom_fid_retire_aborted',
     ')return -T_E_BUSY;\n        int rc=loom_fid_retire_aborted',
     'a pinned FID refuses REAP before any transition'),
    ('reap-clunked-only', '        int rc=loom_fid_retire_aborted(t,nodes,source);\n        if(rc)return rc;\n',
     '        return -T_E_BUSY;\n',
     'the FIDs of a retired scope reap and release it'),
    ('reap-keeps-parent-pin', '    (void)loom_slot_reap(t,source);\n    (void)loom_slot_unpin(t,parent);',
     '    (void)loom_slot_reap(t,source);',
     'a clunked FID reaps once and unpins its scope'),
    ('walk-opened', '    if(walk && (n->fid_flags&LFID_OPEN))return -T_E_BADF;\n', '    (void)walk;\n',
     'an opened FID is no WALK source, and the refusal changes nothing'),
    ('walk-never', '    return fid_borrow(t,nodes,source,scope,0,true,wire);',
     '    (void)source;(void)scope;(void)wire;(void)t;(void)nodes;return -T_E_BADF;',
     'an unopened FID is a WALK source'),
]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--logs', type=Path, required=True)
    args = parser.parse_args()
    args.logs.mkdir(parents=True, exist_ok=False)
    inputs = [ROOT / p for p in ('kernel/loom_service_fids.c',
              'kernel/include/thylacine/loom_service_fids.h',
              'kernel/test/loom_fids_fixture.h',
              'tools/host-tests/loom-service-fids.c',
              'tools/test-loom-service-fids.py',
              'kernel/loom_service_nodes.c', 'kernel/include/thylacine/loom_service_nodes.h',
              'kernel/loom_service_slots.c', 'kernel/include/thylacine/loom_service_slots.h',
              'kernel/include/thylacine/loom_service_abi.h')]
    original = inputs[0].read_text()
    hashes = {str(p.relative_to(ROOT)): sha(p) for p in inputs}
    results = []
    for name, old, new, expected in [('control', None, None, None)] + MUTANTS:
        source = original
        if old is not None:
            assert original.count(old) == 1, (name, original.count(old))
            source = original.replace(old, new)
        path = args.logs / (name + '.c')
        path.write_text(source)
        (args.logs / (name + '.diff')).write_text(''.join(difflib.unified_diff(
            original.splitlines(True), source.splitlines(True),
            fromfile='kernel/loom_service_fids.c', tofile=name + '.c')))
        exe = path.with_suffix('')
        cc = os.environ.get('CC') or (str(LLVM_CC) if LLVM_CC.exists() else 'clang')
        cmd = [cc, '-std=c11', '-O1', '-Wall',
               '-Wextra', '-Werror', '-I', str(ROOT / 'kernel/include'),
               str(path), str(inputs[3]), str(ROOT / 'kernel/loom_service_nodes.c'),
               str(ROOT / 'kernel/loom_service_slots.c'), '-o', str(exe),
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
        cmd += shlex.split(os.environ.get('CFLAGS', ''))
        sdk = subprocess.run(['xcrun', '--show-sdk-path'], capture_output=True, text=True) \
            if shutil.which('xcrun') else None
        if sdk and sdk.returncode == 0 and '-isysroot' not in cmd:
            cmd += ['-isysroot', sdk.stdout.strip()]
        (args.logs / (name + '-command.json')).write_text(json.dumps(cmd) + '\n')
        with (args.logs / (name + '-build.log')).open('wb') as out:
            subprocess.run(cmd, stdout=out, stderr=subprocess.STDOUT, check=True)
        run = subprocess.run([str(exe.resolve())], capture_output=True,
                             text=True, timeout=20)
        (args.logs / (name + '.log')).write_text(run.stdout + run.stderr)
        if expected:
            assert run.returncode == 1 and run.stderr == f'FAIL {expected}\n', (
                name, run.returncode, run.stdout, run.stderr)
        else:
            assert run.returncode == 0 and not run.stderr and run.stdout.startswith(
                'PASS walked FID reap:'), (run.returncode, run.stdout, run.stderr)
        results.append(dict(case=name, expected=expected, rc=run.returncode,
                            generated_sha256=sha(path), binary_sha256=sha(exe)))
        print('PASS', name, expected or run.stdout.strip(), flush=True)
    assert hashes == {str(p.relative_to(ROOT)): sha(p) for p in inputs}
    (args.logs / 'result.json').write_text(json.dumps(dict(
        inputs=hashes, sanitized=True, results=results), indent=2) + '\n')


if __name__ == '__main__':
    main()
