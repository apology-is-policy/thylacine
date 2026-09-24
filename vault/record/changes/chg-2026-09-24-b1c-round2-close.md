---
id: chg-2026-09-24-b1c-round2-close
type: chg
title: "B-1c holotype round 2 close: nothing collected from a line, cat's transforms stream, every write can fail loudly (0 P0 / 0 P1 / 1 P2 / 8 P3, one P2 self-found)"
date: 2026-09-24
arc: arc-boosty
commits: ["96b51346"]
touched:
  - sub-libthyla-rs
  - sub-coreutils-lib
  - sub-coreutils-filters
  - sub-coreutils-presenters
established: []
closed: [fnd-b1c-r2-f1, fnd-b1c-r2-sa2]
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-24
---
Closes [[adt-b1c-r2]], extending [[chg-2026-09-24-b1c-round1-close]]. Round 1's
line bound held the bytes, not what the filters built from them:
[[fnd-b1c-r2-f1]] moved grep's matcher and cut's selection into the library as
`coreutils::find` and `coreutils::select`, which hand each match or field on as
they find it, and a test-only counting allocator shows a mebibyte line costs
them nothing. [[fnd-b1c-r2-sa2]] was cat's line mode holding a whole line; its
transforms are `stream::CatLines` now, a read at a time with no line held. The
P3s: `grep -c` still searches for its verdict after its reader has gone, as far
as a first match; `head` and `tail` write their banners through the failing
path, and "-" reads standard input as their headers always said; cat, head and
tail tell a failed read from a failed write and say "write error" with its
cause; hexdump, pelt and ls stop once their output has failed; ns's raw path
reports a failed write; uniq's first line grows by an exact reservation; `tail
-n 0` reads nothing; the reader-leaves harness bounds its reads and can no
longer pass a filter that ended by itself, and the re-run that reports a failed
check's stderr reads the tool's output instead of dropping its reader, which had
stopped the tool before the failure it was to explain; and grep's styling reads
the colour flags where it styles, so no caller can style a payload by
forgetting a check.
Owed to the operator: netd's conversation files typed as regular files, which
`grep -r` opens (F8); and nineteen one-shot tools that still swallow a write
error. Verified: host coreutils 36/36 with six sabotages RED by name (find and
select collecting, cat holding a line, cat restarting a line per read, the
counter counting nothing, uniq's first line reserved amortized); the device at
-smp 1 and -smp 4 (1667/1667; heap-probe ALL OK; coreutil-smoke, 93 checks, all
OK), and one device sabotage leg reverting part 2's fixes: exactly those eight
checks failed, each by its own mechanism.
