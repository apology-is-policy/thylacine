#!/usr/bin/env python3
"""Bounded hidden-storage lifecycle and named counterexamples (TAPESTRY-STORAGE)."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent
CASES = [('', '')] + [('_buggy_' + key, invariant) for key, invariant in [
    ('offer', 'OfferFresh'), ('fid', 'FidFresh'), ('dead', 'NoResurrection'),
    ('mapping', 'ClientBacked'), ('device', 'DeviceBacked'),
    ('scanout', 'SuspendedUnbound'), ('partial', 'FirstFrameComplete')]]

def main():
    java = os.environ.get('JAVA', '/opt/homebrew/opt/openjdk/bin/java' if Path('/opt/homebrew/opt/openjdk/bin/java').exists() else 'java')
    jar = os.environ.get('TLA_JAR', '/tmp/tla2tools.jar')
    if not Path(jar).is_file():
        raise SystemExit('Missing TLA_JAR; use the release specified in docs/agent/SPEC-POLICY.md')
    for suffix, invariant in CASES:
        name = 'tapestry_storage' + suffix
        with tempfile.TemporaryDirectory(prefix='tapestry-storage-') as tmp:
            run = subprocess.run([java, '-Xmx768m', '-XX:+UseParallelGC', '-cp', jar,
                'tlc2.TLC', '-workers', '2', '-deadlock', '-noGenerateSpecTE',
                '-metadir', tmp, '-config', name + '.cfg', 'tapestry_storage.tla'],
                cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=300)
        if invariant:
            ok = run.returncode == 12 and 'Invariant ' + invariant + ' is violated' in run.stdout
        else:
            ok = run.returncode == 0 and '287 distinct states found' in run.stdout
        if not ok:
            print(run.stdout)
            raise SystemExit(f'FAIL {name}: exit {run.returncode}; expected {invariant or "clean/287"}')
        print(f'PASS {name}: {invariant or "clean, 287 states"}', flush=True)
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
