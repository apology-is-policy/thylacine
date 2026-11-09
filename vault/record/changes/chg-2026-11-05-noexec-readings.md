---
id: chg-2026-11-05-noexec-readings
type: chg
title: "ENOEXEC's readings: a fixture the look voided, two legs that never ran, and a leg nothing had turned red"
date: 2026-11-05
arc: arc-phase6-pouch
commits: [e76b54650, c1bd6a67b, 7f7fab418]
touched:
  - sub-kernel-exec
  - sub-substrate-build
established: []
closed: []
opened: []
depth: skeletal
created: 2026-11-05
---
`e76b54650` is the blob path's half of `fnd-noexec-r1-f4`: `exec_map_segment` flattened a refused VMA to -1, and now it says `-T_E_NOMEM`. Witness `exec.setup_says_nomem_on_a_refused_vma`.

The first boot, at `33d1e4d96`, was 2263 of 2263, and joey then ended at ambush-probe's stage D. `/bin/ambush-notelf`, a file of text, was there so that a launch would make a child which then dies in its load. The parent's look now refuses that file at the spawn, as designed, so there was no child: `fork/exec /bin/ambush-notelf: exec format error`. The kernel was right. `c1bd6a67b` gives stage D `/bin/ambush-nointerp`, a header with one `PT_INTERP` naming nothing.

The run at `c1bd6a67b` applied 26 sabotages. 21 read as predicted, and five did not:

- N24, N25: green. `chg-2026-11-04-libbad` says these legs are "conditional on the phenotype bundle, with sabotages proving each ran". The kernel tests run before the bundle is mounted, so the legs had never run, and the sabotages proved that instead. `7f7fab418` puts `/bin/exec-notelf` and `/bin/exec-empty` in the ramfs and makes the legs unconditional. Each then went red on its own message.
- N01: red on `pouch-hello-fs`'s `posix_spawn(noimg)`, with errno 22, which runs before viv-pheno-probe, so L370 was never reached. No sabotage had turned L370 red. N27 turns it red alone, through an arm only `sys_execve_core` has. N01 now reds in the kernel first.
- N13, N14: red in the named test, on an earlier message than predicted.

No guest had read `ELIBBAD` before L373. N28 turns it red alone, and L374 has N35.

N31, at `bb179781d`, took `ELF_LOAD_EXEC_STACK` out of `exec_refused_by_policy` and stayed green. The legs set `PT_GNU_STACK` to RWE, and `elf_load`'s type-blind W^X check takes that first, so they never reached the arm. `c32114c7c` asks both forms: R+X reaches `EXEC_STACK`, and RWE, what a linker's `-z execstack` writes, reaches the W^X check. Both read `T_E_ACCES`, and N31 goes red. So a real execstack binary reads `RWX_REJECTED`, and only a hand-made R+X stack reaches `EXEC_STACK`.

`execve.interpreter_says_why`'s `/bin/pouch-hello-dlopen` leg stays conditional, on the sysroot having a libc.so. N18 and N23 show it ran on this image.

Specs. No spec models `kernel/exec.c`'s answers. `vma_alloc` and `burrow_map_in` are `burrow.tla`'s `MapVmo`, and this chunk changes what a refusal answers, not a state, so `specs/check-burrow.sh` ran once, at `7f7fab418`: clean at 100 distinct states and the five buggy cfgs each on its named invariant. `bb179781d` does not touch those two functions.

`fnd-noexec-r2-f1`'s report cited POSIX for EACCES on an unsupported file type. POSIX's text (exec, ERRORS) is "not a regular file and the implementation does not support execution of files of its type" and "denies execution permission", which does not name this case. It defines ENOEXEC as "has the appropriate access permission but has an unrecognized format", and a W+X ELF is a recognized format, so that part supports vote 33.
