#!/usr/bin/env python3
"""NA-2b mutant run over the installed native C Loom client cores.

Every C mutant from Astra's drivers (work/oct5-as-r9/astra-takeover/owned-clients/
run-*.py, extracted to astra-c-mutants.json) is applied to the installed core.
Where the storage rewrite changed an anchor's bytes, the PORTED table below
gives the installed equivalent. The NA-2b table adds the storage mutants.

A mutant is rejected only when its NAMED test exits non-zero AND the log
names the check that fired: a test assert ("Assertion failed") or the cores'
fatal hook ("loom-client fatal:"). An NA-2b mutant must also match its own
expected pattern. A timeout, a crash with no named check, or a clean exit
fails the run. The pristine control first builds the same way and requires
every test to print its PASS line, so the harness is shown able to pass.

EQUIVALENT lists the storage mutants that no test can kill, with the reason.

The ABI sabotage runs tools/check-loom-service-abi.py on a cloned copy of
its inputs: once pristine (must pass), once with T_LOOM_CLIENT_ENOSPC at -2
(must fail on its static assert). The tree itself is never edited.

Usage: work/oct12-na2b/na2b-mutants.py --out DIR   (the mac lease must be held)
       work/oct12-na2b/na2b-mutants.py --dry       (anchor counts only; no build)
"""
import argparse, json, os, re, shutil, subprocess, sys
from pathlib import Path

R = Path(__file__).resolve().parents[2]
LLVM = os.environ.get('LLVM_PREFIX', '/opt/homebrew/opt/llvm')
CC = LLVM + '/bin/clang'
SRC = R / 'usr/lib/libt/src/loom_client'
TESTS = R / 'usr/lib/libt/tests/loom_client'
INC = R / 'usr/lib/libt/include'
CORES = ['fixed_requests', 'lease_ledger', 'owned_mapping', 'paired_cq',
         'request_book', 'retirement_executor', 'ring_payload']
PASS = {
    'test_lease_ledger': 'C lease ledger ownership and malformed-vector checks PASS',
    'test_paired_cq': 'C paired CQ geometry/wrap/concurrent100000 PASS',
    'test_owned_mapping': 'C six owned mapping scenarios PASS',
    'test_request_book': 'C seven request correlation scenarios PASS',
    'test_ring_payload': 'C full-ring seven shared ownership scenarios PASS',
    'test_composed_ring': 'C two composed CQ request payload scenarios PASS',
    'test_fixed_requests': 'C eleven typed fixed request scenarios PASS',
    'test_deadlines': 'C eight deadline cancellation scenarios PASS',
    'test_retirement_executor': 'C eight retirement owner scenarios PASS',
    'test_retirement_timers': 'C six retirement timer scenarios PASS',
    'test_fatal': 'C six fatal invariant scenarios PASS',
}
# Astra's driver -> the test it ran (the named test).
DRIVER_TEST = {
    'run-composed-ring.py': 'test_composed_ring', 'run-deadlines-c.py': 'test_deadlines',
    'run-fixed-io-c.py': 'test_ring_payload', 'run-fixed-requests-c.py': 'test_fixed_requests',
    'run-ledger.py': 'test_lease_ledger', 'run-owned-mapping.py': 'test_owned_mapping',
    'run-paired.py': 'test_paired_cq', 'run-registration-c.py': 'test_ring_payload',
    'run-request-book.py': 'test_request_book', 'run-retirement-executor-c.py': 'test_retirement_executor',
    'run-retirement-timers-c.py': 'test_retirement_timers', 'run-ring-payload-c.py': 'test_ring_payload',
}
# payload_owner is superseded by ring_payload and is not installed.
SUPERSEDED = {'run-payload-c.py'}
# (driver, name) -> installed (anchor, replacement) pairs, where the storage
# rewrite changed the original anchor's bytes. Same mutation, new spelling.
PORTED = {
    ('run-fixed-io-c.py', 'pool-fixed-overlap'): [
        ('if(fixed_overlap(member_range(p,k),range))', 'if(0 && fixed_overlap(member_range(p,k),range))')],
    ('run-fixed-requests-c.py', 'unpublished-leak'): [
        ('T_LOOM_CHECK(!t_request_cancel_unpublished(d->book,cookie));', ''),
        ('T_LOOM_CHECK(!t_request_cancel_unpublished(d->book,r->cookie));', '')],
    ('run-registration-c.py', 'global-cap'): [
        ('if(n>64-c->members)return T_LOOM_CLIENT_ENOSPC;', '')],
    ('run-ring-payload-c.py', 'global-cap'): [
        ('if(n>64-c->members)return T_LOOM_CLIENT_ENOSPC;', '')],
    ('run-registration-c.py', 'rollback-no-refund'): [
        ('c->members-=n;release_pool(p);return result;', 'release_pool(p);return result;')],
    ('run-retirement-executor-c.py', 'missing-retire'): [
        ('cell->retiring=1;cell->client_alive=0;t_fixed_begin_retire(cell->driver);',
         'cell->retiring=1;cell->client_alive=0;')],
}
REGISTER = ('struct t_ring_pool_state *p=NULL;int rc=prepare(c,ranges,n,&p);if(rc)return rc;\n'
            ' c->members+=n;struct t_loom_service_ref identity={0};\n'
            ' /* Pass an immutable contiguous copy; bank cells are not ABI input. */\n'
            ' struct t_loom_service_pool_member copy[64];for(size_t i=0;i<n;++i)copy[i]=member_range(p,i);\n'
            ' int64_t result=c->b.register_pool(c->b.context,copy,n,&identity);*native_rc=result;')
