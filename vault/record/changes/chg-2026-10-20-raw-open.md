---
id: chg-2026-10-20-raw-open
type: chg
title: "musl's raw opens through openat's translation, and __map_file maps private: time zone files load (0060)"
date: 2026-10-20
arc: arc-boosty
commits: ["550d138e1", "16419a1ce", "8f1871877"]
touched:
  - sub-pouch-fs
  - seam-pouch-errno-channel
  - sub-pouch-mem
  - sub-pouch-seam
  - sub-stratum-boot
  - sub-substrate-build
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-10-20
---
Pouch names the native open, 65, `__NR_open`, so musl's internal open macros
took their `SYS_open` arm and sent Linux's (path, flags, mode) to a call whose
arguments are (start_fd, path, length, omode). No time zone, locale or
message catalog file ever loaded, and `tzset` fell back to UTC without a word
(OPEN-BUGS 2026-10-15 O-4, found by the B-3b audit's second round). Behind
it stood a second refusal: `__map_file` asked for `MAP_SHARED`, which Pouch
refuses. Patch 0060 routes the four macros to `__pouch_sys_open`, `openat`'s
body made raw, maps `MAP_PRIVATE`, and returns 0023's stdio openers to
upstream's `sys_open` ([[sub-pouch-fs]], [[sub-pouch-seam]],
[[sub-pouch-mem]]). The raw-caller census rule existed and missed it: a
search for `SYS_open` finds only the macro block. `/bin/pouch-hello-tz`
writes its own TZif file and is the device witness ([[sub-stratum-boot]]).
On the CI image of `16419a1ce` `pouch-hello-tz` passed 5/5; without 0060
`tz-file`, `tz-map`, `tz-unmap` and `tz-reload` failed, the zone reading as UTC
with no map row, while the `tz-posix` control passed. The holotype round
(Fable 5.1, 0/0/0/2) moved the arm's fstat to the hidden `__fstat` (F2) and
found that the `EBADF` guard B-3f' added can never fire, because the kernel's
`SYS_FSTAT` answers a flat -1 for a dead handle where `docs/ERRORS.md` says
`-T_E_BADF` (F1; [[seam-pouch-errno-channel]]); that kernel half is the next
chunk. The close `8f1871877` was green on the device: suite 2046/2046,
`pouch-hello-tz` 5/5, the host checkers 86 legs.
