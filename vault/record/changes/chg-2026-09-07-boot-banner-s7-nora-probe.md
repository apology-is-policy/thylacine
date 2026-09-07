---
id: chg-2026-09-07-boot-banner-s7-nora-probe
type: chg
title: "abi-boot-banner: declare the s7-nora-probe.exp gate (mirrors 28 -> 29; EXTINCTION deliverers 24 -> 25)"
date: 2026-09-07
arc: arc-vault
commits: ["PENDING"]
touched:
  - abi-boot-banner
established: []
closed: []
opened: []
mirrors-checked: [tools/test.sh, tools/smp-multiboot.sh, tools/test-cross-reboot.sh, tools/test-fault.sh, tools/ci-idle-gate.sh, tools/np3-bench.sh, tools/verify-kaslr.sh, tools/warp/boot-probe.sh, tools/interactive/lib.exp, tools/interactive/dap-nora.exp, tools/interactive/flood-174.exp, tools/interactive/freeze-172.exp, tools/interactive/ls-gfx-font.exp, tools/warp/quarry-wedge.exp, tools/stall-watch.py, tools/check-arc-gates.sh, tools/display-modes/verify-console-mode.exp, tools/display-modes/verify-gpu-headless-1b.exp, tools/interactive/item10-ctrlc.exp, tools/interactive/ls-gfx-age.exp, tools/interactive/ls-gfx-restore.exp, tools/interactive/ls-gfx-session.exp, tools/interactive/ls-halcyon.exp, tools/interactive/pty-susp-pouch.exp, tools/interactive/r5f9-ash.exp, tools/test-smp-classify.sh, tools/testdata/smp-classify/real-pass-harness.log, tools/warp/composed-screen.exp, tools/interactive/s7-nora-probe.exp]
depth: rich
created: 2026-09-07
---
The s7 F3 chunk (main @70f91be3) added a new interactive gate,
`tools/interactive/s7-nora-probe.exp`, that boots a real guest through `lib.exp`
and fails on `EXTINCTION:` in each of five session phases. It matches the
`EXTINCTION:` ABI literal (5 arms) and neither of the other two, so it is an
undeclared consumer of [[abi-boot-banner]] -- the thylacine pre-commit vault-lint
blocked committing it on the code track until it was declared here. This is that
declaration plus the consequent recount.

## Why the file and the declaration land in one vault commit

The lint's UNMATCHED-mirror arm checks each declared mirror against the tree's
tracked-file hit set (`git ls-files` under the `literal-scan` roots). Declaring
`s7-nora-probe.exp` as a mirror while the file is absent-and-untracked in this
worktree would itself fail the vault lint (a mirror naming nothing). The file was
UNTRACKED in the code worktree and ABSENT here, so the deadlock only breaks by
committing the `.exp` (verbatim, verified byte-identical to the code worktree's
copy) and its declaration together. Once this pushes to `main`, the code track's
pre-commit lint passes and the previously-untracked `.exp` is already tracked.

## What changed in the note

- `mirrors` grew twenty-eight -> twenty-nine (the one new gate).
- The `## Why it is frozen` current-total prose: the set is now twenty-nine
  (twenty-eight match `Thylacine boot OK` / `EXTINCTION:`, one, `real-pass-harness.log`,
  a captured-log fixture; plus `stall-watch.py` on `kernel base:`); 31 tools/ files
  carry a literal (29 mirrors + 2 comment-only mentions). The 2026-09 resync's
  own "grew to twenty-eight" framing is KEPT as the dated event -- s7-nora-probe
  is a post-resync add, not one of that subsection's thirteen.
- A new dated subsection ("s7-nora-probe: one EXTINCTION deliverer added") records
  the single gate by literal (EXTINCTION only) and by class (a program that
  delivers -- it reads real boot output), mirroring the resync subsection's style.
- The delivery table: `EXTINCTION:` matchers 24 -> 25 (boot-OK 13 and kernel-base 1
  unchanged, as the gate matches neither); the "25 of the 29 match `EXTINCTION:`"
  restatement follows.
- The co-update-seam count ("omits ... that can break") and the Prosecution count
  updated twenty-seven -> twenty-eight, with the seam's dated progression extended
  (fourteen at writing, twenty-seven at the resync, twenty-eight since).
- The dated historical records (the 2026-08-18 main#245 fifteen-mirror census, the
  resync-to-twenty-eight subsection, `chg-2026-09-05`'s mirrors-checked-for-28) are
  untouched, per the note's own "which counts are historical vs current" rule.

## The mirror-growth record

This chg carries `mirrors-checked` for all twenty-nine (each verified to match a
literal by grep; the new `s7-nora-probe.exp` matches `EXTINCTION:` 5x). As with
[[chg-2026-09-05-boot-banner-mirror-recount]], carrying the full grown set is only
cleanly possible under the R6 grandfather fix
([[chg-2026-09-05-r6-grandfather]]), which measures a chg's `mirrors-checked`
against the mirror set as of the chg's own commit.
