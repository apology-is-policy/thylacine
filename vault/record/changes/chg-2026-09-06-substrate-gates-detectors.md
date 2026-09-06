---
id: chg-2026-09-06-substrate-gates-detectors
type: chg
title: "substrate-gates de-stale: the second EXTERNAL-KILL arm (#222), the sourceable classifier + producer cross-check (#234/#212/#143), archive-not-delete (#223), the per-boot wall clock (#200), and the lean-shape / arc-gate propagation (#228/#229/#230/#232)"
date: 2026-09-06
arc: arc-vault
commits: []
touched:
  - sub-substrate-gates
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-06
---
[[sub-substrate-gates]] read `updated: 2026-08-16`, but eight numbered commits to
`tools/{smp-multiboot,test}.sh` (dated 2026-07-30 .. 2026-08-13, merged into the
vault lineage 2026-09-05) had not yet reached the vault tree when the dossier was
last edited (`6a275990`). The merge-topology hid them: `git log --since=2026-08-16`
finds NOTHING (their commit dates predate the dossier), while `6a275990..HEAD`
carries all eight -- so the dossier was genuinely behind. Ground-truthed by diffing
`6a275990..HEAD` on the two scripts (+333 lines), not by trusting the stale tool's
merge-date reading. Five real mechanism gaps closed:

- **#222 -- EXTERNAL-KILL now has TWO arms.** The dossier described only arm 1
  (QEMU's `terminating on signal N from pid M`, #88) and made a soundness argument
  that is now SUPERSEDED: it said the harness's uncatchable `kill -KILL` means QEMU
  "prints nothing", framing that as why the line cannot be ambiguous -- but that is
  exactly why arm 1 is BLIND to a real external SIGKILL, which therefore landed in
  OTHER (#200). Arm 2 reads the shell's `line N: PID Killed: 9` from the harness
  stream, GATED on `qemu_alive_at_teardown=0` (a fact only `test.sh` supplies, since
  bash emits the same notification for the harness's own teardown kill), sender NOT
  RECOVERABLE. Rewrote the classifier-table row + the whole soundness section.
- **#234/#212/#143 -- the classifier is a pure sourceable function.**
  `classify_boot`/`harness_result_token` + the `BASH_SOURCE != $0` return
  guard let `tools/test-smp-classify.sh` (added to `code:`, was UNOWNED) drive the
  REAL ladder over fixtures (#143: a re-declared copy proves only self-agreement);
  the token map is cross-checked so a renamed verdict on one side cannot silently
  void an arm (#234); `arc-gates` must precede `pass` (#212, post-banner failure
  after `==> PASS`).
- **#223 -- ARCHIVE, not delete.** Prior captures move to `archive/$LABEL-<ts>/`;
  delete-on-start made re-running a rare-failure label destroy its own evidence
  (cost the #200 sighting-2 harness log). Corrected in Data structures.
- **#200 -- the per-boot wall clock** rides every boot line (the sanitizer-effect
  vs exposure-time question on the SIGKILL asymmetry). New caveat.
- **#228/#229/#230/#232 -- the lean `--production` shape is built + gated in the
  loop** (nothing built it before, so it rotted silently); the warden went
  unconditional so G-4 applies to the lean image (only debug-probe stays skipped,
  keyed on the SHAPE); a second QMP monitor decouples the injector from the gate;
  and `check-arc-gates.sh` propagates the D-5/L-6c + clade gates into the exit
  status. New Mechanism paragraphs.

`updated:` -> 2026-09-06. `code:` gains `tools/test-smp-classify.sh`. Stale backlog
42 -> 41.
