---
id: adt-b1c-r3
type: adt
title: "B-1c (the native heap) round 3: the smoke's capture could hang the boot, and the round-2 fixes' docs overclaimed"
date: 2026-09-24
scope: [sub-libthyla-rs, sub-coreutils-lib, sub-coreutils-filters, sub-coreutils-presenters]
reviewer: opus
model-start: "claude-opus-5-5"
model-end: "claude-opus-5-5"
verdict: dirty
counts: {p0: 0, p1: 0, p2: 1, p3: 9}
findings: [fnd-b1c-r3-f1]
round-of: chg-2026-09-24-b1c-round3-close
prior-round: adt-b1c-r2
created: 2026-09-24
---
## Scope

Branch `b1c-native-heap` at b409f80b (WIP 10): round 2's fixes, `git diff
82b5f0d7 b409f80b` -- `coreutils::find` and `coreutils::select` (grep's matcher
and cut's selection, moved into the library), the test-only `counting`
allocator, `stream::CatLines`, grep's search for its verdict after its reader
has gone, #54's stops in hexdump, pelt, ls and ns, head's and tail's banners and
"-", the reworked smoke harness, and the round-2 close's docs. The round ran on
Opus 5.5 at max effort, the fallback tier, told that context independence was
what it brought and to re-derive each claim from the code; it was also asked to
decide whether grep's verdict search is sound.

## Convergence

0 P0 / 0 P1 / 1 P2 / 9 P3; model-start equals model-end. The main session's
self-audit, running beside it, found C3-1: present-tense claims of the fixed heap
in seventeen files outside the round's scope, which B-1c had made false. F1
([[fnd-b1c-r3-f1]]) is the finding of the round: the smoke's `run_tool` fed a
tool its input, reaped it with no bound and only then read its output, so an
output past a pipe's 4 KiB, or a tool that never ended, hung the boot where a
check should have failed, and round 2's own `grep -o` check had filled 73% of
that budget. Its fix is a new capture, `converse`, through which every check now
runs, so the close is dirty by the shape of the fix and round 4 follows. The
round judged grep's verdict search sound: it happens only for `-c`, stops at the
first selected line, and reads a prefix of what the command reads with its
reader; but the docs did not state its cost and called its answer the whole
search's verdict where it is `-q`'s (F2). The other P3s: the filters dossier's
#54 contract was false for `tee` and the one-shot tools (F3); "uniq drops the
run still open, as GNU's does" was false for GNU's plain `uniq`, which prints a
line as its run begins (F4); two record notes named regressions that pass
without their fix (F5); ARCH 6.5's as-built filter sentence miscounted what the
filters hold (F6); `tail -n +N` was read as the last N lines (F7, pre-existing);
`cat -E` shows a CRLF as `\r$` where GNU 9.2 shows `^M$` (F8, pre-existing); a
CatLines comment undercounted its worst case (F9); and three harness
diagnostics lost what they were to report (F10). All are fixed before landing
except F8, closed as a parity item: `-E` is not POSIX, the output is GNU's
before 9.2, and matching the new rule needs state held across pieces and
operands in a machine audited as holding nothing. Verified sound by the round:
`find` against the removed span search, `select` against the old cut, the
counting allocator's assertions, CatLines against GNU's state machine for every
flag across pieces and operands, the read/write split, `/dev/full`, the banners,
grep's stops and statuses, and that every harness helper reaps. The verbatim
report and dispositions are the repo's untracked
`memory/audit_b1c_closed_list.md`.
