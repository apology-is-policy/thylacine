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
only outside the named set does not count. The verdict comes from a second run
of the named set alone, serially, as her binaries held only her own file's
tests: in the whole suite a test outside the set that panics during unwinding
aborts the binary before the named tests report. A named test that aborts the
binary is recorded as such, and counts only with her text in its panic.

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
    # The -1000 family below every errno but inside another family's block.
    ('ring_payload.h', '#define T_RING_UNCERTAIN (-1003)', '#define T_RING_UNCERTAIN (-1103)',
     'T_RING_UNCERTAIN lies in the -1000 block'),
    ('fixed_requests.h', '#define T_FIXED_CLOSING (-1000)', '#define T_FIXED_CLOSING (-1100)',
     'T_FIXED_CLOSING lies in the -1000 block'),
    ('retirement_executor.h', '#define T_RETIREMENT_CAPACITY (-1002)', '#define T_RETIREMENT_CAPACITY (-1202)',
     'executor codes lie in the -1000 block'),
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


PANIC = re.compile(r"^thread '([^']+)'(?: \(\d+\))? panicked at ([^\n]+):\n([^\n]*)", re.M)


def panics(text):
    """Test leaf name -> every panic its thread printed, as 'site: message'."""
    out = {}
    for m in PANIC.finditer(text):
        out.setdefault(m.group(1).split('::')[-1], []).append(f'{m.group(2)}: {m.group(3)}')
    return out


def failures(log):
    """Failing test leaf names, and each one's panics."""
    text = Path(log).read_text(errors='replace')
    failed = re.findall(r'^test (\S+) \.\.\. FAILED$', text, re.M)
    p = panics(text)
    return {f.split('::')[-1]: p.get(f.split('::')[-1], []) for f in failed}, text


def test_paths(src):
    """Test leaf name -> its full path in the crate, for an --exact run."""
    out = {}
    for f in sorted((src / 'tests').rglob('*.rs')):
        rel = f.relative_to(src).with_suffix('')
        mod = '::'.join(p for p in rel.parts if p != 'mod')
        for name in re.findall(r'^\s*#\[test\]\s*\n(?:\s*#\[[^\n]*\]\s*\n)*\s*fn (\w+)', f.read_text(), re.M):
            if name in out:
                sys.exit(f'na3-mutants: test {name} is defined twice')
            out[name] = f'{mod}::{name}'
    return out


def summary(text):
    m = re.search(r'^test result: (\w+)\. (\d+) passed; (\d+) failed', text, re.M)
    return (m.group(1), int(m.group(2)), int(m.group(3))) if m else None


def errors(text, host, first_probe_line):
    """rustc errors as (code, file, line, message); and those that lie in the probe."""
    out = []
    for m in re.finditer(r'^error(?:\[(E\d+)\])?: ([^\n]*)\n\s*--> ([^:\n]+):(\d+):\d+', text, re.M):
        out.append((m.group(1), m.group(3), int(m.group(4)), m.group(2)))
    inside = [e for e in out if e[1].endswith(host) and e[2] >= first_probe_line]
    return out, inside


def shows(diag, e):
    """Her runner's diag is an error code or a fragment of the message."""
    return e[0] == diag if re.fullmatch(r'E\d+', diag) else diag in e[3]


