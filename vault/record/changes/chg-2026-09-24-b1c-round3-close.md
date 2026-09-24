---
id: chg-2026-09-24-b1c-round3-close
type: chg
title: "B-1c holotype round 3 close: a bounded capture, a run's line as it begins, tail +N (0 P0 / 0 P1 / 1 P2 / 9 P3)"
date: 2026-09-24
arc: arc-boosty
commits: ["*(pending)*"]
touched:
  - sub-coreutils-lib
  - sub-coreutils-filters
  - sub-coreutils-presenters
established: []
closed: [fnd-b1c-r3-f1]
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-24
---
Closes [[adt-b1c-r3]], extending [[chg-2026-09-24-b1c-round2-close]]. The smoke
could hang the boot where a check should have failed: [[fnd-b1c-r3-f1]] replaced
its reap-first capture with `converse`, which feeds a tool's input and reads its
outputs as they come under one poll, within the check's bound, and kills a tool
still running there before its pipes drop. The P3s: `uniq` with no `-c`, `-d` or
`-u` prints a run's line as the run begins, as GNU's does (`stream::firsts`), so
`yes | uniq` shows its line at once and a read error no longer loses the open
run; `tail -n +N` and `-c +N` count from the start, as POSIX has them
(`stream::Skip`, holding nothing), and an overflowing count is refused; the
docs state grep's verdict search with its cost and its `-q` rule, name `tee` and
the fifteen one-shot tools as the #54 contract's exceptions, and count what each
filter holds; two record notes keep only regressions that fail without their
fix; and the smoke reports a tool that ended by itself with its code, reads
every tool's stderr in the one run, and checks `grep -c`'s every-operand count
with its reader live. `cat -E`'s CRLF rendering is owed as a parity item.
The main session's C3-1 corrected seventeen files that still described the
fixed heap in the present tense. Verified: host coreutils 42/42 with four
sabotages RED by name (a run's first line held to its end, the skip skipping
nothing or counting from zero, a stopped first line read on); the device at -smp
1 and -smp 4 (1667/1667; heap-probe ALL OK; coreutil-smoke, 101 checks, all OK);
the reap-first capture restored hangs the boot at the first capture check, and a
capture that drops a cut-off tool's pipes and reaps it for its own code, with
tail and uniq back to their round-2 code, fails exactly six of the eight new
checks, each by name (the seventh is the capture check the first sabotage hangs
at; the eighth, `grep -c`'s count of every operand, is a control).
