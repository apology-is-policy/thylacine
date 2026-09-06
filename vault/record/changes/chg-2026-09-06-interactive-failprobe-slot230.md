---
id: chg-2026-09-06-interactive-failprobe-slot230
type: chg
title: "substrate-interactive de-stale: the failure-time state probe (the burned-retry decider), the #230 second-QMP-monitor per-slot isolation, and the #224 refinement of the in-tree refusal"
date: 2026-09-06
arc: arc-vault
commits: []
touched:
  - sub-substrate-interactive
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
[[sub-substrate-interactive]] (updated 2026-08-15) is thorough and mostly current;
`355ffa3e..HEAD` (+155 on `tools/test-interactive.sh` + `tools/interactive/lib.exp`)
added three things it did not carry, verified in the current source:

- **`fail-probe`** (11173762): a scenario's OWN account of the guest at the moment
  its assertion missed, run ONCE (self-clearing, no recursion through `lc_fail`)
  while the VM is still alive. Not a retry and not a tolerance -- it changes
  nothing on the passing path. Motivated by pty-4 burning a retry at "^Z stops the
  foreground sleep" (echo stopped at `sleep `, `Stopped` never came, guest ALIVE):
  only the guest's own state at that instant separates a real regression from a
  timing miss. The `vm-at-fail` discipline one layer in -- vm-at-fail says the
  guest was alive, the probe asks WHY.
- **#230 second-monitor per-slot isolation** (e680fdd5): the console screendump
  gate's second monitor (`build/qmp-gate.sock`, from substrate-gates' #230) was a
  fixed path that arrived AFTER the per-slot conversion, so at `JOBS=3` three VMs
  raced on it (bind-loser dies "File exists"; three attempt-1 INFRA flakes, each
  retried green -- the deterministic-collision-read-as-flake). Now
  `THYLACINE_QMP_SOCK2 = <slot>/qmp-gate.sock`, cleaned by `slot_release`. The
  sibling of the #230 change already folded into [[sub-substrate-gates]].
- **#224** (849d85fc): the in-tree refusal (dossier's #217 section) refined -- the
  tree-wide reaper SIGKILLs boots it does not own, same finding from the reaper's
  side, same answer (a named refusal over a silent mutual-corruption race). Both
  #217 and #224 are in the code; citation added.

Folded into Concurrency (#230 + #224) and a new fail-probe paragraph beside the
"evidence before the kill" section. `updated:` -> 2026-09-06. Stale backlog 37 -> 36.
