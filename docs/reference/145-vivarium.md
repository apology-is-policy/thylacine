# 145 — VIVARIUM: the Linux-binary-compatibility pole [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-07-vivarium-doc-absorb`).
The master reference for Thylacine's fourth pole — running unmodified Linux
binaries — governed by **I-43** (a phenotype confers ABI *shape*, never
*authority*). Three parts, each with its dedicated home, plus the DISTRO arc that
makes real distro binaries load:

- **the ABI dispatch** (`kernel/vivarium.c` + `vivarium.h`) — the syscall-entry
  phenotype branch (`viv_linux_dispatch`), the T1/T2 translation table (getdents64,
  openat/mkdirat/unlinkat/renameat, pread64/pwrite64, faccessat/readlinkat/
  getrandom, O_APPEND, …), the phenotype-decided-at-every-image-load model
  (execve re-decides; `Territory.root_pheno`), the **per-note phenotype-sigtab
  gate** (a note's class is scanned against the phenotype sigtab), and the V-8
  audit close:

      vault/system/kernel/entry/sub-kernel-vivarium.md   (audit: hard, I-43)

- **the world** (`/sbin/diorama`) — the read-only 9P server presenting Thylacine's
  `/proc` and `/ctl` in Linux shapes, plus the `--vivarium` per-container mode
  (already retired from `docs/reference/141-diorama.md`):

      vault/system/userspace/services/sub-diorama.md   (audit: hard, I-43)

- **the runner** (`usr/viv`) — `viv run <bundle>`: the OCI-runtime half, the
  capability mechanics (holds no authority beyond the invoker's; I-23), the
  pre-open-then-chroot assembly, the phenotype-declared-at-one-call rule, and —
  **now folded** — the ^C-reaches-the-container-not-the-runner masks:

      vault/system/userspace/services/sub-viv.md   (audit: light, I-43/I-23)

- **the DISTRO arc** (real distro binaries load) — distributed by surface:
  D-1 symlink expansion in `stalk` (`sub-kernel-stalk`); D-2 ET_DYN placement +
  AT_ENTRY (`sub-kernel-elf`); D-3 file-backed EL0 mmap (`sub-kernel-vma` +
  `sub-kernel-fault` + `sub-kernel-image`); D-4 PT_INTERP rewrite (`sub-kernel-exec`);
  the `/viv/bin` MPHENO_LINUX mount (`sub-kernel-territory` / `sub-kernel-stalk`).

**What this file got WRONG or MISSED by the time it was absorbed:**

- **One code-grounded fold — the ^C-mask container behavior (2026-08-18, post the
  2026-08-06 sub-viv dossier).** `viv` runs in `ut`'s foreground pgrp with its
  diorama and every container Proc, so a `^C` posts `interrupt` to the whole group
  and — before the fix — killed the native `viv`/diorama (uncaught `interrupt`,
  LS-5 default), orphaning the container. Folded into sub-viv
  (`chg-2026-09-07-vivarium-doc-absorb`; `updated:` 2026-08-06 -> 09-07): `viv`
  masks `interrupt` at startup; nothing leaks into the container because a native
  child starts zero-mask — `rfork_internal` copies `note_mask` only when the parent
  is `PHENO_LINUX` (verified proc.c:1614); the tty family stays unmasked so `^Z`
  stops `viv` with the container (for `ut`'s `wait_pid(WUNTRACED)`), hangup ends it,
  and `^\` detaches like `docker run`; the diorama masks both families.
- **DISTRO D-5 is a build/test artifact, not a kernel mechanism.** The
  `/vivarium/alpine-stock` stock-rootfs bundle + its viv-run E2E arc gate stage
  from the build script (`sub-substrate-build`) and gate through the runner E2E;
  no dossier is owed for the bundle itself.
- **Everything else was covered** — the phenotype dispatch, the T1/T2 table, the
  per-note sigtab gate, the V-8 close, the diorama world, the runner mechanics, and
  the whole DISTRO D-1..D-4 arc are all as-built across the dossiers above. Zero
  code change.
