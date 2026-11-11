---
id: chg-2026-11-10-aux-mmio-forms
type: chg
title: "Device accesses take one instruction form; GICv3 boots under HVF"
date: 2026-11-10
arc: arc-mac-runner
commits: ["40d77f497", "6f8ab5387"]
touched:
  - sub-kernel-gic
  - sub-kernel-hwcap
  - sub-kernel-timer
  - sub-kernel-uart
  - sub-kernel-halls
  - sub-substrate-machine
  - sub-libthyla-rs
established: []
closed: []
opened: []
mirrors-checked:
  - "usr/lib/libthyla-rs/src/hardware.rs: the userspace accessors already took the fixed form (#890); only a comment changed"
depth: skeletal
no-dossier-change: "the note changes no code; the touched dossiers were co-staged with the code in 6f8ab5387"
created: 2026-11-10
---
The Mac runner's first measurement booted the ci image on QEMU 11.1.3 with Apple's in-kernel GICv3 and the guest died at GIC bring-up on QEMU's `assert(isv)`, the assert the tree had blamed on QEMU's emulated distributor. Cause: `dist_init`'s loop compiled to a post-indexed store (`gic_init+0x15c`, `str wzr, [x10], #0x8`, `GICD_ICFGR`), and a hypervisor gets a valid instruction syndrome only for a load or store of one general register with no writeback. Proof: QEMU's `hvf_data_abort` trace shows one abort with `isv=0` per boot, a write at IPA `0x08000c0c`, on QEMU 11.1.3 and stock 10.0.2 alike. Fix: every kernel device access goes through `arch/arm64/mmio.h`; `tools/check-mmio-forms.py`, fatal in the build, refuses the old idiom. The GICv2 `dsb` after each access stays (the one after the `GICC_IAR` read orders the barrier handler's ticket read).

**Round 1 (Opus 5.5 max, 0/0/0/4 in the change, not dirty).** The four P3s were applied before the final build. One P1 outside the change, `/proc/<pid>/mem` at a device leaf, is [[chg-2026-11-10-aux-device-windows]].

**Leases** (`aux-gate/macrunner/m2`, `m3`). Final build 1abdae4194c9c021: 2442/0 six times (QEMU 11.1.3 in-kernel GICv3 four, stock HVF GICv2, stock HVF GICv3 with the ITS); 1,350,398 trapped accesses in a traced suite, none without a syndrome.

**Not run:** thyla-pi, the full SMP matrix (held since 2026-11-02), the interactive fleet.
