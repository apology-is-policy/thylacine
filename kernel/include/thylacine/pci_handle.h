// KObj_PCI — a claimed VirtIO-PCI function as a handle-table-managed kernel
// object (pci-1b, the virtio-PCI transport).
//
// Per docs/VIRTIO-PCI-DESIGN.md + ARCHITECTURE.md §13.2 + §28 I-5 (KObj_PCI
// non-transferable). A userspace driver process holds a KObj_PCI handle naming
// a PCI function (bus/dev/fn) it exclusively owns. The kernel guarantees:
//
//   1. Exclusivity: at most one alive KObj_PCI per (bus, dev, fn) — pinned by
//      the g_pci_claims table in kobj_pci_claim (the PCI analog of KObj_MMIO's
//      HwResourceExclusive). The per-BAR PA ranges the claim assigns inherit
//      KObj_MMIO's own overlap exclusivity.
//
//   2. Non-transferability (I-5): KOBJ_PCI joins KOBJ_KIND_HW_MASK, so the 9P
//      transfer path structurally has no KOBJ_PCI case and handle_dup rejects
//      it (NoHwDup) — both for free, no per-kind code.
//
//   3. Capability-gated creation: SYS_PCI_CLAIM (pci-1c) checks CAP_HW_CREATE
//      before kobj_pci_claim, exactly like SYS_MMIO_CREATE.
//
// Why PCI rather than the virtio-MMIO bank: QEMU packs 8 virtio-mmio slots per
// 4 KiB page (0x200 stride), so two persistent userspace drivers (netd + the
// Stratum-pool stratumd) cannot share a page under the hard 4 KiB MMU granule —
// the page-exclusive KObj_MMIO claim cannot isolate sub-page (#140). On PCIe
// each function carries its own page-aligned BAR, so the existing page-exclusive
// claim works with real per-device isolation. See docs/VIRTIO-PCI-DESIGN.md.
//
// The kernel owns ECAM config space (bus-wide, un-isolatable — already mapped +
// enumerated by kernel/virtio_pci.c at boot); KObj_PCI mediates config space and
// hands out BAR mappings (via kobj_pci_bar_mmio + the pci-1c SYS_PCI_MAP_BAR
// burrow path). The kernel ASSIGNS BARs (bare boot, no UEFI/firmware) linearly
// from the host-bridge MMIO window (dtb_pci_mem_window).

#ifndef THYLACINE_PCI_HANDLE_H
#define THYLACINE_PCI_HANDLE_H

#include <thylacine/types.h>

struct virtio_pci_dev;          // <thylacine/virtio_pci.h>
struct KObj_MMIO;               // <thylacine/mmio_handle.h>

// KOBJ_PCI_MAGIC — sentinel at offset 0; checked at every public entry. SLUB
// freelist write on free clobbers it (UAF defense, mirrors KObj_MMIO).
#define KOBJ_PCI_MAGIC 0x504349000BADC0DEULL    // 'PCI' | 0BADC0DE

// Type-0 PCI header has 6 BARs at config 0x10..0x24.
#define PCI_BAR_COUNT 6u

// VIRTIO_PCI_CAP cfg_type (VIRTIO 1.2 §4.1.4.1) — the four structure kinds we
// resolve from the capability list into regions[], indexed by (cfg_type - 1).
#define VIRTIO_PCI_CAP_COMMON_CFG   1u
#define VIRTIO_PCI_CAP_NOTIFY_CFG   2u
#define VIRTIO_PCI_CAP_ISR_CFG      3u
#define VIRTIO_PCI_CAP_DEVICE_CFG   4u
#define VIRTIO_PCI_CAP_REGION_COUNT 4u

// PCI BAR low-bit decode (PCI Local Bus 6.2.5.1). Bit 0: 0 = memory BAR, 1 =
// I/O BAR. For memory BARs, bits[2:1] = type (00 = 32-bit, 10 = 64-bit), bit 3 =
// prefetchable. The low 4 bits are read-only attribute bits; the size mask
// covers the address bits above them.
#define PCI_BAR_IO          0x1u
#define PCI_BAR_TYPE_MASK   0x6u
#define PCI_BAR_TYPE_64     0x4u
#define PCI_BAR_ATTR_MASK   0xFu

// PCI command register bits (PCI Local Bus 6.2.2).
#define PCI_CMD_MEM_SPACE   (1u << 1)
#define PCI_CMD_BUS_MASTER  (1u << 2)

// PCI status register: bit 4 = capabilities list present.
#define PCI_STATUS_CAP_LIST (1u << 4)

// PCI capability ID for a vendor-specific capability (VIRTIO_PCI_CAP_* live in
// vendor caps per VIRTIO 1.2 §4.1.4).
#define PCI_CAP_ID_VNDR     0x09u

// Legacy INTx pin: 1 = INTA (.. 4 = INTD). v1.0 single-function devices raise
// INTA; dtb_pci_intx_route swizzles it to a GIC SPI INTID.
#define PCI_INT_PIN_INTA    1u

