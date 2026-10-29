#!/usr/bin/env python3
"""Sweep for AS-R9's CLASS, not just its instances.

AS-R9 was "a Burrow pointer used after the reference that kept it alive was
dropped". Fixing the six known sites proves nothing about whether the same shape
lives elsewhere, so this looks for ANY use of a dropped pointer after its drop,
in every kernel .c file.

For each call to a reference-dropping function, it takes the dropped expression
and reports any later mention of that identifier inside the same function. It
over-reports by design -- a reassignment or a NULL-check is a legitimate later
mention -- so every hit is triaged by hand below. A sweep that reported nothing
would be the suspicious outcome: it would mean the matcher never fired.
"""
import pathlib, re, sys
R = pathlib.Path(__file__).resolve().parents[2]
DROPS = ['burrow_unref_freed', 'burrow_unref_settled_in', 'burrow_unref_settled',
         'burrow_unref', 'burrow_release_mapping_freed', 'burrow_release_mapping_deferred',
         'burrow_release_mapping_settled_deferred', 'burrow_release_mapping',
         'burrow_free_deferred', 'burrow_free_internal']
pat = re.compile(r'\b(' + '|'.join(DROPS) + r')\s*\(\s*([A-Za-z_][A-Za-z0-9_\[\]\.>-]*)')

files = sorted(p for p in (R / 'kernel').rglob('*.c'))
files += sorted(p for p in (R / 'mm').rglob('*.c')) if (R / 'mm').is_dir() else []
calls = hits = 0
for f in files:
    lines = f.read_text().splitlines()
    # crude function boundaries: a column-0 '}' ends the enclosing function
    for i, ln in enumerate(lines):
        m = pat.search(ln)
        if not m or ln.lstrip().startswith(('//', '*')):
            continue
        fn, var = m.group(1), m.group(2)
        base = re.split(r'[\[\.>-]', var)[0]
        if base in ('NULL', 'v') and fn == 'burrow_free_internal':
            pass
        calls += 1
        # scan forward to the end of this function
        drop_indent = len(ln) - len(ln.lstrip())
        for j in range(i + 1, len(lines)):
            if lines[j].rstrip() == '}':
                break
            later = lines[j]
            if not later.strip() or later.lstrip().startswith(('//', '*')):
                continue
            # Control flow: a false positive in the first cut was weft.c:312,
            # where the drop sits on a table-full branch that RETURNS and the
            # later store is on the mutually exclusive success path. Stop at a
            # return/goto/break at or left of the drop's indentation, and at any
            # dedent past it -- both mean the drop's branch is over.
            ind = len(later) - len(later.lstrip())
            if ind < drop_indent:
                break
            if ind <= drop_indent and re.match(r'(return|goto|break|continue)\b', later.lstrip()):
                break
            if re.search(r'\b' + re.escape(base) + r'\b', later):
                # a reassignment or a NULL store is not a use of the dead object
                if re.search(r'\b' + re.escape(base) + r'\s*=\s*(?!=)', later):
                    break
                hits += 1
                print(f'{f.relative_to(R)}:{i+1}  {fn}({var})')
                print(f'    then :{j+1}  {later.strip()[:100]}')
                break
print(f'\nscanned {len(files)} files, {calls} drop call sites, {hits} later-mention(s) to triage')
if calls == 0:
    print('SUSPECT: zero call sites found -- the matcher did not fire, so this proves nothing')
    sys.exit(1)
