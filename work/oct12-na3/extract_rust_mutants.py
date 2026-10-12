#!/usr/bin/env python3
"""Extract Astra's Rust source mutants and compile-fail cases from her runners.

Reads work/oct5-as-r9/astra-takeover/owned-clients/run-*.py as Python syntax
trees (never executes them) and writes astra-rust-mutants.json beside this file:
  variants: driver, name, file (the core .rs), a, b, test file, filter, expect,
            replace_all (her runner replaced every match rather than requiring one)
  cases:    driver, name, pre, body, diag (an rustc error code, or null = must compile);
            pre is what her runner's `fn main(){` puts before the body
The `expect` of a mutant run is the runner's own source text for it, kept for review.
"""
import ast, json, re, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
D = HERE.parents[1] / 'work/oct5-as-r9/astra-takeover/owned-clients'
out = dict(variants=[], cases=[], skipped=[])


def lit(node):
    return ast.literal_eval(node)


def terms(n):
    """The operands of a chain of string `+`, left to right."""
    if isinstance(n, ast.BinOp) and isinstance(n.op, ast.Add):
        return terms(n.left) + terms(n.right)
    return [n]


def wrappers(tree):
    """(pre, post) around `body` in every `...'fn main(){' + body + '}...'` the runner builds."""
    found = set()
    for n in ast.walk(tree):
        ops = terms(n)
        for i, o in enumerate(ops):
            if (isinstance(o, ast.Name) and o.id == 'body' and 0 < i < len(ops) - 1
                    and isinstance(ops[i - 1], ast.Constant) and 'fn main(){' in str(ops[i - 1].value)
                    and isinstance(ops[i + 1], ast.Constant)):
                found.add((ops[i - 1].value.split('fn main(){', 1)[1], ops[i + 1].value))
    return found


for drv in sorted(D.glob('run-*.py')):
    src = drv.read_text()
    if 'rustc' not in src:
        continue
    tree = ast.parse(src)
    assigns = {}
    for n in ast.walk(tree):
        if isinstance(n, ast.Assign) and len(n.targets) == 1 and isinstance(n.targets[0], ast.Name):
            assigns.setdefault(n.targets[0].id, n.value)
    test_file = re.search(r"run/'(test_[a-z_]+\.rs)'", src).group(1)
    # The Rust mutant run: cmd([binary(,'filter')?], label, True(, expect)?), the
    # first call that expects failure; `expect` is kept as her source expression.
    filt = expect = None
    for n in ast.walk(tree):
        if (isinstance(n, ast.Call) and getattr(n.func, 'id', None) == 'cmd' and len(n.args) >= 3
                and isinstance(n.args[0], ast.List) and getattr(n.args[0].elts[0], 'id', None) == 'binary'
                and isinstance(n.args[2], ast.Constant) and n.args[2].value is True):
            elts = n.args[0].elts
            filt = elts[1].value if len(elts) > 1 and isinstance(elts[1], ast.Constant) and elts[1].value != '--nocapture' else None
            expect = ast.get_source_segment(src, n.args[3]) if len(n.args) > 3 else None
            break
    # Her runner either requires one match (==1) or replaces every match (>=1).
    replace_all = 's.count(a)>=1' in src
    rust_file = None
    m = re.search(r"src=m/'([a-z_]+\.rs)'", src)
    if m:
        rust_file = m.group(1)
    # Variant formats, by the shape of the loop that consumes them.
    v = assigns.get('variants')
    if v is not None:
        val = lit(v)
        items = []
        if isinstance(val, dict):
            for name, x in val.items():
                if isinstance(x, list):  # {'rust': [(name,a,b)...], 'c': [...]}
                    if name == 'rust':
                        items += [(n, 'request_book.rs', a, b) for n, a, b in x]
                elif len(x) == 4:  # (rust_a, c_a, rust_b, c_b)
                    f = {'run-ledger.py': 'lease_ledger.rs', 'run-paired.py': 'paired_cq.rs'}[drv.name]
                    items.append((name, f, x[0], x[2]))
                elif len(x) == 3:  # (target.rs, a, b)
                    items.append((name, x[0], x[1], x[2]))
                elif len(x) == 2:  # (a, b) into src=m/'X.rs'
                    if not rust_file:
                        out['skipped'].append(dict(driver=drv.name, name=name, why='no Rust target'))
                        continue
                    items.append((name, rust_file, x[0], x[1]))
        else:  # [(lang, name, file, a, b)]
            items += [(n, f, a, b) for lang, n, f, a, b in val if lang == 'rust']
        for name, f, a, b in items:
            out['variants'].append(dict(driver=drv.name, name=name, file=f, a=a, b=b, test=test_file,
                                        filter=filt, expect=expect, replace_all=replace_all))
    c = assigns.get('cases')
    cases = []
    if c is not None:
        for name, (body, diag) in lit(c).items():
            cases.append((name, body, diag))
    for n in ast.walk(tree):  # for name,body,diag in [(...)]
        if isinstance(n, ast.For) and isinstance(n.iter, ast.List) and isinstance(n.target, ast.Tuple) \
                and [getattr(e, 'id', None) for e in n.target.elts] == ['name', 'body', 'diag']:
            cases += [tuple(lit(e)) for e in n.iter.elts]
    if cases:
        w = wrappers(tree)
        if len(w) != 1 or not next(iter(w))[1].startswith('}'):
            sys.exit(f'{drv.name}: expected one fn main wrapper around body, found {sorted(w)}')
        pre = next(iter(w))[0]
    for name, body, diag in cases:
        out['cases'].append(dict(driver=drv.name, name=name, pre=pre, body=body, diag=diag, test=test_file))

(HERE / 'astra-rust-mutants.json').write_text(json.dumps(out, indent=1) + '\n')
by = {}
for v in out['variants']:
    by.setdefault(v['driver'], []).append(v)
for drv, vs in by.items():
    print(f"{drv:30} {len(vs):2} variants  test={vs[0]['test']:28} filter={vs[0]['filter']!s:10} expect={vs[0]['expect']}")
print(len(out['variants']), 'variants;', len(out['cases']), 'cases;', len(out['skipped']), 'skipped', out['skipped'])
