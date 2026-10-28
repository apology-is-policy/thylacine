#!/usr/bin/env python3
"""Audit-trigger row-set gate.

A session finds its audit row in docs/AUDIT-TRIGGERS.md by the row's FIRST
CELL: it greps the surface or the path, reads that one row and prosecutes its
addenda. Merges broke that twice, and nothing failed either time (OPEN-BUGS
2026-10-28T02:20Z). A merge resolution dropped the VMO / BURROW row while both
of its parents still carried it, so burrow.c changes on that branch had no row
to find. Four surfaces carried two divergent copies, each with addenda the
other lacked, so which prosecution a reader got depended on which line grep
printed first.

  A. No first cell occurs twice in the working tree's tables.

  B. Each unpushed merge (reachable from HEAD, not from any origin ref) keeps
     what its parents hold, row by row against their merge bases:
     - LOST: a row every parent carries, or one some parent added or edited,
       is absent from the merge;
     - ONE SIDE: a row one parent changed is, in the merge, another parent's
       copy, so the first parent's change was dropped whole (judged only for
       a row every base carries under the same first cell: a retitled row has
       no anchor to tell an older copy from a lost addendum);
     - COPIED: the merge carries two rows under one first cell where no
       parent did.
     A finding the tree being built has already repaired (the row is back;
     the row has changed since the merge) no longer fails. A deliberate
     resolution says so in the message of the merge or of any later unpushed
     commit, with a trailer naming a prefix of the first cell:
     `Audit-row-dropped: <prefix>` excuses LOST, `Audit-row-one-side:
     <prefix>` excuses ONE SIDE.

--merge REV reports B on one commit, unfiltered, for reading history.
--self-test plants each fault and its control.
"""

import re
import subprocess
import sys
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
TABLE = "docs/AUDIT-TRIGGERS.md"
CELL = re.compile(r"(?<!\\)\|")
SEPARATOR = re.compile(r"^\|[\s:|-]+$")
TRAILER = re.compile(r"^Audit-row-(dropped|one-side):\s*(.+?)\s*$", re.M)


def rows(text):
    """First cell -> the tuple of every row line carrying it, header rows excluded."""
    lines = text.split("\n")
    out = {}
    for i, line in enumerate(lines):
        if not line.startswith("|") or SEPARATOR.match(line):
            continue
        if i + 1 < len(lines) and SEPARATOR.match(lines[i + 1]):
            continue
        cells = CELL.split(line)
        if len(cells) < 3:
            continue
        out.setdefault(cells[1].strip(), []).append(line)
    return {k: tuple(v) for k, v in out.items()}


def duplicates(table):
    return sorted(k for k, v in table.items() if len(v) > 1)


def merge_findings(bases, parents, result):
    """B's three checks over parsed tables; a list of (kind, cell)."""
    found = []
    cells = set(result)
    for t in bases + parents:
        cells |= set(t)
    for c in sorted(cells):
        bs = {b.get(c) for b in bases}
        ps = [p.get(c) for p in parents]
        r = result.get(c)
        if r is None:
            if all(p is not None for p in ps) or any(p is not None and p not in bs for p in ps):
                found.append(("LOST", c))
            continue
        for i, pi in enumerate(ps):
            if None in bs or pi in bs:
                continue
            if any(j != i and pj != pi and r == pj for j, pj in enumerate(ps)):
                found.append(("ONE SIDE", c))
                break
        if len(r) > 1 and all(p is None or len(p) < 2 for p in ps):
            found.append(("COPIED", c))
    return found


def git(*args):
    return subprocess.run(["git", "-C", str(ROOT), *args], capture_output=True,
                          text=True, check=True).stdout


def table_at(rev):
    try:
        return rows(git("show", f"{rev}:{TABLE}"))
    except subprocess.CalledProcessError:
        return {}


def trailers(messages):
    excuse = {"dropped": [], "one-side": []}
    for kind, prefix in TRAILER.findall(messages):
        excuse[kind].append(prefix)
    return excuse


def check_merge(rev):
    """B on one merge commit: (findings, result table)."""
    parents = git("rev-list", "--parents", "-n", "1", rev).split()[1:]
    if len(parents) == 2:
        bases = git("merge-base", "--all", *parents).split()
    else:
        bases = git("merge-base", "--octopus", *parents).split()
    result = table_at(rev)
    found = merge_findings([table_at(b) for b in bases],
                           [table_at(p) for p in parents], result)
    return found, result


def excused(kind, cell, excuse):
    key = {"LOST": "dropped", "ONE SIDE": "one-side"}.get(kind)
    return key is not None and any(cell.startswith(p) for p in excuse[key])


def unrepaired(found, result, tree, excuse):
    """The findings the tree being built still carries and no trailer excuses."""
    out = []
    for kind, cell in found:
        if excused(kind, cell, excuse) or kind == "COPIED":
            continue                          # COPIED that survives is A's
        if kind == "LOST" and cell in tree:
            continue
        if kind == "ONE SIDE" and tree.get(cell) != result.get(cell):
            continue
        out.append((kind, cell))
    return out


