---
id: fnd-ls4-r1-f3
type: fnd
title: "source_is_valid is a tautology"
round: adt-ls4-r1
severity: P3
status: documented
surface: [sub-kernel-territory]
threatens: []
regression: "none"
created: 2026-08-01
---
## Prosecution

`territory.c::source_is_valid(s)` null-checks and returns `true`. Its
own comment explains why it does not check the magic — `spoor_ref` does
that and extincts on mismatch, so duplicating it here would be
redundant. The function is therefore a null check wearing a
validity-check's name, and every call site reads as if a real validation
happened.

## Disposition

Documented, no action — pre-existing and out of the LS-4 scope that
found it, and the behaviour is CORRECT (the magic really is checked, one
frame later, with a better message).

Carried into [[sub-kernel-territory]]'s Caveats because the risk is
readability, not soundness: a future caller could reasonably assume
`source_is_valid` licenses skipping a check it never performed.
