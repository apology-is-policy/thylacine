---
id: fnd-thread-c4-r1-f1
type: fnd
round: adt-thread-c4-r1
severity: P2
status: fixed
title: "The direct post's rouse asked the target's mask: the store-buffer litmus test"
surface: [sub-kernel-notes]
threatens: [inv-i9]
fixed-by: chg-2026-11-02-thread-model-c4
regression: "rendez.direct_note_held_waits_ride_it_out reread1; fatal leg masked_still; K16; caught_target BUGGY_DIRECT_STALE_MASK"
created: 2026-11-02
---
## Prosecution

`proc_note_thread` released an OR of the word, then loaded `x->note_mask` relaxed. The owner stored its mask plainly, then took an acquire-only lock and read the word. ARMv8 allows both to read old: the poster skips the rouse, the owner reads an empty word and sleeps with an unmasked note. Latent, because every served unmask has an eret and an svc before a wait; the first served unmask-and-wait syscall makes it live. The model could not see it, since Mask and its tail read were one atomic step. Fixed: the rouse no longer asks the mask, and the reader's re-check is the precision, as the queue's wakers already do.
