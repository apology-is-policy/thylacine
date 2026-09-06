---
id: chg-2026-09-06-hardware-doc-absorb
type: chg
title: "absorb docs/reference/89-hardware (t::hardware RAII wrappers): fold the ISV-safe MMIO accessor finding (#890) into sub-libthyla-rs, multi-redirect stub"
date: 2026-09-06
arc: arc-vault
commits: ["PENDING"]
touched: [sub-libthyla-rs]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-06
---
libthyla_rs::hardware -- typed RAII over Mmio/Irq/Dma/PciDev. Verified atom-by-atom.

ALREADY COVERED (verified, not assumed):
- Mmio/Irq/Dma + the I-5 non-transferability-by-absence (no transfer method;
  !Send/!Sync from raw pointers) -> sub-libthyla-rs (:214-216, :281, :355).
- The PciDev findings (pci-3 F1 partial-map leak, F2 notify-doorbell bound, the
  region-bounds-the-region-not-the-field-offsets guard) + the NIC consumer ->
  sub-netd-nic + moc-userspace-hardware.
- The I-5 static transfer reject + Drop-doesn't-unmap (Burrow independent refcount)
  -> sub-kernel-handle + sub-kernel-burrow.

THE FOLD (the atom that lived only in the doc):
The ISV-safe MMIO accessor finding (#890). sub-libthyla-rs carried the wrappers +
I-5-by-absence but NOT the HVF-portability finding: mmio_read32/write32 must be a
single BASE-ONLY instruction so ESR_EL1.ISV=1 and HVF can decode the emulated
access -- a plain read_volatile folds to a pre-indexed/unscaled form (ISV=0) under
#[inline(always)] and trips HVF's assert(isv). Signature: kernel virtio worked
under HVF while userspace tripped (kernel out-of-line accessors stayed base-only).
NOTE the discrimination: sub-substrate-machine covers the GIC-side ISV issue
(Lazarus W2, GICv2-not-GICv3) -- a DIFFERENT manifestation; #890 in
sub-substrate-machine is the tickless-idle work, an overloaded number. This is the
DRIVER-ACCESSOR ISV finding. Folded into sub-libthyla-rs as a Mechanism subsection.

NOT REFUTED: the DMA barrier discipline, bounds/alignment panics, error-mapping all
current. Zero code change. Multi-redirect stub.
