---
id: chg-2026-11-03-note-wait
type: chg
title: "sigsuspend, sigtimedwait, sa_mask, SA_RESETHAND: the kernel owns the mask"
date: 2026-11-03
arc: arc-phase6-pouch
commits: ["107bb2d8e", "99e35300a", "057b24967", "087d56d79", "a1381a1d7", "2656ca35b", "ea0d92b15"]
touched:
  - sub-kernel-notes
  - sub-kernel-vivarium
  - sub-kernel-syscall-abi
  - sub-pouch-signal
established: [dec-2026-11-03-note-wait]
closed: []
opened: []
depth: skeletal
created: 2026-11-03
---
Operator vote 24. `SYS_NOTE_WAIT` = 141, and `notes_noted_restore` restores the mask for both
phenotypes. Pouch's patch 0071 removes its copy of the mask, puts `sigsuspend` and `sigtimedwait`
on the two forms, carries `sa_mask`, `SA_NODEFER` and `SA_RESETHAND` in the row, routes
`sigsetjmp` through `pthread_sigmask`, and gives every handler a real `siginfo_t`. ARCH 8.8.3
and 7.6.3 hold the mechanism.
