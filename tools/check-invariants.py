#!/usr/bin/env python3
"""Invariant-registry consistency gate.

CLAUDE.md instructs "Keep the ROW SET + spec column in sync with ARCH section 28"
and records that this drift already happened once (RW-10) and was repaired. The
repair fixed the instance and left nothing that could FAIL, so it drifted again:
CLAUDE.md sat four rows behind ARCH while I-45 -- enforced in code, prosecuted by
name, cited across ten files -- had no row in either. A rule that says "keep these
in sync" is safe-if-remembered. This is the check that makes it safe-by-default.

Two properties, both cheap:

  A. CLAUDE.md's I-NN row set == ARCHITECTURE.md section 28's row set.
     The condensed table is a MIRROR; a mirror missing rows is worse than no
     mirror, because CLAUDE.md is loaded into every session and is therefore an
     instance's DEFAULT belief about the invariant set.

  B. Every I-NN cited anywhere in docs/AUDIT-TRIGGERS.md resolves to a section-28
     row. The prosecutor prompt template says "auditing {scope} against the
     invariants listed in ARCHITECTURE.md section 28" -- so a trigger row naming
     an invariant that section 28 does not define hands a reviewer a dangling
     reference, and the round either skips the surface's named invariant or
     reconstructs it from the code under test.

  C. Rows appear in ascending numeric order in both tables. Added after the
     first version of this checker passed a table whose I-41 row -- whose ENTIRE
     PURPOSE is to explain the gap between I-40 and I-42 -- had been appended
     after I-44, where nobody scanning numerically would ever reach it. A/B was
     green throughout, because a set comparison has no opinion about order: the
     check was structurally incapable of covering the defect, and its green
     result made the misplacement HARDER to notice rather than easier. A
     registry is read in number order; that is the only order that matters.
"""
import re
import sys
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
ROW = re.compile(r"^\|\s*(I-\d+)\s*\|")
CITE = re.compile(r"\bI-(\d+)\b")


def rows(path):
    """The I-NN row set of the first invariant table in `path`."""
    found = []
    for line in (ROOT / path).read_text(encoding="utf-8").splitlines():
        m = ROW.match(line)
        if m:
            found.append(m.group(1))
    return found


def key(i):
    return int(i.split("-")[1])


def main():
    problems = []

    arch = rows("docs/ARCHITECTURE.md")
    claude = rows("CLAUDE.md")

    # Guard the guard: if either table reads as empty the checks below would
    # pass vacuously, which is the failure mode this file exists to prevent.
    if len(arch) < 20 or len(claude) < 20:
        print(f"FAIL: a table parsed as near-empty (arch={len(arch)} "
              f"claude={len(claude)}) -- the row regex no longer matches the "
              f"table format, so these checks would pass vacuously")
        return 1

    dup = {r for r in arch if arch.count(r) > 1} | {r for r in claude if claude.count(r) > 1}
    if dup:
        problems.append(f"duplicate rows: {sorted(dup, key=key)}")

    a, c = set(arch), set(claude)
    if a - c:
        problems.append(f"in ARCH section 28 but MISSING from CLAUDE.md: "
                        f"{sorted(a - c, key=key)}")
    if c - a:
        problems.append(f"in CLAUDE.md but MISSING from ARCH section 28: "
                        f"{sorted(c - a, key=key)}")

    for name, rs in (("ARCH section 28", arch), ("CLAUDE.md", claude)):
        nums = [key(r) for r in rs]
        if nums != sorted(nums):
            bad = [f"{rs[i]} after {rs[i-1]}"
                   for i in range(1, len(nums)) if nums[i] < nums[i - 1]]
            problems.append(f"{name} rows are OUT OF NUMERIC ORDER: "
                            f"{bad} -- a registry is read in number order, so a "
                            f"row filed elsewhere answers a question where it is "
                            f"not asked")

    trig = (ROOT / "docs/AUDIT-TRIGGERS.md").read_text(encoding="utf-8")
    cited = {f"I-{n}" for n in CITE.findall(trig)}
    dangling = sorted(cited - a, key=key)
    if dangling:
        problems.append(f"cited in AUDIT-TRIGGERS.md but NO section-28 row: "
                        f"{dangling}")

    if problems:
        print("FAIL: invariant registries disagree")
        for p in problems:
            print(f"  - {p}")
        return 1

    print(f"invariants OK: {len(arch)} rows, ARCH == CLAUDE.md, "
          f"{len(cited)} cited in AUDIT-TRIGGERS.md all resolve")
    return 0


if __name__ == "__main__":
    sys.exit(main())
