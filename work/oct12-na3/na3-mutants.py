#!/usr/bin/env python3
"""NA-3 mutant run over the Rust Loom client cores (usr/lib/loom-client).

Every Rust mutant in Astra's drivers (extracted, never executed, by
extract_rust_mutants.py into astra-rust-mutants.json) is carried onto the crate
by the derivation that installed it (na3_install.py): her original core gets the
listed edits, then her mutant, whose own text passes through the same edits (so
`global-cap`, which deletes the line the envelope edit anchors on, still deletes
it), then rustfmt. No anchor is rewritten by hand, and a mutant the derivation
would neutralize is reported, not counted.

A mutant is rejected only when the crate still BUILDS and a test from the set
Astra's driver named (her test file's tests, under her filter) FAILS, with her
expected panic text where she gave one. A build failure, a timeout, or a failure
only outside the named set does not count.

Each compile-fail case is appended as a test to the module that holds its
fixtures; it must fail with its rustc error code at the appended lines, and only
there. A case with no code is a positive control: it must build and pass.

Also: the pristine crate passes every declared test; each C local-code block's
static assertion refuses a code moved back into the errno range; and
tools/check-loom-service-abi.py, run on a cloned root, passes pristine and
fails on each sabotage of a leg NA-3 added. The tree itself is never edited.

Usage: work/oct12-na3/na3-mutants.py --out DIR   (the mac lease must be held)
       work/oct12-na3/na3-mutants.py --dry       (anchors and derivation only; no build)
"""
import argparse, json, os, re, shutil, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
R = HERE.parents[1]
sys.path.insert(0, str(HERE))
import na3_install as inst  # noqa: E402

CRATE = R / 'usr/lib/loom-client'
LLVM = os.environ.get('LLVM_PREFIX', '/opt/homebrew/opt/llvm')
CC = LLVM + '/bin/clang'
INC = R / 'usr/lib/libt/include'
SUPERSEDED = {'run-payload.py'}  # payload_owner is superseded by ring_payload
# Astra's test file -> the installed module whose scope holds its fixtures.
HOST = {t: 'tests/ring.rs' for t in (
    'test_ring_payload.rs', 'test_fixed_requests.rs', 'test_fixed_futures.rs', 'test_future_retirement.rs',
    'test_local_wait.rs', 'test_local_tasks.rs', 'test_timer_wait.rs', 'test_task_loop.rs')}
HOST.update({'test_deadlines.rs': 'tests/ring/deadlines.rs', 'test_local_progress.rs': 'tests/ring/local_progress.rs',
             'test_retirement_executor.rs': 'tests/ring/executor.rs', 'test_composed_ring.rs': 'tests/ring/composed.rs',
             'test_request_book.rs': 'tests/request_book.rs', 'test_owned_mapping.rs': 'tests/owned_mapping.rs',
             'test_paired_cq.rs': 'tests/paired_cq.rs', 'test_lease_ledger.rs': 'tests/lease_ledger.rs'})
# header, anchor, sabotage, the static assertion's message
C_BLOCKS = [
    ('lease_ledger.h', 'T_LEASE_BUSY=-1103', 'T_LEASE_BUSY=-3', 'lease codes lie in their block'),
    ('request_book.h', 'T_REQ_EXHAUSTED=-1203', 'T_REQ_EXHAUSTED=-3', 'request book codes lie in their block'),
    ('owned_mapping.h', 'T_MAP_CLOSED=-1303', 'T_MAP_CLOSED=-3', 'mapping codes lie in their block'),
    ('paired_cq.h', 'T_CQ_CORRUPT=-1402', 'T_CQ_CORRUPT=-2', 'CQ codes lie in their block'),
]
# label, file (relative to the cloned root), anchor, sabotage, text the checker's log must hold
ABI = [
    ('enospc', 'usr/lib/libt/include/thyla/loom_client/base.h', '#define T_LOOM_CLIENT_ENOSPC (-28)',
     '#define T_LOOM_CLIENT_ENOSPC (-2)', '"T_LOOM_CLIENT_ENOSPC"'),
    ('rust-ringhdr', 'usr/lib/libthyla-rs/src/loom/ring_abi.rs',
     '    pub(crate) cq_head: u32,\n    pub(crate) cq_tail: u32,', '    pub(crate) cq_tail: u32,\n    pub(crate) cq_head: u32,',
     'abi::RingHdr.cq_head'),
    ('rust-constant', 'usr/lib/libthyla-rs/src/loom/ring_abi.rs', 'pub(crate) const SETUP_CQSIZE: u32 = 1 << 1;',
     'pub(crate) const SETUP_CQSIZE: u32 = 1 << 2;', 'SETUP_CQSIZE'),
    ('rust-setup-twin', 'usr/lib/loom-client/src/owned_mapping.rs', '    pub hdr_off: u32,\n    pub sq_array_off: u32,',
     '    pub sq_array_off: u32,\n    pub hdr_off: u32,', 'owned_mapping::Setup.hdr_off'),
    ('c-setup-twin', 'usr/lib/libt/include/thyla/loom_client/owned_mapping.h', 'uint32_t hdr_off,sq_array_off,',
     'uint32_t sq_array_off,hdr_off,', '"t_owned_setup.hdr_off"'),
    ('rust-service', 'usr/lib/loom-client/src/service_abi.rs', 'pub const SERVICE_POOL_MEMBERS: u32 = 64;',
     'pub const SERVICE_POOL_MEMBERS: u32 = 63;', 'ABI vector mismatch: Rust'),
]
ABI_INPUTS = ['arch/arm64', 'kernel/include', 'usr/lib/libt/include', 'usr/lib/libthyla-rs/src/loom/ring_abi.rs',
              'usr/lib/loom-client/src']


