---
id: fnd-ls4-r1-f1
type: fnd
title: "A deep cwd plus a long relative path is rejected though it would resolve"
round: adt-ls4-r1
severity: P3
status: fixed
surface: [sub-kernel-territory]
threatens: []
fixed-by: chg-2026-06-09-ls4-cwd
regression: "none (documented behaviour; every `out` write is capacity-guarded)"
created: 2026-08-01
---
## Prosecution

`cwd_lexical_resolve` builds the JOINED path into a fixed buffer and
returns `-1` if it will not fit. So a cwd that is itself near the limit,
plus a relative path, can exceed `SYS_OPEN_PATH_MAX` in the join even
when the same target named absolutely would resolve fine — the limit
applies to the intermediate, not the destination.

Memory-safe throughout: every append is guarded by
`olen + 1 + clen + 1 > outcap` before it writes.

## Disposition

Fixed as documentation — the combined-length bound recorded as a known
caveat rather than lifted. Lifting it means either a larger scratch on a
syscall path or resolving without materializing the join, and neither is
worth it for a bound no realistic path approaches.

Worth keeping visible because the failure is CONFUSING rather than
dangerous: the same file, named two ways, resolves one way and not the
other.