// An assigned memory BAR.
struct pci_bar {
    u64               pa;        // assigned CPU PA (page-aligned); 0 if !present
    u64               size;      // the BAR's DECODED size (power of 2); a region
                                 // must fit within it (user maps round_up to page)
    struct KObj_MMIO *mmio;      // the exclusive PA-range claim; NULL if !present
    bool              present;   // an implemented, assigned MEM BAR
    bool              is_64;     // 64-bit BAR (consumes the following slot too)
};

// A resolved VIRTIO_PCI_CAP region (common / notify / isr / device config).
struct pci_region {
    bool present;
    u8   bar;        // which BAR holds it (< PCI_BAR_COUNT, present)
    u32  offset;     // byte offset within the BAR
    u32  length;     // byte length (offset + length <= bars[bar].size)
};

struct KObj_PCI {
    u64 magic;       // KOBJ_PCI_MAGIC
    int ref;         // refcount; starts at 1 from kobj_pci_claim

    u8  bus;
    u8  dev;
    u8  fn;
    u16 virtio_device_id;

    // Stable pointer into g_virtio_pci_devs[] (immutable after boot
    // enumeration); used for config-space access (BAR sizing / assignment /
    // command-register enable + quiesce) over the kernel-owned ECAM mapping.
    struct virtio_pci_dev *vpd;

    struct pci_bar    bars[PCI_BAR_COUNT];
    struct pci_region regions[VIRTIO_PCI_CAP_REGION_COUNT];   // (cfg_type - 1)
    u32 notify_off_multiplier;   // from the NOTIFY_CFG cap

    u32  intid;        // GIC INTID from dtb_pci_intx_route(dev, INTA)
    bool intid_valid;  // false if the DTB interrupt-map didn't resolve
};

// Bring up the PCI-handle subsystem. Idempotent guard extincts on re-call.
// Must run after virtio_pci_init (it claims from g_virtio_pci_devs[]).
void kobj_pci_init(void);

// Claim the first VirtIO-PCI device matching virtio_device_id. On success:
// assigns + enables the function's memory BARs (from the dtb_pci_mem_window
// bump arena), walks the VIRTIO_PCI_CAP_* capability list into regions[],
// resolves the INTx GIC INTID, and enables MEM-decode + bus-master. Returns
// NULL on: subsystem-not-inited / device-not-found / already-claimed / BAR
// assignment failure (window exhausted, OOM, PA-overlap) / malformed cap list
// (out-of-range BAR index, region exceeding the decoded BAR size, capability
// loop) / SLUB OOM / claim-table full. Every partial side effect is rolled
// back on failure (the device is left quiesced + re-claimable).
//
// On success the caller owns the refcount=1 reference; balance with
// kobj_pci_unref. The (bus,dev,fn) exclusivity + the per-BAR PA claims persist
// until the last unref.
struct KObj_PCI *kobj_pci_claim(u32 virtio_device_id);

// Refcount ops. Mirror kobj_mmio_ref / kobj_mmio_unref.
void kobj_pci_ref(struct KObj_PCI *k);

// Decrement ref. The last unref QUIESCES the device (clears MEM-decode +
// bus-master so it stops DMA / decoding before its BAR PA claims are released),
// drops each assigned BAR's KObj_MMIO ref (a live user mapping's independent
// burrow ref keeps that BAR's pages alive — the #847 dual lifetime — until the
// mapping tears down), releases the (bus,dev,fn) slot, and kfrees. NULL-safe.
void kobj_pci_unref(struct KObj_PCI *k);

// Resolve a BAR index to its KObj_MMIO (the pci-1c SYS_PCI_MAP_BAR handler
// passes it to burrow_create_mmio, which takes its own ref). Returns NULL on
// out-of-range index or an absent BAR. Does NOT bump the ref — the caller holds
// the owning KObj_PCI alive (via handle_get) for the call's duration.
struct KObj_MMIO *kobj_pci_bar_mmio(struct KObj_PCI *k, u32 bar_index);

// Decode a memory BAR's size from the all-ones-probe readback (low-dword mask
// with attribute bits cleared + high-dword readback + the 64-bit flag). Exposed
// for a deterministic unit test that pins the width-correct inversion (a 32-bit
// BAR's mask is inverted in 32-bit width). Returns 0 for an unimplemented /
// full-width BAR.
u64 pci_bar_decode_size(u32 lo_mask, u32 hi_rb, bool is64);

// Resolve the VIRTIO_PCI_CAP_* regions from d's config-space capability list
// into k->regions[], validating each region's BAR index + extent against the
// assigned BAR sizes. Returns 0 (resolved + validated) or -1 (malformed: cap
// loop / out-of-range BAR / unassigned BAR / region past the BAR size). Exposed
// for a deterministic hostile-cap-layout unit test over a synthetic config.
int pci_walk_caps(struct KObj_PCI *k, struct virtio_pci_dev *d);

// Diagnostics — cumulative claim counter + currently-live count.
u64 kobj_pci_total_created(void);
u64 kobj_pci_live_count(void);

#endif  // THYLACINE_PCI_HANDLE_H
