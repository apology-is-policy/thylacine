---
id: fnd-b1c-r1-f1
type: fnd
title: "A line with no end grew the filters' carry until the pool ran out, and the fault kill's exit 1 read as grep's \"no match\""
round: adt-b1c-r1
severity: P1
status: fixed
surface: [sub-coreutils-lib, sub-coreutils-filters, sub-coreutils-presenters]
threatens: []
fixed-by: chg-2026-09-24-b1c-round1-close
regression: "stream.a_line_past_the_bound_is_refused_after_the_lines_before_it, stream.a_held_line_never_grows_its_buffer_past_the_bound, stream.the_tails_open_line_is_bounded_and_a_long_ones_room_returns; coreutil-smoke 'grep a line past LINE_MAX' and 'tail a line past LINE_MAX'; the bound's host sabotages RED by name"
created: 2026-09-24
---
## Prosecution

`stream::lines` carried the start of a line a read cut short in a buffer that
grew for as long as no newline came, and `Tail` held its open line the same way.
B-1c had removed `slurp`'s 2 MiB cap with the fixed heap it was sized to, so
nothing bounded a line. `grep x /dev/zero` -- or `grep -r` walking into `/dev` and
meeting `zero`, `full` or `random` -- grew one line until the pool refused a page.
A lazy heap's refusal is a fault kill, v1.0 exit status 1, which is grep's "no
match"; with no victim selection the fault can land on another program instead.
Main before B-1c stopped at the cap with exit 2. The class is HT09.R4-F2's,
reopened by the streaming that closed it.

## Disposition

Fixed. `stream::LINE_MAX` is 64 MiB (POSIX lets a text utility refuse a line
longer than its LINE_MAX). `hold` grows the carried line by a fallible
reservation that never passes the bound and refuses past it with
`Error::TooLong`; a line that arrives whole in one read is checked too, and
`Tail` counts its open line as it scans. grep exits 2, and `cut`, `uniq` and
`tail` 1, each saying "a line longer than 64 MiB". `grep -r` skips a character
device it finds (GNU's rule; the only device type the kernel reports) and reads
everything else. A first cut skipped whatever `is_file()` denied, which on dev9p
(Stratum, where no file reports `S_IFREG`) would have skipped every file. The
pressure-driven half of R4-F2 -- another program exhausting the pool, so grep
faults on any page -- stays open for the operator (the exit-status ABI, or a
commit check).
