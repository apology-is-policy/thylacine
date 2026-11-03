---
id: dec-2026-11-02-usr-notes-chunk
type: dec
title: "SIGUSR1 and SIGUSR2 become notes in a chunk of their own, straight after C4"
date: 2026-11-02
status: standing
decided-by: user-vote
affects: [sub-kernel-notes, sub-kernel-vivarium, abi-note-names]
created: 2026-11-02
---
## Fork

C4's word carries one bit per note family, and SIGUSR1/2 have no family. Options: widen the words inside C4; a separate chunk after it; leave the two signals without a note.

## Decision

Vote 15 of 2026-11-02: a separate chunk straight after C4, on bits 8 and 9, widening `note_caught` to u32 and `note_claim`, `note_target`, `note_direct` to u16. The vote is also the signoff for the `docs/ERRORS.md` change.
