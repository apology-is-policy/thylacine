---
id: abi-pci-windows
type: abi
kind: registry
stability: append-only
title: "PCI mappable windows: routing pages stay in the kernel"
pinned-by:
  - "sizeof(struct pci_map_window) == 24 in pci_handle.h"
  - "sizeof(TPciWindow) == 24 and bar offset 16 in libthyla-rs"
  - "VIV_NATIVE_CEILING == SYS__NATIVE_TOP - 1 in vivarium.c"
mirrors:
  - "kernel/include/thylacine/syscall.h: SYS_PCI_MAP_WINDOW and SYS_PCI_WINDOWS"
  - "kernel/include/thylacine/pci_handle.h: pci_map_window"
  - "kernel/syscall.c: map and query handlers"
  - "usr/lib/libt/include/thyla/syscall.h: t_pci_window and wrappers"
  - "usr/lib/libthyla-rs/src/lib.rs: TPciWindow and wrappers"
  - "kernel/include/thylacine/vivarium.h: native ceiling (123 after trusted-seat/nonblocking append)"
created: 2026-09-17
updated: 2026-09-18
---
## Contract

`PCI_MAP_WINDOW` 113 takes handle, user VA, BAR index, protection, offset and
length in x0..x5. Offset and length are page aligned, length is nonzero, and the
range may not overlap any MSI-X table or PBA page. It returns 0 or -1.
The existing four-argument `PCI_MAP_BAR` retains its register contract and
fails when mapping the whole BAR would expose protected pages.

`PCI_WINDOWS` 114 takes handle, output pointer and record capacity in x0..x2.
It returns the complete count (0..8) or -1. Short capacity fails. Each 24-byte
record contains u64 offset at 0, u64 length at 8, u32 BAR at 16, and zero u32
reserved at 20. Extents describe the page-rounded BAR mapping, not new physical
geometry; `PCI_INFO` is still the unchanged 256-byte topology record.

Every mapping path, including hostmem aliases, applies the exclusion. Window
records carry no authority independently of the caller's owned PCI handle.

## Verification and scope

Kernel and affected Rust drivers compile; guest malformed-layout, protected
window, hostmem-alias and mapped-function-lifetime tests pass. This foundation
does not implement interrupt endpoints, enable MSI-X, or close the INTx storm.
Linux clock_gettime 113 is consumed by the existing Tier-2 phenotype translation;
its collision argument is explicit rather than above-ceiling.
