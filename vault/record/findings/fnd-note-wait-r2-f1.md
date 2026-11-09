---
id: fnd-note-wait-r2-f1
type: fnd
round: adt-note-wait-r2
severity: P1
status: fixed
title: "A fatal note posted during a masked poll is deferred, and the poll answers 0"
surface: [sub-kernel-notes]
threatens: []
fixed-by: chg-2026-11-04-poll-mask
regression: "notewait.poll_death_wake_keeps_the_mask; notewait.mask_end_reads_the_answer; pouch-hello-signals ppoll-dies, pselect-dies; viv-pheno-probe L368 mode 3; sabotage K40"
created: 2026-11-04
---
## Prosecution

The poll core answers a death wake with 0, on the footing that the thread dies at its tail and the result is immaterial. `notes_wait_mask_end` read 0 as no note's doing and put the caller's mask back, so the tail no longer saw the fatal note the call's mask had let in. An untimed `ppoll` returned 0, and the Proc lived until the same mask was next used, or for ever. Three shipped legs would have gone red at the first boot.

## Disposition

End keeps the mask for a 0 with a death pending, so the tail ends the Proc under it. A ready count still restores, as in Linux.