def sh(args, log, cwd=None, timeout=600, env=None):
    with open(log, 'wb') as f:
        try:
            return subprocess.run([str(a) for a in args], stdout=f, stderr=subprocess.STDOUT, cwd=cwd,
                                  timeout=timeout, env=env).returncode
        except subprocess.TimeoutExpired:
            return 'timeout'


def load():
    data = json.loads((HERE / 'astra-rust-mutants.json').read_text())
    variants = [v for v in data['variants'] if v['driver'] not in SUPERSEDED]
    cases = [c for c in data['cases'] if c['driver'] not in SUPERSEDED]
    return data, variants, cases


def named_set(v):
    """The tests Astra's driver ran: her test file's tests, under her filter."""
    names = inst.tests_of((inst.ORIG / v['test']).read_text())
    return {n for n in names if not v['filter'] or n.startswith(v['filter'])}


def expect_text(v):
    if not v['expect']:
        return None
    text = eval(v['expect'], {'__builtins__': {}}, {'name': v['name'], 'lang': 'rust'})  # her literal expression
    return None if text == 'FAILED' else text


def mutant_source(v, pristine):
    """The installed core with Astra's mutant applied to the original; error text if not applicable."""
    core = v['file'][:-3]
    n = (inst.ORIG / v['file']).read_text().count(v['a'])
    a, b = inst.carry(core, v['a']), inst.carry(core, v['b'])
    m = inst.core_source(core).count(a)
    if n != m or n < 1 or (n > 1 and not v['replace_all']):
        return None, f"anchor matches {n} times in the original, {m} after the edits"
    text = inst.derive(post=(core, lambda t: t.replace(a, b)), only=core)[f'{core}.rs']
    if text == pristine[f'{core}.rs']:
        return None, 'the derivation neutralizes it'
    return text, None


def scratch_crate(out):
    """A standalone copy of the crate, so a mutant never touches the tree."""
    d = out / 'crate'
    shutil.copytree(CRATE, d)
    toml = (d / 'Cargo.toml').read_text()
    toml = toml.replace('publish.workspace = true', 'publish = false').replace('license.workspace = true',
                                                                                 'license = "0BSD"')
    (d / 'Cargo.toml').write_text(toml + '\n[workspace]\n')
    return d


def cargo(crate, out_log, *args, timeout=600):
    env = dict(os.environ, CARGO_TARGET_DIR=str(crate.parent / 'target'))
    return sh(['cargo', 'test', '--lib', *args], out_log, cwd=crate, timeout=timeout, env=env)


def failures(log):
    """Failing test leaf names, and each one's panic site and message."""
    text = Path(log).read_text(errors='replace')
    failed = re.findall(r'^test (\S+) \.\.\. FAILED$', text, re.M)
    panics = {}
    for m in re.finditer(r"thread '([^']+)' panicked at ([^\n]+):\n([^\n]*)", text):
        panics.setdefault(m.group(1).split('::')[-1], f'{m.group(2)}: {m.group(3)}')
    return {f.split('::')[-1]: panics.get(f.split('::')[-1], '') for f in failed}, text


def summary(text):
    m = re.search(r'^test result: (\w+)\. (\d+) passed; (\d+) failed', text, re.M)
    return (m.group(1), int(m.group(2)), int(m.group(3))) if m else None