# name -> (core, [(anchor, replacement, count)], named test, expected kill pattern)
NA2B = {
    'claim-after-register': ('ring_payload', [(REGISTER,
        'struct t_loom_service_ref identity={0};int64_t result=c->b.register_pool(c->b.context,ranges,n,&identity);*native_rc=result;\n'
        ' struct t_ring_pool_state *p=NULL;int rc=prepare(c,ranges,n,&p);if(rc)return rc;\n c->members+=n;', 1)],
        'test_ring_payload', r'at_register==before\+1\+n'),
    'refusal-keeps-claims': ('ring_payload', [('c->members-=n;release_pool(p);return result;', 'c->members-=n;return result;', 1)],
        'test_ring_payload', r'claimed\(core\)==\(result==0'),
    'pool-overlap-keeps-claims': ('ring_payload', [('{release_pool(p);return T_LEASE_GEOMETRY;}}}}\n', '{return T_LEASE_GEOMETRY;}}}}\n', 1)],
        'test_ring_payload', r'claimed\(core\)==before'),
    'fixed-overlap-keeps-claims': ('ring_payload', [('{release_pool(p);return T_LEASE_GEOMETRY;}}}\n', '{return T_LEASE_GEOMETRY;}}}\n', 1)],
        'test_ring_payload', r'claimed\(r->core\)==held'),
    'bank-cells-leak': ('ring_payload', [('t_lease_release(&p->ledger);t_bytes_zero(p,sizeof(*p));', 't_bytes_zero(p,sizeof(*p));', 1)],
        'test_ring_payload', r'.'),
    'final-release-keeps-core': ('ring_payload', [('c->b.destroy(c->b.context);t_bytes_zero(c,sizeof(*c));', 'c->b.destroy(c->b.context);', 1)],
        'test_ring_payload', r't_ring_idle'),
    'ring-create-on-live': ('ring_payload', [('T_LOOM_CHECK(!c->in_use);', '', 1)],
        'test_fatal', r't_ring_create: no fatal there'),
    'fixed-create-on-live': ('fixed_requests', [('T_LOOM_CHECK(t_fixed_storage_idle(s));', '', 1)],
        'test_fatal', r't_fixed_create: no fatal there'),
    'mapping-create-on-live': ('owned_mapping', [('T_LOOM_CHECK(!m->live);', '', 1)],
        'test_fatal', r't_mapping_create: no fatal there'),
    'executor-create-on-live': ('retirement_executor', [('T_LOOM_CHECK(!e->refs);', '', 1)],
        'test_fatal', r't_retirement_create: no fatal there'),
    'fixed-write-drops-bytes': ('ring_payload', [('t_bytes_copy((unsigned char *)c->b.buffer(c->b.context,f->range.buffer_index)+(size_t)f->range.offset,bytes,len);', '(void)bytes;', 1)],
        'test_ring_payload', r'.'),
    'request-claimed-before-book': ('fixed_requests', [('uint64_t cookie=0;int rc=t_request_reserve(', 'if(!d->failed)(void)claim(d);uint64_t cookie=0;int rc=t_request_reserve(', 1)],
        'test_fixed_requests', r'.'),
    'read-drop-keeps-cell': ('fixed_requests', [('t_ring_fixed_drop(r->fixed);release_request(cell);}', 't_ring_fixed_drop(r->fixed);}', 1)],
        'test_fixed_requests', r't_fixed_storage_idle'),
    'ledger-release-keeps-count': ('lease_ledger', [('b->count=0;b->outstanding=0;b->bound=1;', 'b->outstanding=0;b->bound=1;', 1)],
        'test_lease_ledger', r'.'),
}
# Storage mutants no test can kill, and why. The audit weighs these.
EQUIVALENT = {
    'leases-not-cleared': 'release_pool runs only once every lease of the pool is inactive and not '
        'pending (destroy needs !live && !outstanding; a definite or local refusal precedes any accept), '
        'and prepare rewrites .pool while accept rewrites the token, so the clear is hygiene only.',
    'idle-ignores-request-cells': 'every used request cell holds a fixed handle (read-drop-keeps-cell '
        'is the mutant that separates them), so the ring is not idle while a cell is used.',
    'executor-release-without-zero': 'create zeroes the storage, and a stale managed drop after release '
        'already fails cell->driver&&!cell->retiring, since the first drop set retiring.',
}


