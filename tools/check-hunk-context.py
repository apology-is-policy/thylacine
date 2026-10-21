#!/usr/bin/env python3
"""tools/check-hunk-context.py <tree> <patch> -- refuse a hunk GNU patch places
only at the end of its file.

GNU patch reads a hunk with less context after its changes than before them as
the end of a file (a diff can only be short of trailing context there), so at
-F 0 it matches that hunk nowhere else: away from the end it fails, where BSD
patch applies it. A series maintained on macOS then breaks on the Linux
builders. This reads each hunk of <patch> against <tree>, the state the patch
applies to (-p1 paths), and exits 1 naming every hunk whose trailing context is
shorter than its leading context and whose old range does not end at the
file's last line. GNU patch 2.7.6 locate_hunk: suffix_fuzz = fuzz +
suffix_context - context, and a negative suffix_fuzz can only match the end of
the file.
"""
import os
import re
import sys

HUNK = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")


def hunks(lines):
    """Yield (path, hunk number, old start, old count, leading, trailing)."""
    path, n, i = None, 0, 0
    while i < len(lines):
        line = lines[i]
        if line.startswith("+++ "):
            name = line[4:].split("\t")[0]
            path = name.split("/", 1)[1] if "/" in name else name
            n = 0
            i += 1
            continue
        m = HUNK.match(line)
        if not m or path is None:
            i += 1
            continue
        n += 1
        ostart, ocount = int(m.group(1)), int(m.group(2) or 1)
        ncount = int(m.group(4) or 1)
        body, seen_o, seen_n, i = [], 0, 0, i + 1
        while i < len(lines) and (seen_o < ocount or seen_n < ncount):
            b = lines[i]
            i += 1
            if b.startswith("\\"):
                continue
            # A tab-led or empty line is context, as GNU patch reads it (pch.c:
            # "assume the space got eaten").
            tag = b[:1]
            if tag in (" ", "\t", ""):
                seen_o += 1
                seen_n += 1
                body.append(" ")
            elif tag == "-":
                seen_o += 1
                body.append("-")
            elif tag == "+":
                seen_n += 1
                body.append("+")
            else:
                break
        changes = [k for k, t in enumerate(body) if t != " "]
        if not changes:
            continue
        yield path, n, ostart, ocount, changes[0], len(body) - 1 - changes[-1]


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: check-hunk-context.py <tree> <patch>")
    tree, patch = sys.argv[1], sys.argv[2]
    lines = open(patch, encoding="utf-8", errors="surrogateescape").read().split("\n")
    bad = 0
    for path, n, ostart, ocount, lead, trail in hunks(lines):
        if trail >= lead or ostart == 0:
            continue
        target = os.path.join(tree, path)
        size = None
        if os.path.exists(target):
            with open(target, "rb") as f:
                data = f.read()
            size = data.count(b"\n") + (1 if data and not data.endswith(b"\n") else 0)
        if size is not None and ostart + ocount - 1 == size:
            continue
        print(f"{os.path.basename(patch)}: {path} hunk #{n} (@@ -{ostart},{ocount}) has "
              f"{trail} trailing context line(s) after {lead} leading, away from the "
              f"end of the file -- GNU patch matches it only at the end; add trailing context")
        bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
