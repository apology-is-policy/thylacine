---
id: fnd-b1c-r2-sa2
type: fnd
title: "cat's line mode held a whole line with no bound, so cat -A of an endless input grew until the pool ran out"
round: adt-b1c-r2
severity: P2
status: fixed
surface: [sub-coreutils-lib, sub-coreutils-filters]
threatens: []
fixed-by: chg-2026-09-24-b1c-round2-close
regression: "stream.cat_lines.a_line_is_shown_as_it_arrives, stream.cat_lines.every_transform_whatever_the_pieces (its unterminated last lines, which a line-holding CatLines never emits); coreutil-smoke 'cat -v streams a line with no end'; host sabotages S3 and S4 RED by name"
created: 2026-09-24
---
## Prosecution

cat's line mode (`-n`, `-b`, `-s`, `-E`, `-T`, `-v`, `-A`) read through
`BufReader::read_until`, which holds a whole line and has no bound. `cat -A
/dev/zero`, or `cat -v` of a disk image with a long run of bytes and no newline,
grew one line until the pool ran out; before B-1c the fixed heap stopped it at
4 MiB. Round 1's list of line readers missed it: it was the only `BufRead` line
reader in native code (395 files searched, `cat.rs` the control). With no victim
selection the fault ends whichever program touches a page next. Found by the
self-audit (P2); the round rated it P3 as its F6.

## Disposition

Fixed. `coreutils::stream::CatLines` applies cat's transforms a read at a time
and holds no line: what it carries is a line number, whether the last line was
blank and whether a line is open, across reads and across operands, so operands
are numbered as their concatenation, as GNU's are. It is host-tested against a
line-at-a-time reference at every piece size from one byte to seventeen and at
the read buffer's size, under all sixty-four flag combinations; "a line is shown
as it arrives" is the test any cat that holds a line fails. On the device `cat
/dev/zero | cat -v` must produce output as it reads. The failing case is not run
on the device on purpose: smoke children run as the SYSTEM principal, charged
but never refused, so an unbounded cat there would eat the reserve. The smoke's
`-n`, `-b`, `-s` and `-A` checks and `operands_are_numbered_as_their_concatenation`
are coverage: a cat that holds a line gives the same bytes for them (round 3's
F5).
