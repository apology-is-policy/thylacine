---
id: chg-2026-05-25-16b-gamma-syscalls
type: chg
title: "16b-gamma: native fstat + lseek, and open() through openat()"
date: 2026-05-25
arc: arc-pouch-boot
commits: ["af6fd824"]
touched:
  - sub-pouch-fs
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-01
---
`__NR_fstat` and `__NR_lseek` un-parked from the sentinel to the
Thylacine numbers (spelled `#undef` + `#define` so the redefinition is
loud), `fstat.c` rewritten to translate `struct t_stat` into musl's
`struct stat`, and `open()` redirected to call the PATCHED `openat()`
instead of expanding `__sys_open_cp` into a raw `SYS_openat` -- which
had been bypassing 0009's work entirely.

The `t_stat` mirror introduced here is the first of three in the series;
all three later had to grow to 88 bytes for #100's `devno`.
