#!/usr/bin/env python3
"""Derive the loom-client crate's cores and tests from Astra's uninstalled Rust twins.

Every installed file is rustfmt(edits(original)): the edits are listed here,
each with its reason, and each must match exactly once. --check proves the tree
still equals that derivation, so the install is mechanical except for the listed
edits. The mutant driver (na3-mutants.py) uses the same derivation to carry each
of Astra's source mutants onto the installed, reformatted code.

  na3_install.py --check          the tree equals the derivation (exit 1 if not)
  na3_install.py --write          create missing files; refuse to change a differing one
"""
from pathlib import Path
import argparse, re, subprocess, sys

ROOT = Path(__file__).resolve().parents[2]
ORIG = ROOT / 'work/oct5-as-r9/astra-takeover/owned-clients'
CRATE = ROOT / 'usr/lib/loom-client'
CORES = ['lease_ledger', 'paired_cq', 'owned_mapping', 'ring_payload', 'request_book',
         'fixed_requests', 'retirement_executor', 'fixed_futures', 'local_tasks',
         'local_wait', 'task_loop']

# (anchor, old, new, reason): the anchor occurs once in the file and `old` once in
# the anchor; a code edit swaps or extends one path token, so it cannot delete
# a statement. Doc lines lose "uninstalled"; the 64-member envelope gets its own
# answer (docs/ASYNC-SERVICE-ABI.md, "Native Rust client cores").
CORE_EDITS = {
    'paired_cq': [('//! Uninstalled private client CQ reader.', 'Uninstalled private', 'Private', 'installed')],
    'owned_mapping': [('//! Private pooled-ring mapping owner. Uninstalled; native setup remains gated.', 'Uninstalled; native', 'Native', 'installed')],
    'ring_payload': [
        ('//! Uninstalled full-ring Rust payload ownership; syscall adapter still pending.',
         'Uninstalled full-ring Rust payload ownership; syscall adapter still pending',
         'Full-ring Rust payload ownership; the syscall adapter is not written yet', 'installed'),
        ('pub(crate) enum Error{Closing,Geometry,Busy,Stale,', 'Busy', 'Busy,Full', 'envelope answer, C -ENOSPC'),
        ('if ranges.len()>64-s.members{return Err(Error::Busy);}', 'Error::Busy', 'Error::Full',
         'envelope answer, C -ENOSPC'),
    ],
    'request_book': [('//! Uninstalled request/CQ correlation', 'Uninstalled request', 'Request', 'installed')],
    'fixed_requests': [('//! Typed fixed request ownership, uninstalled.', ', uninstalled.', '.', 'installed')],
    'local_tasks': [('//! Uninstalled bounded creator-thread', 'Uninstalled bounded', 'Bounded', 'installed')],
    'local_wait': [('//! Uninstalled creator-thread wait', 'Uninstalled creator', 'Creator', 'installed')],
}

# The test files are cumulative copies: test_task_loop.rs holds every earlier
# file's tests, so it becomes tests/ring.rs, and each other file contributes
# only the lines task_loop lacks (union_check() proves no test lost a line).
RING_CHILDREN = {  # child module -> (source, first line, last line), 1-based inclusive
    'composed': ('test_composed_ring.rs', 81, 106),
    'deadlines': ('test_deadlines.rs', 246, 277),
    'local_progress': ('test_local_progress.rs', 246, 246),
    'executor': ('test_retirement_executor.rs', 158, 188),
}
STANDALONE = ['lease_ledger', 'owned_mapping', 'paired_cq', 'request_book']
# NA-3's own tests, for adapter-facing paths no original test reaches (the
# crate-root allow(dead_code) of the original test binaries hid them). Each is a
# child of the module whose fixtures it uses; its file is written by hand, not derived.
OWN_CHILDREN = {'ring': ['seams'], 'request_book': ['seams']}
TEST_EDITS = {
    'ring': [
        ('assert_eq!(r.members(),64);assert!(matches!(unsafe{c.attach_pool(id(2,1),&[range(512)])},Err(Error::Busy)));',
         'Error::Busy', 'Error::Full', 'envelope answer'),
        ('let a=register(&c,&ranges).unwrap();assert!(matches!(register(&c,&[range(512)]),Err(Error::Busy)));',
         'Error::Busy', 'Error::Full', 'envelope answer'),
    ],
}
# (test, lint, reason): an allow attribute on one test, from this list of lints only,
# for a construct the test needs; the runner fails on any warning, so it cannot be
# allowed wider than the test.
ALLOWABLE = {'unreachable_code'}
TEST_ALLOWS = {
    'ring': [('task_capacity_precedes_factory_and_ids_are_owner_specific', 'unreachable_code',
              'a factory that must never run panics before the future its type needs')],
}


def edit(text, edits, where):
    for anchor, old, new, _ in edits:
        n, m = text.count(anchor), anchor.count(old)
        if n != 1 or m != 1:
            raise SystemExit(f'{where}: anchor x{n}, token x{m} in anchor: {anchor[:70]!r}')
        # Outside a doc line `old` is one path token and `new` is a token list that
        # replaces it or keeps it beside an added element, so no edit can drop code.
        tok = r'[A-Za-z_][A-Za-z0-9_:]*'
        parts = new.split(',')
        if not anchor.startswith('//!') and not (re.fullmatch(tok, old) and all(re.fullmatch(tok, t) for t in parts)
                                                 and (len(parts) == 1 or old in parts)):
            raise SystemExit(f'{where}: a code edit must swap or extend one path token: {old!r} -> {new!r}')
        text = text.replace(anchor, anchor.replace(old, new))
    return text