def errors(text, host, first_probe_line):
    """rustc errors as (code, file, line); and whether each lies in the probe."""
    out = []
    for m in re.finditer(r'^error(?:\[(E\d+)\])?: [^\n]*\n\s*--> ([^:\n]+):(\d+):\d+', text, re.M):
        out.append((m.group(1), m.group(2), int(m.group(3))))
    inside = [e for e in out if e[1].endswith(host) and e[2] >= first_probe_line]
    return out, inside


def c_blocks(out):
    sdk = subprocess.check_output(['xcrun', '--show-sdk-path'], text=True).strip()
    tu = out / 'codes.c'
    tu.write_text(''.join(f'#include <thyla/loom_client/{h}>\n' for h, *_ in C_BLOCKS))
    if sh([CC, '-isysroot', sdk, '-std=c99', '-Werror', '-I', INC, '-fsyntax-only', tu], out / 'codes-pristine.log'):
        return 'the pristine headers do not compile'
    for header, a, b, msg in C_BLOCKS:
        root = out / ('codes-' + header[:-2])
        subprocess.run(['cp', '-cR', str(INC), str(root)], check=True)
        h = root / 'thyla/loom_client' / header
        s = h.read_text()
        if s.count(a) != 1:
            return f'{header}: anchor count {s.count(a)}'
        h.write_text(s.replace(a, b))
        log = out / ('codes-' + header[:-2] + '.log')
        rc = sh([CC, '-isysroot', sdk, '-std=c99', '-Werror', '-I', root, '-fsyntax-only', tu], log)
        shutil.rmtree(root)
        if rc == 0 or msg not in log.read_text(errors='replace'):
            return f'{header}: {b} compiled, or failed for another reason (rc={rc})'
        print(f'c-codes {header}: {b} refused by "{msg}"', flush=True)
    return None


def abi(out):
    for label, rel, a, b, want in [('pristine', None, None, None, None)] + ABI:
        root = out / ('abi-' + label)
        for inp in ABI_INPUTS:
            (root / inp).parent.mkdir(parents=True, exist_ok=True)
            subprocess.run(['cp', '-cR', str(R / inp), str(root / inp)], check=True)
        if rel:
            f = root / rel
            s = f.read_text()
            if s.count(a) != 1:
                return f'abi {label}: anchor count {s.count(a)}'
            f.write_text(s.replace(a, b))
        log = out / ('abi-' + label + '.log')
        rc = sh([sys.executable, R / 'tools/check-loom-service-abi.py', '--root', root], log, timeout=300)
        text = log.read_text(errors='replace')
        shutil.rmtree(root)
        if not rel:
            if rc != 0 or text.count('PASS:') != 3:
                return f'abi pristine: rc={rc}, {text.count("PASS:")} PASS lines'
        elif rc == 0 or want not in text:
            return f'abi {label}: rc={rc} without {want!r} in its log'
        print(f'abi {label}: rc={rc}', flush=True)
    return None


