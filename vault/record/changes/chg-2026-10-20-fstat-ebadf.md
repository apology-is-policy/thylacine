---
id: chg-2026-10-20-fstat-ebadf
type: chg
title: "SYS_FSTAT answers its errnos: a closed descriptor reads EBADF from fstat and isatty (0061)"
date: 2026-10-20
arc: arc-boosty
commits: ["2bb84a736", "cffe41eff", "52a1a64c5"]
touched:
  - sub-kernel-syscall-dispatch
  - sub-kernel-syscall-abi
  - sub-kernel-ninep-dev9p
  - sub-kernel-dev
  - sub-kernel-spoor
  - sub-kernel-devdev
  - sub-stratum-boot
  - sub-pouch-seam
  - sub-pouch-fs
  - sub-pouch-tty
  - seam-pouch-errno-channel
established: []
closed: [seam-fstat-errno-flattened-above-the-leaf, seam-devdev-winsize-statless]
opened: []
mirrors-checked: []
no-dossier-change: "the close changes no code; the touched list carries the chunk's whole set, whose dossiers were co-staged with its code and fixes in cffe41eff and 52a1a64c5"
depth: skeletal
created: 2026-10-20
---
`SYS_FSTAT` answered a flat `-1` for every failure, where `docs/ERRORS.md`
already promised `-T_E_BADF` for a dead handle. Pouch read the `-1` as EIO, and
a Linux guest and Go read it as EPERM, so no program could tell a closed
descriptor from a failing file (OPEN-BUGS 2026-10-20 07:48Z, raised by the
raw-open round's F1). `2bb84a736` makes the core answer -errno. A handle that
names no Spoor gets `-T_E_BADF`, and a Dev's refusal is mapped the way stalk's
`err_code` maps it. The handler checks the destination first and answers
`-T_E_FAULT` ([[sub-kernel-syscall-dispatch]], [[sub-kernel-syscall-abi]],
[[sub-kernel-dev]]). Patch 0061 removes fstat.c's dead branch. ioctl now asks
`SYS_FSTAT` whether a refused descriptor is dead, so `isatty` on a closed fd
sets EBADF where it used to set ENOTTY ([[sub-pouch-fs]], [[sub-pouch-tty]],
[[sub-pouch-seam]], [[seam-pouch-errno-channel]]). `/bin/pouch-hello-fd`
gains the `fstat-ebadf` and `isatty-ebadf` legs ([[sub-stratum-boot]]). The
holotype round (Fable 5.1, 0/0/1/2) found that a 9P server's EBADF crossed
`SYS_FSTAT` as if the handle were dead. Pouch's `O_DIRECTORY` unwind would then
have leaked a live descriptor (F1). `52a1a64c5` folds a Dev's `-T_E_BADF` to
`-T_E_IO` and answers a NULL ioctl argument with EFAULT once the descriptor is
known to be a terminal ([[sub-kernel-ninep-dev9p]], [[sub-kernel-spoor]]). The
pass closed two seams. The native-stat arm's comment is now true in both
halves ([[seam-fstat-errno-flattened-above-the-leaf]]). The geometry leaf has
answered fstat since H-1 ([[seam-devdev-winsize-statless]],
[[sub-kernel-devdev]]).
On the CI image of `e4a0cfded`, built with the libraries, the suite passed
2054/2054 with `syscall.fstat_errnos`, `pouch-hello-fd` passed 19/19 and the
links and time zone provers 10/10 and 5/5. With `SYS_FSTAT` back to its flat
-1, exactly `syscall.fstat_errnos` failed. With that test unregistered, exactly
`fstat-ebadf` (errno 5) and `isatty-ebadf` (errno 25) failed. Without 0061,
`isatty-ebadf` failed alone. The restored tip was green again.
