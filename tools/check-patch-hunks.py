#!/usr/bin/env python3
"""Validate unified-diff hunk line counts across the hand-written patch series.

WHY THIS EXISTS. A hunk header `@@ -a,b +c,d @@` does not merely describe the
body -- `patch` TRUSTS it. It consumes body lines until both promised counts
are satisfied and then stops. If `d` is smaller than the number of new lines
actually written, the surplus is DISCARDED: silently, exit 0, no warning, no
`.rej`. When the discarded lines sit at the end of the hunk (the ordinary shape
of an append) the patch "applies cleanly" and does not do what it says.

That is not hypothetical. It ate `void S_UnblockSound() {}` off the end of
usr/ports/tyrquake/patches/0001, whose header promised 7 new lines where the
body had 8. The build then failed one layer away with an undefined symbol --
which was the lucky outcome. The same mechanism applied to a patch that adds a
GUARD rather than a definition would remove the guard and leave something that
still compiles, still runs, and is no longer doing the check.

Every patch in this tree is hand-written or hand-edited, so the counts are
hand-maintained, so this is a real and recurring hazard rather than a
theoretical one.

SEVERITY MATTERS, and the first version of this tool got it wrong. A surplus of
CONTEXT lines is benign: context is matched, never applied, so a short count
only narrows the window patch checks. A surplus of ADDED lines is content loss.
Reporting both as "would DROP" is how a checker earns being ignored, and a
checker that is ignored is worth less than no checker at all -- so the two are
classified separately and only the second is an error.

Usage: tools/check-patch-hunks.py [dir ...]     (default: the tree's patch dirs)
"""
import re
import sys
import pathlib

HDR = re.compile(r'^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@')

# A hunk body ends at the next header, at a new file's diff, or at git's
# "-- " signature separator. Counting the signature as a removed line is what
# made the first draft of this tool report 20 false positives, all of them
# `old` off by exactly one -- a uniform off-by-one across unrelated files is
# the shape of a parser bug, not twenty independent authoring mistakes.
def _is_body_end(ln):
    return (ln.startswith('@@ ') or ln.startswith('--- ')
            or ln.startswith('+++ ') or ln.startswith('diff --git')
            or ln.rstrip() == '--')


def classify(lines, start, want_old, want_new):
    """Return (actual_old, actual_new, dropped_added), where dropped_added is
    the number of ADDED lines patch would not consume."""
    old = new = 0
    consumed_to = start
    satisfied = False
    i = start
    while i < len(lines):
        ln = lines[i]
        if _is_body_end(ln):
            break
        if ln.startswith('\\'):                 # "\ No newline at end of file"
            i += 1
            continue
        if ln.startswith('-'):
            old += 1
        elif ln.startswith('+'):
            new += 1
        elif ln.startswith(' ') or ln == '':
            old += 1
            new += 1
        else:
            break                                # trailing prose
        i += 1
        if not satisfied and old >= want_old and new >= want_new:
            satisfied = True
            consumed_to = i                      # exactly where patch stops
    if not satisfied:
        consumed_to = i
    # Count added lines in the tail patch would never reach.
    dropped_added = sum(1 for ln in lines[consumed_to:i] if ln.startswith('+'))
    return old, new, dropped_added


def main(argv):
    roots = argv[1:] or ['usr', 'third_party']
    repo = pathlib.Path(__file__).resolve().parent.parent
    errors = warnings = checked = 0
    for root in roots:
        base = (repo / root) if not pathlib.Path(root).is_absolute() else pathlib.Path(root)
        if not base.exists():
            continue
        for path in sorted(base.rglob('*.patch')):
            try:
                lines = path.read_text(errors='replace').splitlines()
            except OSError as e:
                print(f'{path}: UNREADABLE ({e})', file=sys.stderr)
                errors += 1
                continue
            for i, ln in enumerate(lines):
                m = HDR.match(ln)
                if not m:
                    continue
                want_old = int(m.group(2)) if m.group(2) is not None else 1
                want_new = int(m.group(4)) if m.group(4) is not None else 1
                checked += 1
                old, new, dropped = classify(lines, i + 1, want_old, want_new)
                if old == want_old and new == want_new:
                    continue
                rel = path.relative_to(repo) if repo in path.parents else path
                if dropped:
                    errors += 1
                    print(f'ERROR {rel}:{i+1}: {ln.strip()}')
                    print(f'      promises new={want_new}, body has {new} -- '
                          f'patch DISCARDS {dropped} ADDED line(s); '
                          f'the patch will apply cleanly and be incomplete')
                else:
                    warnings += 1
                    print(f'warn  {rel}:{i+1}: {ln.strip()}')
                    print(f'      counts off (old {want_old}/{old}, '
                          f'new {want_new}/{new}) but the surplus is CONTEXT '
                          f'only -- applies correctly, narrower match window')
    print(f'\n{checked} hunks checked: {errors} error(s), {warnings} warning(s)')
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
