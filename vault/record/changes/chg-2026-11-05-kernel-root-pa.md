---
id: chg-2026-11-05-kernel-root-pa
type: chg
title: "A kernel thread's TTBR0 is the kernel's root at last, not a high VA"
date: 2026-11-05
arc: arc-phase6-pouch
commits: [64f75eba9]
touched:
  - sub-kernel-mmu
established: []
closed: []
opened: []
depth: skeletal
created: 2026-11-05
---
`g_l0_ttbr0_pa` was a static written once, before the MMU, with one link-time address, so clang shrank it to a byte and `mmu_kernel_ttbr0_pa` recomputed the address at its caller's PC: a high VA, on main since P3-Bdb (fd400476b, 2026-05-06). Every kernel thread but the boot thread ran with ASID 0xffff and a root made of VA bits, never the empty tables the retired identity leaves. The ELF shows it, here and on main's tree. C9's switch decides on the ASID field, so `context.ttbr0_follows_next` went red at the kernel hop, and switch-cost's direct row did not drop by the V pair. `volatile`, as `kaslr.c` has had for the same reason; S10 reverts it. No other pre-MMU store of an address remains: `gic.c`'s run after the MMU. Separately, `test_fp.c`'s `fp_load_regs` named V clobbers, so the compiler restored d8-d15 on its way out over what it had just loaded: a fixture bug of mine, red on all three FP tests whatever the kernel did.
