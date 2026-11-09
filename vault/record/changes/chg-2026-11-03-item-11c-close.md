---
id: chg-2026-11-03-item-11c-close
type: chg
title: "Item 11c close: rounds 1 to 3, the sweep, three leases"
date: 2026-11-03
arc: arc-phase6-pouch
commits: ["a2930f8bf", "d3683153d", "d0afb5937"]
touched:
  - sub-kernel-notes
  - sub-pouch-signal
established: []
closed: [seam-queue-note-fatal-after-post-wakes-nobody, seam-pouch-sigtstp-ignore]
opened: [seam-self-managing-handler-no-restart]
mirrors-checked: []
depth: skeletal
created: 2026-11-03
---
The close of [[chg-2026-11-03-item-11c]], whose record fields were fixed when it was committed. Audit rounds 1 to 3 (Fable 5.1, start == end, each on a frozen copy): 0/0/1/4, 0/0/1/2, 0/0/0/3. Runs on a2930f8bf: the kernel and guest sabotage sweep; lease 2's Pouch and Rust mutations, the walk hold, and `pty-4`, `pty-susp-pouch`, `item10-ctrlc`; lease 3's `viv-run`, `r5f9-ash`, `viv-console-ctrlc` on an image with the Alpine bundles, and G06, G07, G11, K60, which lease 2 had not booted. Final suite rc 0. The SMP subset (`default-smp4`, `ubsan-smp4`, 10 boots each) has NOT run at this commit: it is queued, and the push waits for it. Vote 17 named C4, so running it here is aux's reading of the hold as a narrow gate, not a vote. The full SMP matrix, `tools/test-fault.sh`, the interactive fleet and `tools/check-production.sh` did not run and are owed at the merge to main, under the operator's hold.