def allow(text, allows, where):
    for test, lint, _ in allows:
        head = f'#[test]fn {test}('
        if lint not in ALLOWABLE or text.count(head) != 1 or f'\n{head}' not in text:
            raise SystemExit(f'{where}: cannot allow {lint} on {test}')
        text = text.replace(f'\n{head}', f'\n#[allow({lint})]\n{head}')
    return text


def module_lines(text):
    """`#[path=...] mod x;` and `mod a;mod b;` become uses of the crate's modules."""
    out = []
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.startswith('#[path=') and line.rstrip().endswith(']'):
            line = line + lines[i + 1]
            i += 1
        m = re.fullmatch(r'#\[path="[^"]*/service_abi\.rs"\]\s*mod service_abi;', line)
        if m:
            out.append('use crate::service_abi;')
        elif re.fullmatch(r'#\[path="lease_ledger\.rs"\] mod ledger;', line):
            out.append('use crate::lease_ledger as ledger;')
        elif re.fullmatch(r'(mod [a-z_]+;)+', line):
            names = re.findall(r'mod ([a-z_]+);', line)
            out.append('use crate::' + (names[0] if len(names) == 1 else '{' + ','.join(names) + '}') + ';')
        else:
            out.append(line)
        i += 1
    return '\n'.join(out) + '\n'


def core_source(name, post=None):
    text = edit((ORIG / f'{name}.rs').read_text(), CORE_EDITS.get(name, []), name)
    return post(text) if post else text


def carry(name, s):
    """A fragment of the original as the edits leave it: each edit whose anchor lies inside."""
    for anchor, old, new, _ in CORE_EDITS.get(name, []):
        s = s.replace(anchor, anchor.replace(old, new))
    return s


def test_sources():
    out = {}
    ring = (ORIG / 'test_task_loop.rs').read_text()
    ring = module_lines(allow(edit(ring, TEST_EDITS['ring'], 'ring'), TEST_ALLOWS['ring'], 'ring'))
    out['tests/ring.rs'] = ring + '\n' + ''.join(f'mod {c};\n' for c in [*RING_CHILDREN, *OWN_CHILDREN['ring']])
    for child, (src, a, b) in RING_CHILDREN.items():
        lines = (ORIG / src).read_text().splitlines()[a - 1:b]
        body = module_lines('\n'.join(lines))
        out[f'tests/ring/{child}.rs'] = 'use super::*;\n' + body
    for name in STANDALONE:
        text = module_lines((ORIG / f'test_{name}.rs').read_text())
        out[f'tests/{name}.rs'] = text + ''.join(f'\nmod {c};\n' for c in OWN_CHILDREN.get(name, []))
    return out


def rustfmt(text, where):
    r = subprocess.run(['rustfmt', '--edition', '2024', '--emit', 'stdout'], input=text,
                       capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(f'rustfmt failed on {where}:\n{r.stderr}')
    return r.stdout


def derive(post=None, only=None):
    """Installed path (relative to src/) -> content. post=(core, fn) rewrites one edited core."""
    out = {}
    for name in CORES:
        if only and name != only:
            continue
        fn = post[1] if post and post[0] == name else None
        out[f'{name}.rs'] = rustfmt(core_source(name, fn), name)
    if not only:
        for path, text in test_sources().items():
            out[path] = rustfmt(text, path)
    return out


def tests_of(text):
    """Test name -> its non-blank source lines, from the #[test] line up to the next item."""
    tests, cur = {}, None
    for line in text.splitlines():
        m = re.match(r'#\[test\]\s*fn ([A-Za-z0-9_]+)', line)
        if m:
            cur = m.group(1)
            tests[cur] = [line]
        elif cur and (line.startswith(' ') or line == '}'):
            tests[cur].append(line)
        else:
            cur = None
    return tests


def union_check():
    """Every original test survives with every line, in the installed (pre-rustfmt) union."""
    installed = {}
    for path, text in test_sources().items():
        for name, lines in tests_of(text).items():
            if name in installed:
                raise SystemExit(f'test {name} installed twice')
            installed[name] = lines
    edited = {a: a.replace(old, new) for _, edits in TEST_EDITS.items() for a, old, new, _ in edits}
    seen = 0
    for src in sorted(ORIG.glob('test_*.rs')):
        if src.name == 'test_payload_owner.rs':
            continue  # payload_owner is superseded by ring_payload and not installed
        for name, lines in tests_of(src.read_text()).items():
            seen += 1
            if name not in installed:
                raise SystemExit(f'{src.name}: test {name} is not installed')
            have = set(installed[name])
            for line in lines:
                want = line
                for a, b in edited.items():
                    want = want.replace(a, b)
                if want not in have:
                    raise SystemExit(f'{src.name}: {name} lost a line: {line[:90]!r}')
    return seen, len(installed)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--write', action='store_true')
    args = ap.parse_args()
    seen, unique = union_check()
    print(f'union: {seen} original test definitions -> {unique} installed tests, none lost a line')
    bad = 0
    for rel, text in derive().items():
        p = CRATE / 'src' / rel
        if p.exists() and p.read_text() == text:
            continue
        if args.write and not p.exists():
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(text)
            print(f'wrote {p.relative_to(ROOT)}')
            continue
        print(f'DIFFERS: {p.relative_to(ROOT)}')
        bad += 1
    if bad:
        sys.exit(1)
    print(f'PASS: {len(CORES)} cores and the tests equal rustfmt(listed edits(originals))')


if __name__ == '__main__':
    main()
