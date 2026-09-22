---
id: fnd-shed-r2-f2
type: fnd
title: "the shed closure's seed obligation is recorded only on the shed's side, where the next resolver change will never look -- and TLC cannot see a seed missing from both rule and truth"
round: adt-shed-r2
severity: P2
status: fixed
surface: [sub-kernel-territory, sub-kernel-stalk]
threatens: [inv-i28]
fixed-by: chg-2026-09-21-mount-shed
regression: "none possible by test (a seed absent from rule AND walker is invisible by construction: TrueStart == {r} + buggy_no_union_seed.cfg -> 'No error', 657,024 states); the pin is the two-sided record -- AUDIT-TRIGGERS rows 79 / 99 / UM, the WHY comments at both stalk.c consult sites, sub-kernel-stalk"
created: 2026-09-21
---
## Prosecution

**File**: `docs/AUDIT-TRIGGERS.md` row 79 (its file list omits `kernel/stalk.c` and `kernel/spoor.c`); the stalk, symlink and union rows never mention the shed; no back-pointer at either consult site in `kernel/stalk.c`
**Invariant**: I-28 (through `ShedLosesNothing`)
**Prosecution**:
1. Round 1's P1 arose because UM-8c F5 added a base-time consult of `union_snap->point` and the later shed did not know.
2. The order is now reversed: the next union change edits `stalk.c`, which fires the stalk / union rows and NOT the shed's.
3. TLC cannot catch it: with both the walker and the truth forgetting the point, the no-seed buggy cfg reports no error.
**Suggested fix**: name `stalk.c` and `spoor.c` in the shed row; one sentence in the stalk and union rows; a WHY comment at both sites; one line in the stalk dossier.

## Disposition

Fixed on every surface named, and the rule stated once: a new base-time consult in the resolver is a new seed in the shed, and no spec can notice one missing from both sides.
