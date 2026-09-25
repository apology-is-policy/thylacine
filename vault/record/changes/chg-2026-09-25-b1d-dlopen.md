---
id: chg-2026-09-25-b1d-dlopen
type: chg
title: "B-1d (dlopen): SYS_BURROW_MAP_FILE, PT_INTERP for native execs, libc.so as the loader, the initrd's bin/ and lib/, /lib as a union, and the device witness"
date: 2026-09-25
arc: arc-boosty
commits: ["dfdd6344"]
touched:
  - sub-kernel-exec
  - sub-kernel-syscall-abi
  - sub-kernel-syscall-dispatch
  - sub-kernel-vivarium
  - sub-kernel-content
  - sub-kernel-joey
  - sub-kernel-territory
  - sub-kernel-devproc
  - sub-pouch-mem
  - sub-pouch-seam
  - sub-pouch-net
  - sub-stratum-boot
  - sub-substrate-build
  - sub-substrate-remote-host
  - sub-warden
  - sub-libhalcyon
  - sub-parley
  - sub-utopia-eval
  - sub-utopia-interactive
  - sub-coreutils-filters
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-25
---
B-1d's exit: a Pouch-built `.so` loaded by a Pouch host on the device, and both
deny paths ([[dec-2026-09-24-b1d-loader-shape]]). The kernel exposes DISTRO
D-3's file-map arms natively as SYS_BURROW_MAP_FILE (126), with `addr` as its
sixth argument under BURROW_MAP_FIXED and every executable window vouched (a
Dev that may back code, on a mount not marked MNOEXEC, else EACCES), and lifts
PT_INTERP to every phenotype: one level, the interpreter resolved in the
Proc's own namespace. `libc.so` is musl's own loader, built by a second
configure through the fork clang; patch 0047 routes musl's file maps to 126,
and 0048 makes RELRO a protect reduction that fails the load on any error. The
fork driver learns `-shared` and `-pie` and refuses `-static-pie`. The initrd
gains directories ([[dec-2026-09-25-devramfs-directories]]) and keeps its
programs in `bin/` and the loader in `lib/`
([[dec-2026-09-25-initrd-bin-directory]]); after the pivot joey mounts `lib/`
MBEFORE the disk's `/lib`, a union [[chg-2026-09-25-b1d-u-unions]] made
possible. The witness, `/pouch-hello-dlopen`, runs on every boot: its loader,
the MNOEXEC refusal (EACCES), the confined refusal (ENOENT, with a control on
each side of the pivot), the load, the segments and RELRO. Audited over three
rounds ([[adt-b1d-r1]], [[adt-b1d-r2]], [[adt-b1d-r3]]).

The dossier gate, run on the landing as one commit rather than on each WIP
commit, named two owners the dossier pass had missed. [[sub-pouch-seam]] and
[[sub-pouch-mem]] still counted four numbers in `_pouch_mman.h`, where 0047
adds a fifth, and [[sub-utopia-interactive]] still described the completion
index and the command resolver disagreeing by `/`, a disagreement B-1d
removed. Both are corrected in the landing commit.
