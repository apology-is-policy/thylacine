#!/usr/bin/env python3
"""Actual slot module and shared guest/host fixture. Hold a Yip host lease.

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
    ('stale', ' || s->incarnation != ref.incarnation', '',
     'stale incarnation cannot borrow reused slot'),
    ('reserved', 'ref.reserved || ', '', 'reserved reference refused'),
    ('provisional', 's->phase != LSLOT_LIVE || s->kind != kind',
     '(s->phase != LSLOT_LIVE && s->phase != LSLOT_PROVISIONAL) || s->kind != kind',
     'provisional target is not usable'),
    ('wrong-kind', ' || s->kind != kind', ' || (kind == 0)', 'wrong kind cannot borrow'),
    ('rollback-generation', '*s = (struct loom_service_slot){0};\n    return 0;\n}\nint loom_slot_pin',
     '--t->next_incarnation;\n    *s = (struct loom_service_slot){0};\n    return 0;\n}\nint loom_slot_pin',
     'rollback identity never repeats'),
    ('early-reap', 's->phase != LSLOT_RETIRING || s->pins',
     's->phase != LSLOT_RETIRING', 'borrow prevents early reuse'),
    ('live-reap', 's->phase != LSLOT_RETIRING || s->pins',
     's->pins', 'live slot cannot be reaped'),
    ('capacity', 'i < LOOM_SERVICE_SLOT_COUNT', 'i < LOOM_SERVICE_SLOT_COUNT - 1',
     'mixed table reservation'),
    ('wrap', 'if (t->next_incarnation == UINT64_MAX) return -T_E_NOSPC;', '',
     'incarnation exhaustion never wraps'),
    ('pin-wrap', 'if (s->pins == UINT32_MAX) return -T_E_NOSPC;', '',
     'borrow saturation never wraps'),
    ('close-reserve', 'if (t->closing) return -T_E_CANCELED;\n    if (t->next_incarnation',
     'if (t->next_incarnation', 'close freezes reservations'),
    ('close-publish', 'if (t->closing) return -T_E_CANCELED;\n    if (s->phase != LSLOT_PROVISIONAL)',
     'if (s->phase != LSLOT_PROVISIONAL)', 'close wins against output publication'),
    ('close-pin', 'if (t->closing) return -T_E_CANCELED;\n    if (s->phase != LSLOT_LIVE',
     'if (s->phase != LSLOT_LIVE', 'close freezes borrows'),
]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--logs', type=Path, required=True)
    args = parser.parse_args()
    args.logs.mkdir(parents=True, exist_ok=False)
    inputs = [ROOT / p for p in ('kernel/loom_service_slots.c',
              'kernel/include/thylacine/loom_service_slots.h',
              'kernel/include/thylacine/loom_service_abi.h',
              'kernel/test/loom_slots_fixture.h',
              'tools/host-tests/loom-service-slots.c',
              'tools/test-loom-service-slots.py')]
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
            fromfile='kernel/loom_service_slots.c', tofile=name + '.c')))
        exe = path.with_suffix('')
        cmd = [os.environ.get('CC', 'clang'), '-std=c11', '-O1', '-Wall',
               '-Wextra', '-Werror', '-I', str(ROOT / 'kernel/include'),
               str(path), str(inputs[4]), '-o', str(exe),
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
                'PASS actual slot ledger:'), (run.returncode, run.stdout, run.stderr)
        results.append(dict(case=name, expected=expected, rc=run.returncode,
                            generated_sha256=sha(path), binary_sha256=sha(exe)))
        print('PASS', name, expected or run.stdout.strip(), flush=True)
    assert hashes == {str(p.relative_to(ROOT)): sha(p) for p in inputs}
    (args.logs / 'result.json').write_text(json.dumps(dict(
        inputs=hashes, sanitized=True, results=results), indent=2) + '\n')


if __name__ == '__main__':
    main()
