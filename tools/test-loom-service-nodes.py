#!/usr/bin/env python3
"""Actual scope/fid reservation module and shared guest/host fixture. Hold a Yip host lease.

Sequential state/identity checks only: no proof of caller locking, allocation,
authority, protocol terminality or payload accounting. Retain every mutant and
require its named assertion, never an arbitrary crash or sanitizer diagnostic.
"""
import argparse
import difflib
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('host_sanitizer_cc', ROOT / 'tools/host-sanitizer-cc.py')
sanitizer_cc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sanitizer_cc)
MUTANTS = [
    ('parent-zero', 'kind == LOOM_SERVICE_SCOPE && !node_zero_ref(parent)', 'kind == LOOM_SERVICE_SCOPE && !node_zero_ref(parent) && false', 'scope parent must be zero'),
    ('parent-ready', 'p->state == LOOM_SERVICE_READY;', 'p->state != LOOM_SERVICE_EMPTY;', 'unused scope cannot authorize child'),
    ('omit-parent-pin', 'int rc = loom_slot_pin(t, parent, LOOM_SERVICE_SCOPE);', 'int rc = 0;', 'provisional child retains parent'),
    ('early-visible', '    *out = ref;', '    (void)loom_slot_publish(t, ref);\n    *out = ref;', 'provisional scope cannot be borrowed'),
    ('quota-parent-leak', 'if (kind == LOOM_SERVICE_FID) (void)loom_slot_unpin(t, parent);', 'if (kind == LOOM_SERVICE_FID && false) (void)loom_slot_unpin(t, parent);', 'quota refusal restores parent and output'),
    ('omit-parent-drop', 'if (child) (void)loom_slot_unpin(t, parent);', 'if (child && false) (void)loom_slot_unpin(t, parent);', 'child rollback releases parent and both ledgers'),
    ('stale-remove', ' || s->incarnation != ref.incarnation', '', 'stale child cannot release replacement parent'),
    ('allow-borrow-reap', ' || t->slots[ref.slot].pins', '', 'borrowed unused child cannot be reaped'),
    ('allow-ready-reap', 'n->state != LOOM_SERVICE_RESERVED || ', '', 'ready scope cannot use unused reap'),
    ('abort-publish', '!node_parent_ready(t, nodes, n->parent)', '!node_parent_ready(t, nodes, n->parent) && false', 'parent abort wins child publication without losing pins'),
    ('omit-slot-reap', '        (void)loom_slot_reap(t, ref);', '', 'closed unused child cleanup releases parent'),
]



def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--logs', type=Path, required=True)
    args = parser.parse_args()
    args.logs.mkdir(parents=True, exist_ok=False)
    inputs = [ROOT / p for p in ('kernel/loom_service_nodes.c',
              'kernel/include/thylacine/loom_service_nodes.h',
              'kernel/include/thylacine/loom_service_abi.h',
              'kernel/test/loom_nodes_fixture.h',
              'tools/host-tests/loom-service-nodes.c',
              'tools/test-loom-service-nodes.py', 'kernel/loom_service_slots.c',
              'kernel/include/thylacine/loom_service_slots.h')]
    original = inputs[0].read_text()
    hashes = {str(p.relative_to(ROOT)): sha(p) for p in inputs}
    cc, cc_extra = sanitizer_cc.select(args.logs)
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
            fromfile='kernel/loom_service_nodes.c', tofile=name + '.c')))
        exe = path.with_suffix('')
        cmd = [cc, '-std=c11', '-O1', '-Wall',
               '-Wextra', '-Werror', '-I', str(ROOT / 'kernel/include'),
               str(path), str(inputs[4]), str(ROOT / 'kernel/loom_service_slots.c'), '-o', str(exe),
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
        cmd += cc_extra + shlex.split(os.environ.get('CFLAGS', ''))
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
                'PASS scope/fid reservations:'), (run.returncode, run.stdout, run.stderr)
        results.append(dict(case=name, expected=expected, rc=run.returncode,
                            generated_sha256=sha(path), binary_sha256=sha(exe)))
        print('PASS', name, expected or run.stdout.strip(), flush=True)
    assert hashes == {str(p.relative_to(ROOT)): sha(p) for p in inputs}
    (args.logs / 'result.json').write_text(json.dumps(dict(
        inputs=hashes, sanitized=True, results=results), indent=2) + '\n')


if __name__ == '__main__':
    main()
