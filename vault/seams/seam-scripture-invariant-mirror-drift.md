---
id: seam-scripture-invariant-mirror-drift
type: seam
title: "The always-loaded invariant mirror drifted from the authoritative table"
status: open
surface: inv-i32
opened-by: chg-2026-08-16-i32-scope-correction
tracker: "unfiled -- yip to main 2026-08-16"
created: 2026-08-16
updated: 2026-08-16
---
## Owed

Two separate problems in the section-28 invariant tables, both found while
correcting [[inv-i32]] and neither belonging to this vault to fix.

**One: a verified mirror drift.** The authoritative table in the architecture
document states I-32 on **two granularities** — the memory axes bound an address
space, the thread and child axes bound a process, and the process's budget field
is an *authorization* rather than the enforced cap. The condensed mirror in the
always-loaded operating-notes file still states the pre-extraction version: a
per-process floor over pages, threads and children alike.

The authoritative copy is right. The convenient copy is wrong.

**Two: a status whose own trigger has fired.** Both copies mark I-44 as
`RESERVED ... ENFORCED at L-4/L-5`. L-4 and L-5 have landed, and the arc-close
audit returned no findings above P2 and declared itself not dirty. The condition
the row names as its own trigger has been met and the row has not moved. Whether
to call it enforced is a declaration for the implementation track to make — the
observation here is only that the row's stated precondition is satisfied and
nobody flipped it.

## What closes it

For the drift: restate the mirror's I-32 row from the authoritative one.

For the status: decide I-44 deliberately — flip it, or record why it stays
reserved despite its trigger.

**Neither is a vault edit.** Both files are scripture on the implementation
branch.

## Risk while open

For I-32, the risk is inverted from ordinary staleness and that is the whole
point of recording it. **The stale copy is the one that loads into every session
automatically; the correct copy has to be opened on purpose.** So the wrong
version has near-total readership and the right version has almost none — which
is exactly backwards from a stale document nobody reads, and it is how this
vault's own note came to be wrong in the same direction. Two summaries agreeing
with each other rather than with the source.

For I-44, a row reading `RESERVED` tells an auditor the invariant is not yet
something to prosecute against. If the mechanism is in fact built and audited,
that label suppresses exactly the scrutiny it was created to schedule.

## Why the existing rule did not prevent it

The mirror carries an explicit instruction to stay in sync, and a note that the
drift has been repaired once before. It names what to keep in sync: **the row
set and the specification column.** Both of those are currently correct — the
row counts match exactly, and no phantom specification names remain.

**What drifted is the one thing the instruction does not name: the statement
text.** A diligent reader following it literally would check the count, check the
spec names, find both clean, and stop.

That is the third instance of one shape in a single sweep, after
[[chg-2026-08-16-hwcap-widths]]'s width rule and the category-versus-property
family: **a rule stated over an enumerated part protects that part and reads as
protecting the whole.** The instruction is not wrong; it is narrower than the
hazard, and its narrowness is invisible to someone obeying it.

Note also what cannot be automated here. Comparing the two tables mechanically
reports 44 of 45 rows as differing — which is the *design*, since one is a
deliberate condensation of the other. A detector on textual identity measures
disagreement between two things meant to disagree, and would bury the one real
row in forty-three false ones. The single verified drift was found by reading.
