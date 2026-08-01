---
id: fnd-66b-r1-f2
type: fnd
title: "A truncated mount line concatenated into the binds: line"
round: adt-66b-r1
severity: P3
status: fixed
surface: [sub-kernel-territory]
threatens: []
fixed-by: chg-2026-06-12-66b-mp-path
regression: "territory_mount.format_ns (a mid[20] cap yielding exactly line 1 and no binds; a tiny[8] cap yielding empty, not a partial 'mount ')"
created: 2026-08-01
---
## Prosecution

`territory_format_ns` appended field by field with an overflow-stop, so
a line that PARTIALLY fit left its prefix in the buffer — `"mount /sr"`
— and broke out of the loop. The `binds: N` line was then appended
unconditionally, producing `"mount /srbinds: 0\n"`: a record that parses
as neither a mount line nor a binds line.

Bounded and memory-safe (no overrun, no freed read) — but malformed for
the line-oriented reader `/proc/<pid>/ns` exists to serve. And the
`binds:` line's presence implies the list above it is COMPLETE, which
after a truncation it is not.

## Disposition

Fixed: each mount line renders ATOMICALLY — snapshot the offset, rewind
on any overflow to discard the partial, set a `truncated` flag — and the
`binds:` line is emitted only when nothing was truncated (with its own
rewind). The output is now always a sequence of whole lines.

The self-audit had noted the truncation as acceptable best-effort; the
formal round sharpened it by asking what the NEXT write does to a
partial line. "Best-effort" is a claim about completeness, not about
well-formedness — a truncated record may be short, but it must still
parse.