def make_plan(records):
    plan = []
    for m in json.load(open(R / 'work/oct12-na2b/astra-c-mutants.json')):
        if m['driver'] in SUPERSEDED:
            records.append(dict(name=m['driver'] + ':' + m['name'], verdict='superseded (payload_owner not installed)'))
            continue
        key = (m['driver'], m['name'])
        orig = (R / 'work/oct5-as-r9/astra-takeover/owned-clients' / m['file']).read_text().count(m['a'])
        edits = [(a, b, None) for a, b in PORTED[key]] if key in PORTED else [(m['a'], m['b'], orig)]
        plan.append(dict(name='astra:' + m['driver'][4:-3] + ':' + m['name'], core=m['file'][:-2], edits=edits,
                         test=DRIVER_TEST[m['driver']], expect=None, ported=key in PORTED))
    for name, (core, edits, test, expect) in NA2B.items():
        plan.append(dict(name='na2b:' + name, core=core, edits=edits, test=test, expect=expect, ported=False))
    return plan


def sh(args, log, timeout=120):
    with open(log, 'wb') as f:
        try:
            return subprocess.run([str(a) for a in args], stdout=f, stderr=subprocess.STDOUT, timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            return 'timeout'


def sdk():
    return subprocess.check_output(['xcrun', '--show-sdk-path'], text=True).strip()


def build(dirpath, core_dir, tests, sdk_path):
    """Build the cores from core_dir and link each named test against them."""
    common = [CC, '-isysroot', sdk_path, '-Wall', '-Wextra', '-Werror', '-O0', '-g', '-DT_REQUEST_TEST', '-I', INC]
    objs = []
    for c in CORES:
        o = dirpath / (c + '.o')
        rc = sh(common + ['-std=c99', '-c', core_dir / (c + '.c'), '-o', o], dirpath / (c + '.build.log'))
        if rc != 0:
            return None, c + ' build failed'
        objs.append(o)
    hf = dirpath / 'host_fatal.o'
    if sh(common + ['-std=c11', '-c', TESTS / 'host_fatal.c', '-o', hf], dirpath / 'host_fatal.build.log') != 0:
        return None, 'host_fatal build failed'
    bins = {}
    for t in tests:
        b = dirpath / t
        if sh(common + ['-std=c11', TESTS / (t + '.c'), *objs, hf, '-lpthread', '-o', b], dirpath / (t + '.build.log')) != 0:
            return None, t + ' build failed'
        bins[t] = b
    return bins, None


def prune(d):
    """Keep the logs; build products are the disk the 8 GiB floor protects."""
    for f in sorted(Path(d).rglob('*'), reverse=True):
        if f.is_dir():
            if not any(f.iterdir()):
                f.rmdir()
        elif not f.name.endswith('.log'):
            f.unlink()


def killed(log):
    text = Path(log).read_text(errors='replace')
    return [l for l in text.splitlines() if 'Assertion failed' in l or 'loom-client fatal:' in l
            or 'no fatal there' in l]


def abi_sabotage(out):
    """The checker on a clone of its inputs: pristine passes, -2 fails."""
    for label, sabotage in (('abi-pristine', False), ('abi-sabotage', True)):
        root = out / label
        for rel in ('arch/arm64', 'kernel/include', 'usr/lib/libt/include'):
            (root / rel).parent.mkdir(parents=True, exist_ok=True)
            subprocess.run(['cp', '-cR', str(R / rel), str(root / rel)], check=True)
        rs = root / 'usr/lib/libthyla-rs/src/loom'; rs.mkdir(parents=True)
        shutil.copy2(R / 'usr/lib/libthyla-rs/src/loom/service_abi.rs', rs / 'service_abi.rs')
        if sabotage:
            base = root / 'usr/lib/libt/include/thyla/loom_client/base.h'
            s = base.read_text(); a = '#define T_LOOM_CLIENT_ENOSPC (-28)'
            if s.count(a) != 1:
                return f'{label}: anchor count {s.count(a)}'
            base.write_text(s.replace(a, '#define T_LOOM_CLIENT_ENOSPC (-2)'))
        rc = sh([sys.executable, R / 'tools/check-loom-service-abi.py', '--root', root], out / (label + '.log'), timeout=300)
        text = (out / (label + '.log')).read_text(errors='replace')
        if not sabotage and rc != 0:
            return f'{label}: the pristine clone failed (rc={rc})'
        if sabotage and (rc == 0 or '"T_LOOM_CLIENT_ENOSPC"' not in text):
            return f'{label}: rc={rc} without the T_LOOM_CLIENT_ENOSPC static assert'
        print(f'{label}: rc={rc}', flush=True)
        shutil.rmtree(root)
    return None


def anchors(plan):
    bad = []
    for p in plan:
        s = (SRC / (p['core'] + '.c')).read_text()
        for a, b, n in p['edits']:
            cnt = s.count(a)
            if cnt == 0 or (n is not None and cnt != n):
                bad.append(f"{p['name']}: anchor count {cnt}, want {n or '>=1'}")
            s = s.replace(a, b)
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', type=Path)
    ap.add_argument('--dry', action='store_true')
    args = ap.parse_args()
    if args.dry:
        plan = make_plan([])
        bad = anchors(plan)
        print('\n'.join(bad) or f'{len(plan)} mutants, every anchor found')
        sys.exit(1 if bad else 0)
    out = args.out
    out.mkdir(parents=True)
    res = subprocess.check_output([os.path.expanduser('~/.local/bin/yip'), 'resources'], text=True)
    if not re.search(r'^mac +HELD by (you|corona)\b', res, re.M):
        sys.exit('na2b-mutants: the mac lease is not held by corona')
    st = os.statvfs(R)
    if st.f_bavail * st.f_frsize < 8 * 1024 ** 3:
        sys.exit('na2b-mutants: below the 8 GiB disk floor')
    sdk_path = sdk()
    records = []

    # Control: the pristine cores, every test, each printing its PASS line.
    ctl = out / 'pristine'; ctl.mkdir()
    bins, err = build(ctl, SRC, list(PASS), sdk_path)
    if err:
        sys.exit('na2b-mutants: control: ' + err)
    for t, b in bins.items():
        rc = sh([b], ctl / (t + '.log'))
        lines = (ctl / (t + '.log')).read_text(errors='replace').splitlines()
        if rc != 0 or PASS[t] not in lines:
            sys.exit(f'na2b-mutants: control: {t} rc={rc} without its PASS line')
    print(f'control: {len(bins)} tests PASS', flush=True)
    prune(ctl)

    plan = make_plan(records)
    bad = anchors(plan)
    if bad:
        sys.exit('na2b-mutants: ' + '; '.join(bad))
    err = abi_sabotage(out)
    if err:
        sys.exit('na2b-mutants: ' + err)

    failures = []
    for p in plan:
        d = out / re.sub(r'[^a-z0-9.-]+', '_', p['name']); d.mkdir()
        cores = d / 'src'; shutil.copytree(SRC, cores)
        f = cores / (p['core'] + '.c'); s = f.read_text()
        for a, b, n in p['edits']:
            cnt = s.count(a)
            if cnt == 0 or (n is not None and cnt != n):
                failures.append(f"{p['name']}: anchor count {cnt}, want {n or '>=1'}"); break
            s = s.replace(a, b)
        else:
            f.write_text(s)
            bins, err = build(d, cores, [p['test']], sdk_path)
            if err:
                failures.append(f"{p['name']}: {err}"); continue
            rc = sh([bins[p['test']]], d / 'run.log', timeout=60)
            lines = killed(d / 'run.log')
            rec = dict(name=p['name'], test=p['test'], rc=rc, kill=lines[:3], ported=p['ported'])
            records.append(rec)
            if rc == 0 or rc == 'timeout' or not lines:
                failures.append(f"{p['name']}: survived or unnamed (rc={rc})")
            elif p['expect'] and not any(re.search(p['expect'], l) for l in lines):
                failures.append(f"{p['name']}: killed elsewhere: {lines[0]}")
            shown = next((l for l in lines if p['expect'] and re.search(p['expect'], l)), lines[0] if lines else '')
            rec['shown'] = shown
            print(f"{p['name']:58} rc={rc} {shown[:110]}", flush=True)
        prune(d)
    for name, why in EQUIVALENT.items():
        records.append(dict(name='na2b:' + name, verdict='equivalent', why=why))
    (out / 'result.json').write_text(json.dumps(dict(mutants=len(plan), failures=failures, records=records), indent=1) + '\n')
    if failures:
        print('\n'.join(failures))
        sys.exit(f'na2b-mutants: {len(failures)} of {len(plan)} mutants not rejected')
    print(f'na2b-mutants: PASS -- {len(plan)} mutants rejected, each by a named check')


if __name__ == '__main__':
    main()
