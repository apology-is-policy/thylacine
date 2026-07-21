// VirtIO PCI transport — ECAM enumeration (P4-H).
//
// Per ARCHITECTURE.md §13 + ROADMAP §6.1 + PCI Local Bus spec (rev 3.0)
// + PCI Express Base spec § 7 (ECAM) + VIRTIO 1.2 §4.1 (PCI transport).
//
// QEMU virt exposes a PCIe root complex at the DTB node
// `compatible = "pci-host-ecam-generic"`. The ECAM range (1 MiB per bus
// × 256 buses = 256 MiB at PA 0x40_1000_0000 on QEMU virt) is the only
// access path to PCI configuration space — there's no I/O port hole and
// no legacy CF8/CFC mechanism.
//
// v1.0 P4-H scope: enumerate bus 0 (the only bus actually populated in
// QEMU virt without bridges), identify VirtIO-PCI devices by vendor ID
// 0x1AF4 + device ID range, expose them through a virtio_mmio_dev-like
// API. BAR mapping and PCI capability walking (VIRTIO_PCI_CAP_*) are
// deferred to the first PCI-using driver chunk (likely P4-L virtio-gpu,
// since GPU is PCI-only on QEMU virt per ARCH §13).
//
// Naming: we keep canonical PCI terminology ("ECAM", "BDF") because this
// is the wire-protocol layer — the spec is the contract.

#ifndef THYLACINE_VIRTIO_PCI_H
#define THYLACINE_VIRTIO_PCI_H

#include <thylacine/types.h>

// =============================================================================
// PCI configuration space header — common Type 0 fields (PCI Local Bus 6.1).
// =============================================================================

#define PCI_CFG_VENDOR_ID       0x00  // R   u16; 0xFFFF if no device
#define PCI_CFG_DEVICE_ID       0x02  // R   u16
#define PCI_CFG_COMMAND         0x04  // RW  u16; bit 0 IO space, 1 MEM space, 2 bus master
#define PCI_CFG_STATUS          0x06  // RW  u16
#define PCI_CFG_REVISION_ID     0x08  // R   u8
#define PCI_CFG_PROG_IF         0x09  // R   u8
#define PCI_CFG_SUBCLASS        0x0a  // R   u8
#define PCI_CFG_CLASS_CODE      0x0b  // R   u8
#define PCI_CFG_CACHE_LINE_SIZE 0x0c  // RW  u8
#define PCI_CFG_LATENCY_TIMER   0x0d  // RW  u8
#define PCI_CFG_HEADER_TYPE     0x0e  // R   u8; bit 7 = multifunction
#define PCI_CFG_BIST            0x0f  // RW  u8
#define PCI_CFG_BAR0            0x10  // RW  u32; Type 0 has 6 BARs (0x10..0x24)
#define PCI_CFG_BAR1            0x14
#define PCI_CFG_BAR2            0x18
#define PCI_CFG_BAR3            0x1c
#define PCI_CFG_BAR4            0x20
#define PCI_CFG_BAR5            0x24
#define PCI_CFG_SUBSYS_VENDOR   0x2c  // R   u16
#define PCI_CFG_SUBSYS_DEVICE   0x2e  // R   u16
#define PCI_CFG_CAP_PTR         0x34  // R   u8; offset of first capability
#define PCI_CFG_INT_LINE        0x3c  // RW  u8
#define PCI_CFG_INT_PIN         0x3d  // R   u8

// Vendor ID 0xFFFF means no device at this BDF.
#define PCI_VENDOR_ABSENT       0xFFFFu

// PCI_CFG_HEADER_TYPE bit 7 = multifunction (walk all 8 functions).
#define PCI_HEADER_TYPE_MF      0x80u
// Low 7 bits encode header layout: 0 = standard Type 0; 1 = PCI-to-PCI
// bridge; 2 = CardBus bridge. v1.0 only handles Type 0 (endpoints).
#define PCI_HEADER_TYPE_MASK    0x7Fu
#define PCI_HEADER_TYPE_NORMAL  0x00u

// =============================================================================
// VirtIO-PCI vendor + device ID ranges — VIRTIO 1.2 §4.1.2.
// =============================================================================

#define VIRTIO_PCI_VENDOR_ID            0x1AF4u  // Red Hat / Qumranet