def judge(crate, out, paths, v, key, text, pristine, rec):
    """Build and run one mutant (text = the whole mutated core); record and return its verdict."""
    core = crate / 'src' / v['file']
    core.write_text(text)
    stem = 'mutant-' + re.sub(r'[^a-z0-9.-]+', '_', key)
    log, nlog = out / (stem + '.log'), out / (stem + '.named.log')
    named = named_set(v)
    if not named <= paths.keys():
        sys.exit(f'na3-mutants: {key}: named tests not installed: {sorted(named - paths.keys())[:5]}')
    rc = cargo(crate, log, timeout=300)  # the whole suite, for the record
    fails, body = failures(log)
    nrc = 'not run'
    if rc != 'timeout' and 'error: could not compile' not in body:
        nrc = cargo(crate, nlog, '--', '--exact', '--test-threads=1', *sorted(paths[n] for n in named),
                    timeout=300)
    core.write_text(pristine[v['file']])
    want = expect_text(v)
    rec.update(rc=rc, failing=sorted(fails), expect=want, named_rc=nrc)
    if rc == 'timeout' or nrc == 'timeout':
        verdict = 'timeout'
    elif nrc == 'not run':
        verdict = 'did not build' if 'error: could not compile' in body else 'no test binary'
    else:
        hit, ntext = failures(nlog)
        s = summary(ntext)
        aborted = s is None
        if aborted:  # serial, named tests only: every panic is a named test's
            hit = {n: p for n, p in panics(ntext).items() if n in named}
        rec.update(named_failing=sorted(hit), aborted=aborted, named_summary=s)
        if want is None:
            shown = [m for ms in hit.values() for m in ms] or [f'{n} FAILED' for n in hit]
        else:
            shown = [m for ms in hit.values() for m in ms if want in m]
        if nrc == 0 or not hit:
            outside = fails or summary(body) is None
            verdict = 'killed only outside the named set' if outside else 'SURVIVED'
        elif not aborted and s[1] + s[2] != len(named):
            verdict = f'named run counted {s[1] + s[2]} of {len(named)} tests'
        elif not shown:
            verdict = f'no named failure shows {want!r}'
        else:
            verdict = 'rejected'
            rec['shown'] = shown[0]
    rec['verdict'] = verdict
    return verdict

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
    ap.add_argument('--only', help='comma-separated mutant/case keys, for iterating on the driver; not evidence')
    args = ap.parse_args()
    if args.dry:
        sys.exit(dry())
    out = args.out.resolve()  # cargo runs inside the scratch crate: a relative target dir would land there
    out.mkdir(parents=True)
    res = subprocess.check_output([os.path.expanduser('~/.local/bin/yip'), 'resources'], text=True)
    if not re.search(r'^mac +HELD by (you|corona)\b', res, re.M):
        sys.exit('na3-mutants: the mac lease is not held by corona')
    st = os.statvfs(R)
    if st.f_bavail * st.f_frsize < 8 * 1024 ** 3:
        sys.exit('na3-mutants: below the 8 GiB disk floor')
    data, variants, cases = load()
    if args.only:
        keep = set(args.only.split(','))
        variants = [v for v in variants if f"{v['driver'][4:-3]}:{v['name']}" in keep]
        cases = [c for c in cases if f"case:{c['driver'][4:-3]}:{c['name']}" in keep]
        if len(variants) + len(cases) != len(keep):
            sys.exit(f'na3-mutants: --only matched {len(variants) + len(cases)} of {len(keep)} keys')
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
    paths = test_paths(crate / 'src')
    if len(paths) != declared:
        sys.exit(f'na3-mutants: {len(paths)} test paths found, {declared} declared')
    # Negative control: a mutant that changes nothing must come out SURVIVED through the
    # same verdict path, or the driver cannot tell a survivor from a kill.
    v0 = (variants or load()[1])[0]
    noop = pristine[v0['file']] + '// na3-mutants negative control: no behaviour change\n'
    ctl = dict(name='control:no-op', file=v0['file'])
    if judge(crate, out, paths, v0, 'control:no-op', noop, pristine, ctl) != 'SURVIVED':
        sys.exit(f"na3-mutants: negative control: a no-op mutant came out {ctl['verdict']!r}")
    print(f"control: a no-op mutant of {v0['file']} survives {len(named_set(v0))} named tests", flush=True)

    for v in variants:
        key = f"{v['driver'][4:-3]}:{v['name']}"
        rec = dict(name=key, file=v['file'], test=v['test'], filter=v['filter'])
        records.append(rec)
        text, err = mutant_source(v, pristine)
        if err:
            failed.append(f'{key}: {err}'); rec['verdict'] = err; continue
        verdict = judge(crate, out, paths, v, key, text, pristine, rec)
        if verdict != 'rejected':
            failed.append(f'{key}: {verdict}')
        flag = ' (aborted)' if rec.get('aborted') else ''
        print(f"{key:52} {verdict}{flag}  {len(rec.get('failing', []))} failing, {len(rec.get('named_failing', []))} named  "
              f"{rec.get('shown', '')[:80]}", flush=True)

    for c in cases:
        key = f"case:{c['driver'][4:-3]}:{c['name']}"
        rec = dict(name=key, test=c['test'], diag=c['diag'])
        records.append(rec)
        host = HOST[c['test']]
        f = crate / 'src' / host
        orig = f.read_text()
        first = orig.count('\n') + 2
        f.write_text(orig + '\n#[test]\nfn na3_probe() {\n' + c['pre'] + c['body'] + '\n}\n')
        log = out / ('case-' + re.sub(r'[^a-z0-9.-]+', '_', key) + '.log')
        rc = cargo(crate, log, 'na3_probe', timeout=300)
        f.write_text(orig)
        text = log.read_text(errors='replace')
        errs, inside = errors(text, host, first)
        if c['diag']:
            ok = rc != 0 and errs and len(inside) == len(errs) and any(shows(c['diag'], e) for e in inside)
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
    aborted = sum(r.get('verdict') == 'rejected' and r.get('aborted', False) for r in records)
    refused = sum(r.get('verdict') in ('refused', 'compiles and passes') for r in records)
    result = dict(owner='corona', state='failed' if failed else 'passed', only=args.only, declared_tests=declared,
                  mutants=len(variants), rejected=rejected, rejected_by_abort=aborted, cases=len(cases),
                  cases_ok=refused,
                  superseded=[f"{v['driver']}:{v['name']}" for v in data['variants'] if v['driver'] in SUPERSEDED],
                  c_code_blocks=len(C_BLOCKS), abi_sabotages=len(ABI), versions=versions,
                  failures=failed, records=records)
    (out / 'result.json').write_text(json.dumps(result, indent=1) + '\n')
    print(f'{rejected}/{len(variants)} mutants rejected ({aborted} by a named test aborting the binary); '
          f'{refused}/{len(cases)} cases as required; '
          f'{len(C_BLOCKS)} C code blocks and {len(ABI)} ABI sabotages refused' if not failed else
          'FAILED:\n  ' + '\n  '.join(failed))
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
