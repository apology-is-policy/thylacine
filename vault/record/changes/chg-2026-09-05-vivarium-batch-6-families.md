---
id: chg-2026-09-05-vivarium-batch-6-families
type: chg
title: "sub-kernel-vivarium brought current: the 6.25-6.27 syscall batch, the D-3 mmap + #50 path-mutation shapes, the ceiling to 109"
date: 2026-09-05
arc: arc-vault
commits: []
touched:
  - sub-kernel-vivarium
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-05
---
The fourth kernel-entry giant this session (the biggest by churn, 2671 lines
since 2026-08-16). The dossier's PRINCIPLES -- the admission rule, the four
verdicts, the reject-table-as-data, the collision re-check, the fd-freeing
obligation, and the whole sigtab torn-write / cross-process-UAF Concurrency
section -- are current and excellent; they were left intact. The new syscall
families are INSTANCES of those principles, so the de-stale is the enumerations,
the counts, the two stale caveats, and a principle-level note on the two
genuinely-new admission SHAPES. Every count re-measured against vivarium.c
(audit:hard).

## The enumerations (measured)

- T1 pure-renumber table 6 -> **11** rows (added lseek, pread64, pwrite64,
  getpid, getpgid, getsid).
- reject table ~50 -> **73** rows.
- The Contract table gained the decide functions that landed since: the #50
  path-mutation family (`openat_create`/`mkdirat`/`unlinkat`/`renameat`), the
  D-3 file-backed mmap family (`mmap_file`/`mmap_fixed_file`/`mmap_fixed_anon`),
  the poll family (`ppoll`/`pselect6`), the V-5 socket data path
  (`recvfrom`/`recvmsg`/`sendto`), and `faccessat`/`ioctl`/`futex`/`getsockopt`.
  It had frozen at the read-only surface it held before the git-under-VIVARIUM
  and D-3 work.

## The two new admission SHAPES (Mechanism)

- **The mmap decision fanned into four arms at D-3.** The file arms admit a
  read/exec map riding a shared `BURROW_TYPE_FILE` Burrow demand-paged from the
  file (the I-36 generalization to mmap-time) and REFUSE `PROT_WRITE` outright --
  no write-back path, so a writable file map would lose or leak the guest's
  writes and corrupt every other Proc sharing the Image. Allow-list, not a
  `PROT_WRITE` name. Added to Mechanism + Invariants (I-36, new to this dossier's
  list -- kept as prose, guarded-by unchanged [inv-i43], the dossier's style).
- **The #50 path-mutation family** is the create/remove half of the layer --
  ordinary admissions of the admission rule, documented so the Contract table
  does not read frozen at read-only.

## The two stale caveats (re-derived)

- **Caveat #164 (`VIV_NATIVE_CEILING`)**: the dossier recorded the constant as
  105; the code has since moved it to **109** (`SYS_OPEN_CREATE`, the #50 family
  -- matching the syscall-abi census done earlier this session). The declaration
  comment still narrates only the 100->102 move, so the defect (prose lagging the
  symbol) PERSISTS and stays tracked as #164, but the countable facts updated:
  ceiling 105->109, and the "seventeen rows below 105" is now **forty-two**
  `VIV_LINUX_*` enum values below 109.
- **Caveat #163 (the header dead-code claim)**: VERIFIED still present in
  vivarium.h (the "Nothing here is wired into syscall_dispatch ... provably
  always 0" block) -- the code defect is unchanged, so the caveat stays accurate
  as written. Left intact.

## Out of scope (correctly)

Design D (execve re-decides the phenotype) and PHENOTYPE-FORK-INHERITS-CAPS
(6.26) live in proc.c/proc.h, NOT vivarium.c -- they belong to
[[sub-kernel-syscall-dispatch]] (done this session) and [[sub-kernel-proc]]
(pending). vivarium.c is the per-syscall translation table, not the per-process
phenotype decision. `updated:` -> 2026-09-05.

## Remaining stale giants

sub-stratum-boot (joey.c ~5659), sub-substrate-build (1713), the proc.c cluster
(proc/jobctl/caps/death ~772).
