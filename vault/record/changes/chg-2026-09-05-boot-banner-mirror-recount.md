---
id: chg-2026-09-05-boot-banner-mirror-recount
type: chg
title: "abi-boot-banner recount: the prose caught up to the twenty-eight-mirror set"
date: 2026-09-05
arc: arc-vault
commits: []
touched:
  - abi-boot-banner
established: []
closed: []
opened: []
mirrors-checked: [tools/test.sh, tools/smp-multiboot.sh, tools/test-cross-reboot.sh, tools/test-fault.sh, tools/ci-idle-gate.sh, tools/np3-bench.sh, tools/verify-kaslr.sh, tools/warp/boot-probe.sh, tools/interactive/lib.exp, tools/interactive/dap-nora.exp, tools/interactive/flood-174.exp, tools/interactive/freeze-172.exp, tools/interactive/ls-gfx-font.exp, tools/warp/quarry-wedge.exp, tools/stall-watch.py, tools/check-arc-gates.sh, tools/display-modes/verify-console-mode.exp, tools/display-modes/verify-gpu-headless-1b.exp, tools/interactive/item10-ctrlc.exp, tools/interactive/ls-gfx-age.exp, tools/interactive/ls-gfx-restore.exp, tools/interactive/ls-gfx-session.exp, tools/interactive/ls-halcyon.exp, tools/interactive/pty-susp-pouch.exp, tools/interactive/r5f9-ash.exp, tools/test-smp-classify.sh, tools/testdata/smp-classify/real-pass-harness.log, tools/warp/composed-screen.exp]
depth: rich
created: 2026-09-05
---
The 2026-09 resync grew [[abi-boot-banner]]'s `mirrors` set from fifteen to
twenty-eight (it added thirteen `.exp`/`.sh` consumer gates that had accumulated
on `main` while the vault branch was behind), but only the frontmatter moved --
the note's PROSE still said "fourteen tools / fifteen mirrors" throughout, and
its delivery table still classified fifteen. This is the recount that brings the
prose current, and the formal record of the growth.

## What changed in the note

- The headline count (`## Why it is frozen`) now reads twenty-eight: twenty-seven
  mirrors match `Thylacine boot OK` / `EXTINCTION:` (one, `real-pass-harness.log`,
  a captured-log fixture) plus `stall-watch.py` on `kernel base:`; 30 tools/ files
  carry a literal (28 mirrors + 2 comment-only mentions).
- A new dated subsection ("The resync grew the set to twenty-eight") classifies
  the thirteen new gates by literal (5 match boot-OK, 10 match EXTINCTION, 0
  kernel-base) AND by delivery: eleven are programs that deliver (the ten
  interactive/display/warp `.exp` gates boot a real guest via `lib.exp`;
  `check-arc-gates.sh` reads real boot output), two do not (`test-smp-classify.sh`
  is the classifier's own "no boots" unit test and `real-pass-harness.log` is one
  of its fixtures) -- no-delivery BY DESIGN here, so co-update-only, not the
  nothing-invokes rot the 2026-08-18 census found.
- The delivery table: boot-OK matchers 8 -> 13, EXTINCTION 14 -> 24, kernel-base 1.
- The co-update-list argument ("omits fourteen that can") updated to twenty-seven.
- The dated 2026-08-18 findings (the main#245 census, the checker-bug) are KEPT as
  the historical fifteen-mirror record, with a pointer at the top marking which
  counts are historical vs current -- avoiding the both-readings anti-pattern the
  note itself warns about elsewhere.

## What is deferred (stated, not guessed)

Whether the `el1_sync_runaway` extinction-message body joins the pinned
message-body set (as `test-fault.sh`'s seven are) is left OPEN: its context
(yip 0026) is purged and the decision needs the #246 el1_sync_runaway test's
ground truth. Recorded in the note's Prosecution as owed, tied to
[[seam-extinction-line-unserialized]].

## The mirror-growth record

This chg carries `mirrors-checked` for all twenty-eight (each verified to match
a literal by grep). That is only cleanly possible because
[[chg-2026-09-05-r6-grandfather]] fixed R6 to measure a chg's `mirrors-checked`
against the mirror set as of the chg's own commit: before that fix, a
mirror-growing chg could not satisfy R6 without wedging the append-only history
of the chgs that checked the smaller set. This is the first mirror-growing chg
to land under the fixed rule -- the worked example the R6 fix existed to enable.
