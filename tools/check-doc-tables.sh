#!/usr/bin/env bash
# Check Markdown tables for the two defects that make text INVISIBLE when
# rendered, while reading perfectly fine in an editor -- which is how they get
# written and how they survive.
#
#   RAGGED  -- a row with more cells than its header. GFM does not just drop the
#              excess: the extra pipe SHIFTS every later cell left, so the last
#              column displays a fragment of the previous one and the row's real
#              tail is dropped entirely. In a section 25.4 row that means the
#              "Why" column shows a piece of the file list and the prosecution
#              list -- the thing an auditor is sent to read -- is gone.
#              Cause: an unescaped `|` in the row text. Backticks do NOT escape
#              it; `\|` does, and renders as a bare `|` inside a code span too.
#
#   SEVERED -- a run of `|`-rows with no delimiter row above it, because a blank
#              line ended the table. Those rows are not table rows at all; they
#              render as paragraphs of literal pipe-and-backtick soup.
#
# The two are independent, and a ragged-row checker CANNOT see a severed table:
# it re-derives the header from the block it is checking, so an orphan block is
# measured against itself and always agrees.
#
# Usage:
#   tools/check-doc-tables.sh              # the scripture a reviewer reads
#   tools/check-doc-tables.sh --all        # every tracked .md
#   tools/check-doc-tables.sh FILE...      # named files
#
# Exits nonzero if anything is reported.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 2

# mapfile is bash 4+; macOS ships 3.2, where it is silently "command not found"
# and the array is then unbound. Read the list portably instead.
FILES=()
case "${1:-}" in
  --all) while IFS= read -r f; do FILES+=("$f"); done < <(git ls-files '*.md') ;;
  "")    FILES=(docs/ARCHITECTURE.md CLAUDE.md) ;;
  *)     FILES=("$@") ;;
esac
if [ ${#FILES[@]} -eq 0 ]; then echo "no files to check" >&2; exit 2; fi

python3 - "${FILES[@]}" <<'PY'
import re, sys

def check(path):
    try:
        lines = open(path).read().split('\n')
    except OSError as e:
        print(f"{path}: {e}"); return 1

    # A '|' line inside a fenced code block is not a table row.
    fence, live = False, []
    for l in lines:
        if l.lstrip().startswith('```'):
            fence = not fence
            live.append(False)
            continue
        live.append(not fence)

    found = 0

    # SEVERED: a '|'-block whose second line is not a delimiter row.
    i = 0
    while i < len(lines):
        if live[i] and lines[i].strip().startswith('|'):
            j = i
            while j < len(lines) and live[j] and lines[j].strip().startswith('|'):
                j += 1
            block = lines[i:j]
            if not (len(block) > 1 and re.match(r'^\|[\s:|-]+\|$', block[1].strip())):
                print(f"{path}:{i+1}: SEVERED table -- {j-i} row(s) with no header/delimiter above")
                print(f"    {lines[i][:100]}")
                found += 1
            i = j
        else:
            i += 1

    # RAGGED: a row whose cell count differs from ITS OWN table's header.
    # Compare against the header, never a global width -- this file has tables
    # of several widths, and a fixed assumption reports dozens of false hits.
    hdr = None
    for i, l in enumerate(lines):
        s = l.strip()
        if not (live[i] and s.startswith('|')):
            hdr = None
            continue
        # count only UNESCAPED pipes: `\|` is content, not a separator
        n = len([m.start() for m in re.finditer(r'(?<!\\)\|', l)]) - 1
        if hdr is None:
            hdr = n
            continue
        if re.match(r'^\|[\s:|-]+\|$', s):
            continue
        if n != hdr:
            what = "more" if n > hdr else "fewer"
            print(f"{path}:{i+1}: RAGGED row -- {n} cells, {what} than its header's {hdr}")
            found += 1
    return found

total = sum(check(p) for p in sys.argv[1:])
print(f"\n{len(sys.argv)-1} file(s) checked, {total} finding(s)")
sys.exit(1 if total else 0)
PY