def dry():
    _, variants, cases = load()
    pristine = inst.derive()
    bad = [f"{v['driver']}:{v['name']}: {err}" for v in variants for _, err in [mutant_source(v, pristine)] if err]
    bad += [f"{c['driver']}:{c['name']}: no host for {c['test']}" for c in cases if c['test'] not in HOST]
    bad += [f"{v['driver']}:{v['name']}: empty named set" for v in variants if not named_set(v)]
    print('\n'.join(bad) or f'{len(variants)} mutants carried, {len(cases)} cases hosted')
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', type=Path)
    ap.add_argument('--dry', action='store_true')
    args = ap.parse_args()
    if args.dry:
        sys.exit(dry())
    out = args.out
    out.mkdir(parents=True)
    res = subprocess.check_output([os.path.expanduser('~/.local/bin/yip'), 'resources'], text=True)
    if not re.search(r'^mac +HELD by (you|corona)\b', res, re.M):
        sys.exit('na3-mutants: the mac lease is not held by corona')
    st = os.statvfs(R)
    if st.f_bavail * st.f_frsize < 8 * 1024 ** 3:
        sys.exit('na3-mutants: below the 8 GiB disk floor')
    data, variants, cases = load()
    pristine = inst.derive()
    for rel, text in pristine.items():
        if (CRATE / 'src' / rel).read_text() != text:
            sys.exit(f'na3-mutants: {rel} differs from its derivation (run na3_install.py --check)')
    versions = {t: subprocess.check_output([t, '--version'], text=True).strip() for t in ('rustc', 'cargo', 'rustfmt')}
    records, failed = [], []
    crate = scratch_crate(out)

    # Control: the pristine crate passes every declared test.
    declared = sum(len(re.findall(r'^\s*#\[test\]', p.read_text(), re.M)) for p in (crate / 'src').rglob('*.rs'))
    rc = cargo(crate, out / 'pristine.log')
    s = summary((out / 'pristine.log').read_text(errors='replace'))
    if rc != 0 or s != ('ok', declared, 0):
        sys.exit(f'na3-mutants: control: rc={rc}, summary {s}, declared {declared}')
    print(f'control: {declared} of {declared} declared tests pass', flush=True)

    for v in variants:
        key = f"{v['driver'][4:-3]}:{v['name']}"
        rec = dict(name=key, file=v['file'], test=v['test'], filter=v['filter'])
        records.append(rec)
        text, err = mutant_source(v, pristine)
        if err:
            failed.append(f'{key}: {err}'); rec['verdict'] = err; continue
        core = crate / 'src' / v['file']
        core.write_text(text)
        log = out / ('mutant-' + re.sub(r'[^a-z0-9.-]+', '_', key) + '.log')
        rc = cargo(crate, log, timeout=300)
        core.write_text(pristine[v['file']])
        fails, body = failures(log)
        hit = {n: p for n, p in fails.items() if n in named_set(v)}
        want = expect_text(v)
        rec.update(rc=rc, failing=sorted(fails), named_failing=sorted(hit), expect=want)
        if rc == 'timeout':
            verdict = 'timeout'
        elif summary(body) is None:
            verdict = 'did not build' if 'error: could not compile' in body else 'no test summary'
        elif rc == 0 or not fails:
            verdict = 'SURVIVED'
        elif not hit:
            verdict = 'killed only outside the named set'
        elif want and not any(want in p for p in hit.values()):
            verdict = f'no named failure shows {want!r}'
        else:
            verdict = 'rejected'
            rec['shown'] = next((p for p in hit.values() if want and want in p), next(iter(hit.values())))
        rec['verdict'] = verdict
        if verdict != 'rejected':
            failed.append(f'{key}: {verdict}')
        print(f"{key:52} {verdict:9} {len(fails):3} failing, {len(hit)} named  {rec.get('shown', '')[:80]}", flush=True)

    for c in cases:
        key = f"case:{c['driver'][4:-3]}:{c['name']}"
        rec = dict(name=key, test=c['test'], diag=c['diag'])
        records.append(rec)
        host = HOST[c['test']]
        f = crate / 'src' / host
        orig = f.read_text()
        first = orig.count('\n') + 2
        f.write_text(orig + '\n#[test]\nfn na3_probe() {\n' + c['body'] + '\n}\n')
        log = out / ('case-' + re.sub(r'[^a-z0-9.-]+', '_', key) + '.log')
        rc = cargo(crate, log, 'na3_probe', timeout=300)
        f.write_text(orig)
        text = log.read_text(errors='replace')
        errs, inside = errors(text, host, first)
        if c['diag']:
            ok = rc != 0 and errs and len(inside) == len(errs) and any(e[0] == c['diag'] for e in inside)
            verdict = 'refused' if ok else f'rc={rc}, errors {errs[:3]}'
        else:
            s = summary(text)
            ok = rc == 0 and s is not None and s[1] == 1 and s[2] == 0
            verdict = 'compiles and passes' if ok else f'positive control failed: rc={rc}, {s}, errors {errs[:3]}'
        rec.update(rc=rc, errors=errs[:5], verdict=verdict)
        if not ok:
            failed.append(f'{key}: {verdict}')
        print(f"{key:52} {c['diag'] or 'control':8} {verdict}", flush=True)

    err = c_blocks(out) or abi(out)
    if err:
        failed.append(err)
    shutil.rmtree(out / 'target', ignore_errors=True)
    rejected = sum(r.get('verdict') == 'rejected' for r in records)
    refused = sum(r.get('verdict') in ('refused', 'compiles and passes') for r in records)
    result = dict(owner='corona', state='failed' if failed else 'passed', declared_tests=declared,
                  mutants=len(variants), rejected=rejected, cases=len(cases), cases_ok=refused,
                  superseded=[f"{v['driver']}:{v['name']}" for v in data['variants'] if v['driver'] in SUPERSEDED],
                  c_code_blocks=len(C_BLOCKS), abi_sabotages=len(ABI), versions=versions,
                  failures=failed, records=records)
    (out / 'result.json').write_text(json.dumps(result, indent=1) + '\n')
    print(f'{rejected}/{len(variants)} mutants rejected; {refused}/{len(cases)} cases as required; '
          f'{len(C_BLOCKS)} C code blocks and {len(ABI)} ABI sabotages refused' if not failed else
          'FAILED:\n  ' + '\n  '.join(failed))
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
