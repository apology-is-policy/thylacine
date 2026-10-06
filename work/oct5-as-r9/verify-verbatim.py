#!/usr/bin/env python3
"""Prove old-source.c is VERBATIM kernel/burrow.c, not a paraphrase.

A fixture that merely resembles the shipped source proves nothing about the
shipped source. Scans old-source.c for column-0 function blocks and requires
each to appear byte-identically in kernel/burrow.c. Prints the denominator so a
zero-block run cannot pass as agreement.
"""
import sys, pathlib
W = pathlib.Path(__file__).resolve().parent
root = W.parents[1]
src = (root / 'kernel/burrow.c').read_text()
lines = (W / 'old-source.c').read_text().splitlines(keepends=True)

blocks, cur = [], None
for ln in lines:
    if cur is None:
        if ln[:1].isalpha() and '(' in ln:
            cur = [ln]
    else:
        cur.append(ln)
        if ln.rstrip('\n') == '}':
            blocks.append(''.join(cur)); cur = None
if cur is not None:
    print('FAIL: unterminated final block'); sys.exit(1)

EXPECTED = 4
print(f'function blocks found in old-source.c: {len(blocks)} (expected {EXPECTED})')
if len(blocks) != EXPECTED:
    print(f'FAIL: splitter found {len(blocks)}, not {EXPECTED} -- do not trust a partial comparison')
    sys.exit(1)
bad = 0
for b in blocks:
    name = b.split('(')[0].split()[-1].lstrip('*')
    if b in src:
        print(f'  VERBATIM  {name:38s} {len(b):5d} bytes')
    else:
        print(f'  DIVERGES  {name:38s} NOT byte-identical to kernel/burrow.c')
        bad += 1
print('RESULT:', 'all blocks verbatim against kernel/burrow.c' if not bad else f'{bad} divergent block(s)')
sys.exit(1 if bad else 0)
