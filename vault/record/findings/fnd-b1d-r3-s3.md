---
id: fnd-b1d-r3-s3
type: fnd
title: "CoveredIsItsPoint had no buggy configuration: the one covered-member invariant nothing showed could fail"
round: adt-b1d-r3
severity: P3
status: fixed
surface: [spec-territory]
threatens: []
fixed-by: chg-2026-09-25-b1d-round3-close
regression: "specs/territory_buggy_covered_takes_flags.cfg (CoveredIsItsPoint violated)"
created: 2026-09-25
---
## Prosecution

B-1d-u added seven covered-member invariants to `specs/territory.tla`
(UnionHasCovered, CoveredOnlyInUnion, CoveredIsItsPoint, CoveredPlacement,
NoOrphanCovered, NoSelfMount, NoCoveredFile) and six buggy configurations.
Paired up, the six fail six of the seven; CoveredIsItsPoint failed in none. It
held in every run, and no run showed it could fail: its flag clause (a covered
member is never MBEFORE or MCREATE) was true by construction, because
`CovMember` wrote both flags FALSE. A check nothing shows failing proves
nothing, and the UM arc's rule is one buggy configuration per union invariant.
The main session found it while writing the SPEC-TO-CODE rows for the round-3
close, after its own landing draft had claimed every new invariant had one.

The kernel was covered at runtime: `territory_mount.union_keeps_covered` and
`covered_noexec_scoped` assert the covered entry is the point itself with
MCOVERED alone (`m1 == mb && f1 == MCOVERED`), so a kernel that copied the
mount's flags onto the covered entry fails both.

## Disposition

Fixed in the spec: `BUGGY_COVERED_TAKES_FLAGS` gives the covered member the
triggering mount's MBEFORE and MCREATE, the kernel analogue of installing it
with `flags | MCOVERED` (a fresh MAFTER|MCREATE union would then take its first
create in the covered directory). `territory_buggy_covered_takes_flags.cfg`
violates CoveredIsItsPoint. With the flag FALSE the covered member is the same
record as before, so the other configurations' state graphs are unchanged; the
whole matrix re-ran on the final spec anyway. The invariant's other two clauses
(at most one covered member; it is the point's own directory) still hold by
construction in the model: `CovMember` is the only builder of a covered member,
it names `Covered[pt]`, and the clean model calls it only at a fresh point.
`union_keeps_covered` pins both at runtime (the point itself, exactly two
members after the first mount), and `no_covered_unless_fresh` pins that a
member joining a formed union adds none.