// Modern VirtIO PCI devices use device ID 0x1040 + virtio_device_id.
// Legacy (pre-1.0 spec) devices use 0x1000..0x103F with subsystem
// device ID carrying the virtio_device_id. v1.0 P4-H identifies both
// ranges but defers legacy-quirk handling to drivers.
#define VIRTIO_PCI_DEVICE_ID_MODERN_MIN 0x1040u
#define VIRTIO_PCI_DEVICE_ID_MODERN_MAX 0x107Fu
#define VIRTIO_PCI_DEVICE_ID_LEGACY_MIN 0x1000u
#define VIRTIO_PCI_DEVICE_ID_LEGACY_MAX 0x103Fu

// =============================================================================
// Public types.
// =============================================================================

// Max VirtIO-PCI devices we track at v1.0. QEMU virt's PCIe surface
// realistically holds 4-8 devices in practice; 16 leaves headroom
// for VirtIO-net + virtio-blk + virtio-gpu + virtio-input + spare.
#define VIRTIO_PCI_MAX_DEVS  16u

// Per-VirtIO-PCI-device handle. Populated by virtio_pci_init at boot;
// immutable thereafter except for the BAR mappings the eventual driver
// installs (v1.0 P4-H leaves BARs unmapped — the driver is responsible
// for mapping the BARs it needs).
struct virtio_pci_dev {
    u8       bus;
    u8       dev;             // device (0..31)
    u8       fn;              // function (0..7)
    u8       header_type;     // PCI_CFG_HEADER_TYPE (low 7 bits)
    u16      vendor_id;       // VIRTIO_PCI_VENDOR_ID
    u16      device_id;       // 0x1000..0x103F (legacy) or 0x1040..0x107F (modern)
    u16      virtio_device_id;// VIRTIO_DEVICE_ID_*; derived per spec
    u16      subsys_vendor;
    u16      subsys_device;
    u8       class_code;
    u8       subclass;
    u8       prog_if;
    bool     is_modern;       // device_id in [0x1040, 0x107F]
    void    *cfg;             // KVA of this function's 4 KiB config space
};

// =============================================================================
// API.
// =============================================================================

// Bring up VirtIO-PCI. Probes DTB for "pci-host-ecam-generic", maps
// bus 0's 1 MiB of config space via mmu_map_mmio, walks (dev, fn) for
// each populated slot, and populates g_virtio_pci_devs[] with every
// matching VirtIO vendor/device-ID combination. Non-VirtIO PCI devices
// are skipped (we don't drive them at v1.0). Idempotent guard extincts
// on re-call. Safe to call when no PCIe root is present in the DTB (no
// devices populated, count = 0).
//
// Must run after slub_init + virtio_init — slub_init for any KVA
// metadata storage; virtio_init because we share the boot banner block.
void virtio_pci_init(void);

// Number of VirtIO-PCI devices identified during enumeration (0 if no
// ECAM in DTB, or no VirtIO devices on the bus).
int virtio_pci_dev_count(void);

// Get the idx-th VirtIO-PCI device. Returns NULL if idx out of range.
struct virtio_pci_dev *virtio_pci_dev_get(int idx);

// Find the first VirtIO-PCI device matching the given virtio_device_id
// (VIRTIO_DEVICE_ID_BLOCK / _NET / _GPU / _INPUT / etc.). Returns NULL
// if no match.
struct virtio_pci_dev *virtio_pci_find_by_device_id(u32 virtio_device_id, u32 nth);

// PCI config-space read helpers. Take a BDF triple + offset; offset
// MUST be within [0, 4096) and naturally aligned to the access width.
// Returns 0xFF / 0xFFFF / 0xFFFFFFFF on out-of-range. Loads are
// volatile to defeat compiler reordering (config space is MMIO).
u8  virtio_pci_cfg_read8 (const struct virtio_pci_dev *d, u32 off);
u16 virtio_pci_cfg_read16(const struct virtio_pci_dev *d, u32 off);
u32 virtio_pci_cfg_read32(const struct virtio_pci_dev *d, u32 off);

// PCI config-space WRITE helpers (pci-1a) -- mirror the reads. Same
// bounds: offset within [0, 4096) and naturally aligned to the access
// width; a misaligned or out-of-range write is a silent no-op. The kernel
// drives these only from the pci-1b claim path (BAR sizing + assignment +
// command-register enable); config space is never mapped to userspace.
void virtio_pci_cfg_write8 (const struct virtio_pci_dev *d, u32 off, u8  val);
void virtio_pci_cfg_write16(const struct virtio_pci_dev *d, u32 off, u16 val);
void virtio_pci_cfg_write32(const struct virtio_pci_dev *d, u32 off, u32 val);

#endif  // THYLACINE_VIRTIO_PCI_H
