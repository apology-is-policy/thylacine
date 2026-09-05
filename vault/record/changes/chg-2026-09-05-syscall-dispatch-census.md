---
id: chg-2026-09-05-syscall-dispatch-census
type: chg
title: "sub-kernel-syscall-dispatch census re-derived (14731 lines, 107 arms, 50 split) + execve Design-D phenotype re-decision"
date: 2026-09-05
arc: arc-vault
commits: ["72d09ada"]
touched:
  - sub-kernel-syscall-dispatch
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-05
---
The sibling of [[sub-kernel-syscall-abi]] (done earlier this session): the abi is
the contract (numbers, records, mirrors), this is the machinery (the dispatch
switch, marshalling, staging, the layering rule). It was re-swept at the
2026-08-15 lineage change, so its STRUCTURE -- the phenotype prologue, the three
frame-taking handlers, the handler/inner split, the two staging tiers, the two
error conventions -- is current and was left intact. What was stale was the
census and one mechanism.

## Census re-derived by measurement (audit:hard)

Every count re-measured against kernel/syscall.c, not incremented:

- **file 11138 -> 14731 lines** (~4300 moved since the last sweep).
- **live handlers / dispatch arms 103 -> 107**, sets equal both directions
  (verified by isolating syscall_dispatch's body and diffing its case set against
  the enum -- the same measurement that backs the abi dossier).
- **grep case labels 106 -> 110**, still +3 for the pts inner switch (110 total
  - 107 distinct dispatch = 3), so the "grep over-counts by the pts routing"
  parenthetical stays true at the new number.
- **handler/inner split 49 -> 50** (50 distinct `_for_proc` col-0 defs);
  **non-static inners 41 -> 45** (5 static: fstat, open_kpath, readlink, two
  spawn variants). The testability framing is unchanged; the numbers moved with
  the new syscalls.
- **sys_validate_user_buf call sites 69 -> 88** (the I-13 boundary validator).
- **three never-returning arms UNCHANGED** (the 2 exits + thread-exit; 7 total
  `noreturn` attributes in the file, but only 3 are dispatch arms -- not
  conflated).

The new syscalls since 2026-08-18 (FD_DEVCLASS=80, OPEN_CREATE=109,
HOSTMEM_REFCOUNT=108, and the VIVARIUM 6.25-6.27 batch) all slot into shapes the
dossier already describes -- ordinary handler/inner splits or phenotype
translation rows -- so none needed a new Mechanism section. Verified by reading
their handlers, not assumed.

## The one structural addition: execve re-decides the phenotype (Design D)

sys_execve_core now computes `new_pheno = phenotype_decide(crossed_pheno,
territory_root_pheno(p->territory))` at the resolve and commits it in
proc_exec_replace's single infallible region, alongside the address-space swap
(kernel/syscall.c ~9593/9622). So the ABI numbering the next instruction meets
and the memory it runs in flip together or not at all. Added to the execve
ordering section as a third load-bearing placement, and to the I-43 invariant
section as the SECOND place "shape, never authority" lives (the entry prologue is
the first): a native binary exec'd inside a Linux vivarium comes out native --
shape follows the image, never the caller, and authority is untouched. The
prologue's "declared at spawn" line now notes the re-decision too. This is the
dispatch half of the same Design D whose territory half landed in
[[sub-kernel-territory]] this session.

## Frontmatter

`updated:` -> 2026-09-05. guarded-by/abis unchanged (abis is empty -- no ABI
struct owned here; the records live in the abi dossier).

## Remaining stale kernel giants

sub-stratum-boot (joey.c, ~5659 churn), sub-kernel-{vivarium 2671, stalk 1251,
proc 1097 cluster}, sub-substrate-build (1713) -- each its own de-stale.
