---
id: fnd-b1c-r2-f1
type: fnd
title: "grep's matches and cut's fields were collected per line: sixteen bytes an entry, up to two gigabytes for one line at LINE_MAX"
round: adt-b1c-r2
severity: P2
status: fixed
surface: [sub-coreutils-lib, sub-coreutils-filters, sub-coreutils-presenters]
threatens: []
fixed-by: chg-2026-09-24-b1c-round2-close
regression: "find.a_line_of_matches_costs_nothing_to_search, select.a_line_of_fields_costs_nothing_to_cut, counting.an_allocation_is_counted (the counter's control); host sabotages S1 and S2 RED by name"
created: 2026-09-24
---
## Prosecution

Round 1 bounded a line at `LINE_MAX`, 64 MiB, and recorded HT09.R4-F2's
input-driven half as closed. The bound was on the bytes a filter held. grep built
a `Vec` of match spans for every line it styled -- `-o`, and every line under
colour or at the Rich tier -- sixteen bytes a match, and a line holds as many
matches as bytes (`grep -o x` on a line of `x`): a gigabyte of spans for one line
at the bound. `cut -f` built a `Vec` of the line's fields, sixteen bytes each and
doubling as it grew: up to thirty-two times the line, two gigabytes, with the
direct block's copy doubling residency while it moved. Past free memory the fault
kill exits 1, which is grep's "no match".

## Disposition

Fixed. Nothing built from a line is collected. `coreutils::find` (grep's matcher,
moved into the library) hands each non-overlapping match to a callback, and
`coreutils::select` hands `cut` each selected field or run of bytes the same way;
both stop when the callback says so, which is how a failed write stops them. The
cost is measured, not argued: `coreutils::counting`, a test-only global allocator
with its own control, counts what each thread asks for, and a mebibyte line of
matches and one of delimiters are searched and cut with nothing allocated. Every
selection and match is checked against the collecting code's answer. The
register's closure of the input-driven half now names this fix. The smoke's
`grep -o every match of a line` and `cut -f fields of a line` show each shape's
output once; the collecting code gave the same bytes, so they are coverage, not
the regression (round 3's F5).
