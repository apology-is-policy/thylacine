#!/usr/bin/env python3
"""Actual joint pool/slot module and shared guest/host fixture. Hold a Yip host lease.

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
    ('prepare-leak', '        (void)loom_slot_rollback(t, ref);', '',
     'failed member prepare leaves no hidden slot or output'),
    ('early-visible', '    *out = ref;', '    (void)loom_slot_publish(t, ref);\n    *out = ref;',
     'provisional hidden but excludes extent'),
    ('rollback-members', '    (void)loom_pool_rollback(&pools->bank, p);', '',
     'rollback releases both exclusions'),
    ('borrow-reap', 'pool_record(t, pools, ref, LSLOT_LIVE);\n    if (!p) return -T_E_NOENT;\n    if (t->slots[ref.slot].pins) return -T_E_BUSY;',
     'pool_record(t, pools, ref, LSLOT_LIVE);\n    if (!p) return -T_E_NOENT;',
     'borrow refusal preserves both ledgers'),
    ('busy-reap', '    if (rc) return rc;\n    // No streams', '    (void)rc;\n    // No streams',
     'stream refusal preserves slot'),
    ('reap-slot', '    return loom_slot_reap(t, ref);', '    return 0;',
     'exact return permits both ledgers to reap'),
    ('late-publish', '    if (rc) return rc;\n    return loom_pool_publish(p);',
     '    (void)rc;\n    return loom_pool_publish(p);',
     'close rejects joint publication without half commit'),
]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--logs', type=Path, required=True)
    args = parser.parse_args()
    args.logs.mkdir(parents=True, exist_ok=False)
    inputs = [ROOT / p for p in ('kernel/loom_service_pool_table.c',
              'kernel/include/thylacine/loom_service_pool_table.h',
              'kernel/include/thylacine/loom_service_abi.h',
              'kernel/test/loom_pool_table_fixture.h',
              'tools/host-tests/loom-service-pool-table.c',
              'tools/test-loom-service-pool-table.py',
              'kernel/loom_service_slots.c', 'kernel/loom_service_pool.c',
              'kernel/include/thylacine/loom_service_slots.h',
              'kernel/include/thylacine/loom_service_pool.h')]
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
            fromfile='kernel/loom_service_pool_table.c', tofile=name + '.c')))
        exe = path.with_suffix('')
        cmd = [cc, '-std=c11', '-O1', '-Wall',
               '-Wextra', '-Werror', '-I', str(ROOT / 'kernel/include'),
               str(path), str(inputs[4]), str(inputs[6]), str(inputs[7]), '-o', str(exe),
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
                'PASS joint pool table:'), (run.returncode, run.stdout, run.stderr)
        results.append(dict(case=name, expected=expected, rc=run.returncode,
                            generated_sha256=sha(path), binary_sha256=sha(exe)))
        print('PASS', name, expected or run.stdout.strip(), flush=True)
    assert hashes == {str(p.relative_to(ROOT)): sha(p) for p in inputs}
    (args.logs / 'result.json').write_text(json.dumps(dict(
        inputs=hashes, sanitized=True, results=results), indent=2) + '\n')


if __name__ == '__main__':
    main()
