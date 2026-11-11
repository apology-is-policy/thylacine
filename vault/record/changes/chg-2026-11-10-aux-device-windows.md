---
id: chg-2026-11-10-aux-device-windows
type: chg
title: "The kernel never touches a device window on a user's behalf: /proc mem, claims over RAM, the CPU's address size, a bus error in a copy"
date: 2026-11-10
arc: arc-mac-runner
commits: ["e5e9d7b2e", "f48fc214c", "5340e2713", "91c0d51af"]
touched:
  - sub-kernel-mmu
  - sub-kernel-mm-phys
  - sub-kernel-hwcap
  - sub-kernel-exception
  - sub-kernel-fault
  - sub-kernel-uaccess
  - sub-kernel-devproc
  - sub-kernel-syscall-dispatch
  - sub-substrate-machine
  - sub-substrate-gates
established: []
closed: []
opened: []
mirrors-checked:
  - "kernel/syscall.c: the Linux-phenotype accept reads and writes the same addrlen word, byte-wise; no syscall's interface changes"
  - "EXTINCTION bodies: one reworded, mmu_map_mmio's 'PA exceeds TCR.IPS=40 (1 TiB)' -> 'PA exceeds the physical address size'; nothing in tools/, vault/ or docs/agent matches either text (prose under dec-2026-09-25-extinction-bodies)"
depth: skeletal
no-dossier-change: "the note changes no code; the touched dossiers were co-staged with the code in e5e9d7b2e, f48fc214c, 5340e2713 and 91c0d51af"
created: 2026-11-10
---
Four holes of one class, a user leaf the kernel cannot safely touch for the user; each was reproduced on a build without its fix before the fix was trusted. Branch aux-3-mac, after 6f8ab5387 ([[chg-2026-11-10-aux-mmio-forms]], whose audit read the first one from the code).

**The mechanisms.** `cross_proc_resolve` answers NULL for a leaf whose address is not RAM (`phys_pa_is_ram`), so `/proc/<pid>/mem` at a device window reads as a hole; it judges by address, since a cached host-memory window is a device behind a Normal attribute. `kobj_mmio_create` refuses a range that shares a byte with the DTB's memory bank (`phys_range_touches_ram`) and anything past `mmu_pa_bits()`, the CPU's PARange capped at the 40 bits TCR_EL1.IPS is programmed for; `mmu_install_user_pte`, the leaf replace and `mmu_map_mmio` use the same bound. A synchronous external abort on a data access at a uaccess PC goes to the fixup (`fault_is_user_access_bus`); an alignment fault there stays fatal, and the one caller that skipped the alignment check (the Linux-phenotype `accept`) reads `addrlen` byte-wise.

**Found on the way.** The fourth hole (40 bits assumed on a CPU with 36) turned up while choosing an address for the bus-error witness. TCG raises a synchronous external abort on an unassigned address; HVF completes the access, so the route needed a test that drives the handler with a synthetic frame.

**Round 2 (Opus 5.5 max, on e5e9d7b2e: 0/0/1/5, not dirty).** The P2: the test could not tell an address test from an attribute test (f48fc214c). F2 and F3, outside that diff, are the RAM claim and the bus error. **Round 3 (Opus 5.5 max, on 5340e2713: 0/0/0/10, clean; 91c0d51af).** F8 and F9 (a device error as an SError; DFSC 0x18) are recorded in [[sub-kernel-exception]] as not handled.

**Leases** (`aux-gate/macrunner/m5`, `m7`, `m8`). Every sabotage red as predicted: translation-fault extinctions at `mmu_cross_proc_read` and `mmu_cross_proc_write`, ESR 0x96000010 at `uaccess_load_u8` under TCG, ESR 0x96000003 (address size) under HVF, and named assertion failures for the rest. m8 (2026-11-11, on macOS 27.2.1 after the operator's update; the earlier leases ran on 26.4, and the hypervisor probe reads the same on both): round 3's three sabotages red as predicted (no bus route: ESR 0x96000010 in `uaccess.bus_abort_resumes_at_the_fixup` under HVF; every external abort taken: "a fault status other than 0x10 is not taken"; a start-only RAM predicate: "a range that starts below and ends inside touches"), and 91c0d51af (112730ff19bc2bff) 2447/0 on stock HVF GICv2, QEMU 11.1.3 in-kernel GICv3 and TCG GICv3.

**Not run:** `tools/test-fault.sh` past 3 of 23 variants (all passed), thyla-pi, the full SMP matrix (held since 2026-11-02), the interactive fleet. **Open:** TCR_EL1.IPS is 40 on a 36-bit CPU; the DTB parser reads one memory bank; the direct map covers 1 to 9 GiB whatever the DTB says; SError and DFSC 0x18 at a uaccess (OPEN-BUGS 2026-11-10, bare-metal work).
