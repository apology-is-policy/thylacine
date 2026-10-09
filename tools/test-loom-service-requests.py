#!/usr/bin/env python3
"""Actual request terminal-obligation module and shared guest/host fixture. Hold a Yip host lease.

Sequential state/identity checks only: no proof of caller locking, allocation,
authority, protocol terminality or payload accounting. Retain every mutant and
require its named assertion, never an arbitrary crash or sanitizer diagnostic.
"""
import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parent.parent
MUTANTS = [
    ('expired', 'if (deadline && deadline <= now)', 'if (((void)now, false))', 'expired request reserves nothing'),
    ('admit-expired', 'if (r->deadline && r->deadline <= now)', 'if (((void)now, false))', 'admission rechecks absolute deadline'),
    ('omit-pin', 'int rc = loom_slot_pin(s,scope,LOOM_SERVICE_SCOPE);', 'int rc = 0;', 'prepare reserves terminal and parent'),
    ('omit-unpin', '    (void)loom_slot_unpin(s,scope);', '    (void)scope;', 'rollback releases capacity and pin'),
    ('reuse-identity', '.identity=++t->next_identity,', '.identity=t->next_identity=1,', 'rollback burns request identity'),
    ('stale', ' && r->identity == ref.identity', '', 'stale request cannot mutate replacement'),
    ('early-terminal', 'if (r->phase != LREQ_ACTIVE || r->borrows)', 'if (r->phase != LREQ_ACTIVE)', 'live borrow blocks terminal and free'),
    ('double-terminal', 'if (r->phase != LREQ_ACTIVE || r->borrows)', 'if ((r->phase != LREQ_ACTIVE && r->phase != LREQ_TERMINAL) || r->borrows)', 'duplicate terminal refused'),
    ('late-success', 'if (result >= 0 && (t->detached || !request_scope_open(s,n,r->scope)))', 'if (((void)n, false))', 'abort forbids fresh success'),
    ('early-reap', 'return request_remove(t,s,ref,LREQ_DELIVERED);', 'return request_remove(t,s,ref,LREQ_TERMINAL);', 'undelivered terminal cannot be discarded or freed'),
    ('live-discard', 'if (!t->detached || r->phase != LREQ_TERMINAL || r->borrows)', 'if (r->phase != LREQ_TERMINAL || r->borrows)', 'undelivered terminal cannot be discarded or freed'),
    ('wrap', ' || t->next_identity == UINT64_MAX', '', 'request identity wrap refused'),
    ('detach-borrow', 'if (t->records[i].borrows)', 'if (t->records[i].borrows && false)', 'live borrows prevent consumer detach'),
]




def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--logs', type=Path, required=True)
    args = parser.parse_args()
    args.logs.mkdir(parents=True, exist_ok=False)
    inputs = [ROOT / p for p in ('kernel/loom_service_requests.c',
              'kernel/include/thylacine/loom_service_requests.h',
              'kernel/include/thylacine/loom_service_abi.h',
              'kernel/test/loom_requests_fixture.h',
              'tools/host-tests/loom-service-requests.c',
              'tools/test-loom-service-requests.py', 'kernel/loom_service_slots.c',
              'kernel/include/thylacine/loom_service_slots.h', 'kernel/loom_service_nodes.c', 'kernel/include/thylacine/loom_service_nodes.h')]
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
            fromfile='kernel/loom_service_requests.c', tofile=name + '.c')))
        exe = path.with_suffix('')
        cmd = [os.environ.get('CC', 'clang'), '-std=c11', '-O1', '-Wall',
               '-Wextra', '-Werror', '-I', str(ROOT / 'kernel/include'),
               str(path), str(inputs[4]), str(ROOT / 'kernel/loom_service_slots.c'), str(ROOT / 'kernel/loom_service_nodes.c'), '-o', str(exe),
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
        cmd += shlex.split(os.environ.get('CFLAGS', ''))
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
                'PASS request terminal obligations'), (run.returncode, run.stdout, run.stderr)
        results.append(dict(case=name, expected=expected, rc=run.returncode,
                            generated_sha256=sha(path), binary_sha256=sha(exe)))
        print('PASS', name, expected or run.stdout.strip(), flush=True)
    assert hashes == {str(p.relative_to(ROOT)): sha(p) for p in inputs}
    (args.logs / 'result.json').write_text(json.dumps(dict(
        inputs=hashes, sanitized=True, results=results), indent=2) + '\n')


if __name__ == '__main__':
    main()
