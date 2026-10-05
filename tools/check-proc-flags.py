#!/usr/bin/env python3
"""Gate: no two PROC_FLAG_* defines in proc.h share a bit of proc_flags.

Each flag's _Static_assert names the flags its author knew, so two branches
can each take the same free bit and both compile.  This check derives the set
from the source instead: it evaluates every `#define PROC_FLAG_*` (resolving
the other macros of the header it references) and fails on a shared bit.

A define whose bits are exactly the union of two or more other PROC_FLAG_
defines is a composite mask (e.g. TERMINATE_PENDING_MASK) and may overlap
its constituents; every other define must own its bits.  A define that does
not evaluate fails the gate: an unread flag is not a checked one.

usage: check-proc-flags.py [path/to/proc.h]
"""
import os
import re
import sys

WORD_BITS = 32

def load_defines(text):
    text = re.sub(r'\\\n', ' ', text)
    defs = {}
    for m in re.finditer(r'^[ \t]*#[ \t]*define[ \t]+(\w+)[ \t]+(.+)$', text, re.M):
        body = re.sub(r'//.*$', '', m.group(2))
        body = re.sub(r'/\*.*?\*/', '', body).strip()
        if body:
            defs[m.group(1)] = body
    return defs

def evaluate(name, defs, memo, stack=()):
    if name in memo:
        return memo[name]
    if name in stack:
        raise ValueError('%s: recursive definition' % name)
    expr = re.sub(r'\b(0[xX][0-9a-fA-F]+|\d+)[uU]?[lL]{0,2}\b',
                  lambda m: str(int(m.group(1), 0)), defs[name])
    expr = re.sub(r'\(\s*(u8|u16|u32|u64|uint32_t|uint64_t|unsigned)\s*\)', '', expr)
    for tok in sorted(set(re.findall(r'[A-Za-z_]\w*', expr)), key=len, reverse=True):
        if tok not in defs:
            raise ValueError('%s: references %s, which proc.h does not define' % (name, tok))
        val = evaluate(tok, defs, memo, stack + (name,))
        expr = re.sub(r'\b%s\b' % tok, '(%d)' % val, expr)
    if not re.fullmatch(r'[\s\d()|&^~<>+\-*]+', expr):
        raise ValueError('%s: cannot evaluate %r' % (name, defs[name]))
    val = eval(expr, {'__builtins__': {}}, {})
    memo[name] = val
    return val

def bits(v):
    return [i for i in range(64) if v >> i & 1]

def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        root, 'kernel', 'include', 'thylacine', 'proc.h')
    defs = load_defines(open(path).read())
    names = sorted(n for n in defs if n.startswith('PROC_FLAG_'))
    if not names:
        print('check-proc-flags: no PROC_FLAG_ define found in %s' % path)
        return 1
    memo, vals, bad = {}, {}, []
    for n in names:
        try:
            v = evaluate(n, defs, memo)
        except (ValueError, SyntaxError) as e:
            bad.append(str(e))
            continue
        if v <= 0 or v >= 1 << WORD_BITS:
            bad.append('%s = %#x is outside the %d-bit flag word' % (n, v, WORD_BITS))
        vals[n] = v
    composite = set()
    for n, v in vals.items():
        parts = [m for m, w in vals.items() if m != n and w & v == w and w != v]
        union = 0
        for m in parts:
            union |= vals[m]
        if len(parts) >= 2 and union == v:
            composite.add(n)
    owner = {}
    for n in sorted(vals, key=lambda k: vals[k]):
        if n in composite:
            continue
        for b in bits(vals[n]):
            if b in owner:
                bad.append('bit %d is taken by both %s and %s' % (b, owner[b], n))
            else:
                owner[b] = n
    free = [b for b in range(WORD_BITS) if b not in owner]
    print('check-proc-flags: %d defines (%d composite), %d bits owned, free %s'
          % (len(names), len(composite), len(owner), free or 'none'))
    for msg in bad:
        print('check-proc-flags: FAIL: ' + msg)
    return 1 if bad else 0

if __name__ == '__main__':
    sys.exit(main())