def self_test():
    def t(*lines):
        return rows("\n".join(["| Surface | Files |", "|---|---|", *lines]))
    a, b2, c = "| A | x |", "| B | y |", "| C | z |"
    a1, a2 = "| A | x (+1) |", "| A | x (+2) |"
    cases = [
        ("clean merge", t(a, b2), [t(a1, b2), t(a, b2, c)], t(a1, b2, c), []),
        ("both added, one kept", t(b2), [t(a1, b2), t(a2, b2)], t(a1, b2), []),
        ("criss-cross bases", [t(a), t(a1)], [t(a1), t(a2)], t(a2), []),
        ("union of two addenda", t(a), [t(a1), t(a2)], t("| A | x (+1) (+2) |"), []),
        ("deleted by one side", t(a, b2), [t(a), t(a, b2)], t(a), []),
        ("row both carry, lost", t(a, b2), [t(a, b2), t(a, b2)], t(a), [("LOST", "B")]),
        ("row one side added, lost", t(a), [t(a), t(a, c)], t(a), [("LOST", "C")]),
        ("edit vs delete, lost", t(a, b2), [t(a), t(a, "| B | y (+1) |")], t(a), [("LOST", "B")]),
        ("one side's addendum dropped", t(a), [t(a1), t(a2)], t(a1), [("ONE SIDE", "A")]),
        ("an edit reverted", t(a), [t(a), t(a1)], t(a), [("ONE SIDE", "A")]),
        ("two copies made", t(a), [t(a1), t(a2)], t(a1, a2), [("COPIED", "A")]),
    ]
    bad = 0
    for name, base, parents, result, want in cases:
        got = merge_findings(base if isinstance(base, list) else [base], parents, result)
        if got != want:
            print(f"  self-test {name}: want {want}, got {got}")
            bad += 1
    if duplicates(t(a, b2)) != [] or duplicates(t(a, a1)) != ["A"]:
        print("  self-test duplicates: wrong")
        bad += 1
    if t(a1).get("A") != (a1,) or "Surface" in t(a):
        print("  self-test parse: wrong")
        bad += 1
    none = {"dropped": [], "one-side": []}
    found = [("LOST", "B"), ("ONE SIDE", "A"), ("COPIED", "C")]
    filters = [
        ("nothing repaired", t(a1), t(a1), none, [("LOST", "B"), ("ONE SIDE", "A")]),
        ("row restored, row edited since", t(a1), t(a2, b2), none, []),
        ("trailers", t(a1), t(a1), {"dropped": ["B"], "one-side": ["A"]}, []),
        ("a trailer for another row", t(a1), t(a1), {"dropped": ["C"], "one-side": []},
         [("LOST", "B"), ("ONE SIDE", "A")]),
    ]
    for name, result, tree, excuse, want in filters:
        got = unrepaired(found, result, tree, excuse)
        if got != want:
            print(f"  self-test filter {name}: want {want}, got {got}")
            bad += 1
    total = len(cases) + 2 + len(filters)
    print(f"self-test: {total - bad}/{total}")
    return 1 if bad else 0


def main(argv):
    if argv[:1] == ["--self-test"]:
        return self_test()
    if argv[:1] == ["--merge"] and len(argv) == 2:
        found, _ = check_merge(argv[1])
        excuse = trailers(git("log", "-1", "--format=%B", argv[1]))
        for kind, cell in found:
            tag = " (excused by trailer)" if excused(kind, cell, excuse) else ""
            print(f"{kind}: {cell[:100]}{tag}")
        return 1 if any(not excused(k, c, excuse) for k, c in found) else 0

    problems = []
    tree = rows((ROOT / TABLE).read_text(encoding="utf-8"))
    # Guard the guard: a table that parses as near-empty would pass both
    # halves vacuously.
    if len(tree) < 100:
        print(f"FAIL: {TABLE} parsed as {len(tree)} rows -- the row format no "
              f"longer matches, so these checks would pass vacuously")
        return 1
    for c in duplicates(tree):
        problems.append(f"two rows carry the first cell '{c[:100]}'")

    try:
        origin = git("for-each-ref", "--format=%(refname)", "refs/remotes/origin").split()
        if origin:
            merges = git("rev-list", "--merges", "HEAD", "--not", *origin).split()
            excuse = trailers(git("log", "--format=%B", "HEAD", "--not", *origin))
            scope = f"{len(merges)} unpushed merge(s)"
        else:
            merges = git("rev-list", "--merges", "--no-walk", "HEAD").split()
            excuse = trailers(git("log", "-1", "--format=%B", "HEAD"))
            scope = f"no origin refs, so HEAD only ({len(merges)} merge)"
    except (subprocess.CalledProcessError, FileNotFoundError):
        merges, scope = [], "no git history, so the merge half did not run"

    for m in merges:
        found, result = check_merge(m)
        for kind, cell in unrepaired(found, result, tree, excuse):
            problems.append(f"merge {m[:9]} {kind}: '{cell[:100]}' -- still so "
                            f"in this tree; restore it, or name it in an "
                            f"Audit-row-* trailer if the resolution meant it")

    if problems:
        print("FAIL: the audit-trigger row set is damaged")
        for p in problems:
            print(f"  - {p}")
        return 1
    print(f"audit rows OK: {len(tree)} first cells, none twice; {scope} checked")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
