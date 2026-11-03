---
id: chg-2026-10-29-env-bind-at-open
type: chg
title: "An open /env file binds its opener's environment; unknown syscalls answer ENOSYS; ut's unset reaches /env"
date: 2026-10-29
arc: arc-boosty
commits: ["e3ba2a540"]
touched:
  - sub-kernel-content
  - sub-kernel-syscall-dispatch
  - sub-kernel-syscall-abi
  - sub-kernel-dev
  - sub-kernel-territory
  - sub-utopia-eval
  - sub-coreutils-filters
established: []
closed: []
opened: []
mirrors-checked: []
no-dossier-change: "the note changes no code; the touched dossiers were co-staged with the code in e3ba2a540"
depth: skeletal
created: 2026-10-29
---
Three operator votes on `/env`, the `b3` sub-chunk between B-3g and B-3h
([[dec-2026-10-22-env-bind-at-open-sequenced]],
[[dec-2026-10-22-unknown-syscall-enosys]], [[dec-2026-10-28-ut-unset-env]]).
devenv resolved the calling process on every operation, and spawn copies
the environment, so `echo 1 > /env/V` typed at ut emptied ut's `V` and
wrote `1` into echo's copy. `e3ba2a540` makes open and create take a
reference on the opener's Env into the Spoor, owned exactly while the Spoor
is open; every later read, write, stat and truncate through it acts there,
whoever holds the handle. The directory stays the caller's, and an unopened
entry carries the devno of the Env that resolved it
([[sub-kernel-content]], [[sub-kernel-territory]]). A new
`Dev.no_file_mapping`, set by devenv, makes both mapping cores refuse an
`/env` entry with `ENODEV` after the exec floor, because the Image cache
would keep the entry, and so its environment, alive past every handle
([[sub-kernel-dev]]). An unknown syscall answers `ENOSYS` where it answered
a bare -1, and joey's V-1b leg now asks getpid
([[sub-kernel-syscall-dispatch]], [[sub-kernel-syscall-abi]]). ut's `unset`
also removes the name from the shell's own `/env`, since a spawned `rm`
removes it from its own copy only ([[sub-utopia-eval]],
[[sub-coreutils-filters]]).
Two Opus rounds ran (Fable was out): round 1 found 1 P0, 1 P2 and 5 P3s,
round 2 found 10 P3s. The P0 was `unset` of a name the shell's `/env`
lacked failing, because devenv's unlink answered a bare -1; the P2 was the
mapping retention. The lease then found one more: round 2's reorder of the
non-fixed mapping core had put the new gate ahead of the exec floor, and
`mmap_file.devenv_never_exec_backs` failed. Both cores now ask the exec
floor first, as Linux asks noexec before a missing mmap.
On the CI image of this tree over Stratum `925cfc2`, built with the libraries, the suite passed
2084/2084, and ls-3b, manual and ls-libs passed. Eight kernel mutants each turned exactly
their own legs red: the bind ignored, the devno check gone, unlink's bare -1, the mapping gate
dropped in each core or moved past the Image cache, and the gate put back before the exec
floor in each core. Each restored tree was green again.
Tracked in OPEN-BUGS, not fixed here: devenv's refusals answer EIO and want
an errno channel of their own, and other Devs whose content changes without
a `qid.vers` bump are served stale from the Image cache.
