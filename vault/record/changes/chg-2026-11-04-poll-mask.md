---
id: chg-2026-11-04-poll-mask
type: chg
title: "ppoll's and pselect's mask, and sigpending, on SYS_NOTE_WAIT"
date: 2026-11-04
arc: arc-phase6-pouch
commits: ["94cf380fd", "994735ddf", "a03d9b4fd", "982d66e93"]
touched:
  - sub-kernel-notes
  - sub-kernel-vivarium
  - sub-kernel-syscall-abi
  - sub-pouch-signal
established: [dec-2026-11-04-poll-mask]
closed: []
opened: []
depth: skeletal
created: 2026-11-04
---
Operator vote 28. `notes_wait_mask_begin` and `notes_wait_mask_end`, `NOTE_WAIT_POLL` and
`NOTE_WAIT_PENDING`; Linux's `ppoll`, `pselect6` and `rt_sigpending` serve what they refused.
Patch 0071 carries the mask through `__pouch_poll` and `__pouch_select`, and `sigpending` through
the pending form. It fixes two defects in Pouch's `select`: every failure read `EIO`, and a closed
fd read as a timeout. Witnesses: viv-pheno-probe L99, L131, L359 to L368; pouch-hello-signals
`ppoll-*`, `pselect-*`, `sigpending`, `select-eintr`, `select-badfd`; test_note_wait's seven tests.
