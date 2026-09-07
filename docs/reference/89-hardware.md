# 89 — t::hardware (libthyla-rs typed RAII over hardware handles) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-hardware-doc-absorb`).
`libthyla_rs::hardware` — typed RAII wrappers over the kernel's hardware-handle
surfaces: `Mmio` (KObj_MMIO), `Irq` (KObj_IRQ), `Dma` (KObj_DMA), and `PciDev`
(the virtio-PCI transport). Its content lives, code-verified and current, in:

- the **runtime wrappers + the two invariants-by-absence + the ISV-safe MMIO
  accessor** — `Mmio`/`Irq`/`Dma`/`PciDev`, the I-5 non-transferability preserved
  *by having no transfer method* (and `!Send`/`!Sync` for free from the raw
  pointers), and the **#890 ISV-safe accessor folded here at this absorption**
  (`mmio_read32`/`write32` must be a single base-only instruction so `ESR_EL1.ISV=1`
  and HVF can decode the emulated access — a plain `read_volatile` can inline to a
  writeback/unscaled form with ISV=0 that trips HVF's `assert(isv)`):

      vault/system/userspace/runtime/sub-libthyla-rs.md   (audit: light, inv-i5)

- the **driver consumers + the PciDev findings** — the virtio NIC's use of
  `PciDev`, the pci-3 F1 (partial-map leak, no v1.0 detach) and F2 (notify-doorbell
  bound), and the region-bounds-the-region-not-the-field-offsets guard:

      vault/system/userspace/services/sub-netd-nic.md
      vault/system/userspace/hardware/moc-userspace-hardware.md

- the **kernel-side handle surfaces** — the I-5 static transfer reject and the
  Drop-doesn't-unmap Burrow-independent-refcount lifetime:

      vault/system/kernel/security/sub-kernel-handle.md
      vault/system/kernel/memory/sub-kernel-burrow.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **The ISV-safe MMIO accessor finding (#890) was uncovered — folded at
  absorption.** sub-libthyla-rs carried the wrappers and the I-5-by-absence
  property, but not the hard-won HVF-portability finding: the userspace MMIO
  accessor must be a single base-only `ldr`/`str` (ISV=1), because a plain
  `read_volatile` folds to a pre-indexed/unscaled form (ISV=0) under
  `#[inline(always)]` and trips HVF's `assert(isv)` — the signature being that
  kernel virtio worked under HVF while userspace tripped. The *GIC-side* ISV issue
  (Lazarus W2, GICv2-not-GICv3) is a distinct manifestation in sub-substrate-
  machine; this is the driver-accessor one, now in sub-libthyla-rs.
- **The rest is current and home** — the DMA normal-memory + `virtio_rmb`-is-the-
  caller's barrier discipline, the bounds/alignment panics, the error-mapping, and
  the PciDev shm_region/partial-map details are all carried by the runtime + driver
  dossiers. Zero code change.
