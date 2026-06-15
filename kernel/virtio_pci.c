// VirtIO PCI transport — ECAM enumeration (P4-H).
//
// Per `docs/reference/37-virtio_pci.md`. Probes DTB for the PCIe root
// complex (compatible = "pci-host-ecam-generic"), maps bus 0's config
// space via mmu_map_mmio, walks (dev, fn) on the bus, and populates
// g_virtio_pci_devs[] with every VirtIO vendor/device-ID match.
//
// Scope at v1.0 P4-H:
//   - Bus 0 enumeration only. PCIe bridges (Type 1 headers + secondary
//     bus walking) deferred until a chunk introduces a real bridge.
//     QEMU virt's basic config has no bridges; all VirtIO PCI devices
//     live on bus 0.
//   - BAR mapping + PCI capability walking deferred to the first
//     PCI-using driver chunk. VirtIO PCI capabilities (VIRTIO_PCI_CAP_*
//     per VIRTIO 1.2 §4.1.4) carry the common-cfg / notify / ISR /
//     device-cfg region locations; mapping each requires per-driver
//     decisions (which BAR + which offset).
//   - MSI-X enable deferred. Legacy INTx routing via the DTB
//     interrupt-map deferred too — irqfwd (P4-G) currently handles
//     INTID-based GIC IRQs; PCI INTx → GIC INTID mapping lands when
//     a driver actually needs IRQ delivery.

#include <thylacine/virtio_pci.h>

#include <thylacine/dtb.h>
#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/types.h>
#include <thylacine/virtio.h>

#include "../arch/arm64/mmu.h"
#include "../arch/arm64/uart.h"

// =============================================================================
// State.
// =============================================================================

// ECAM base + size, populated by virtio_pci_init from DTB.
static paddr_t g_ecam_pa;
static size_t  g_ecam_size;
static void   *g_ecam_kva;            // KVA of bus 0's config space (1 MiB)
static bool    g_ecam_mapped;
static bool    g_init_called;

// Discovered VirtIO PCI devices.
static struct virtio_pci_dev g_virtio_pci_devs[VIRTIO_PCI_MAX_DEVS];
static u32                   g_virtio_pci_dev_count;

// QEMU virt populates only bus 0 in its basic config (no PCIe bridges).
// Mapping just bus 0 keeps the vmalloc footprint at 1 MiB (256 pages)
// out of the available 2 MiB (512 pages) — leaves ~80 page headroom
// after the prior boot consumers (UART, GIC, VirtIO MMIO).
#define VIRTIO_PCI_BUSES_MAPPED    1u
#define VIRTIO_PCI_BUS_CFG_SIZE    0x100000ull   // 1 MiB per bus

// PCI config space offset within ECAM for (bus, dev, fn):
//   offset = (bus << 20) | (dev << 15) | (fn << 12)
// Per PCI Express Base spec §7.2.2.
static inline u64 pci_cfg_offset(u8 bus, u8 dev, u8 fn) {
    return ((u64)bus << 20) | ((u64)dev << 15) | ((u64)fn << 12);
}

// Compute the function's config-space KVA. Returns NULL if (bus, dev, fn)
// is outside the mapped region (we map bus 0 only at v1.0).
static void *pci_cfg_kva(u8 bus, u8 dev, u8 fn) {
    if (!g_ecam_mapped)             return NULL;
    if (bus >= VIRTIO_PCI_BUSES_MAPPED) return NULL;
    if (dev >= 32)                  return NULL;
    if (fn >= 8)                    return NULL;
    return (u8 *)g_ecam_kva + pci_cfg_offset(bus, dev, fn);
}

// =============================================================================
// MMIO accessors. Config space is Device-nGnRnE per ARM ARM and PCI
// Express spec; volatile loads/stores defeat compiler reordering.
// =============================================================================

static inline u8 mmio_read8(const void *p) {
    return *(const volatile u8 *)p;
}
static inline u16 mmio_read16(const void *p) {
    return *(const volatile u16 *)p;
}
static inline u32 mmio_read32(const void *p) {
    return *(const volatile u32 *)p;
}

static inline void mmio_write8 (void *p, u8  v) { *(volatile u8  *)p = v; }
static inline void mmio_write16(void *p, u16 v) { *(volatile u16 *)p = v; }
static inline void mmio_write32(void *p, u32 v) { *(volatile u32 *)p = v; }

u8 virtio_pci_cfg_read8(const struct virtio_pci_dev *d, u32 off) {
    if (!d || !d->cfg)      return 0xFFu;
    if (off >= 4096)        return 0xFFu;
    return mmio_read8((const u8 *)d->cfg + off);
}

u16 virtio_pci_cfg_read16(const struct virtio_pci_dev *d, u32 off) {
    if (!d || !d->cfg)      return 0xFFFFu;
    if (off >= 4096)        return 0xFFFFu;
    if (off & 1)            return 0xFFFFu;          // alignment
    return mmio_read16((const u8 *)d->cfg + off);
}

u32 virtio_pci_cfg_read32(const struct virtio_pci_dev *d, u32 off) {
    if (!d || !d->cfg)      return 0xFFFFFFFFu;
    if (off >= 4096)        return 0xFFFFFFFFu;
    if (off & 3)            return 0xFFFFFFFFu;
    return mmio_read32((const u8 *)d->cfg + off);
}

void virtio_pci_cfg_write8(const struct virtio_pci_dev *d, u32 off, u8 val) {
    if (!d || !d->cfg)      return;
    if (off >= 4096)        return;
    mmio_write8((u8 *)d->cfg + off, val);
}

void virtio_pci_cfg_write16(const struct virtio_pci_dev *d, u32 off, u16 val) {
    if (!d || !d->cfg)      return;
    if (off >= 4096)        return;
    if (off & 1)            return;
    mmio_write16((u8 *)d->cfg + off, val);
}

void virtio_pci_cfg_write32(const struct virtio_pci_dev *d, u32 off, u32 val) {
    if (!d || !d->cfg)      return;
    if (off >= 4096)        return;
    if (off & 3)            return;
    mmio_write32((u8 *)d->cfg + off, val);
}

// =============================================================================
// Enumeration helpers.
// =============================================================================

// True if (vendor_id, device_id) is in the VirtIO PCI range.
static bool is_virtio_pci(u16 vendor_id, u16 device_id, bool *out_modern) {
    if (vendor_id != VIRTIO_PCI_VENDOR_ID) {
        *out_modern = false;
        return false;
    }
    if (device_id >= VIRTIO_PCI_DEVICE_ID_MODERN_MIN &&
        device_id <= VIRTIO_PCI_DEVICE_ID_MODERN_MAX) {
        *out_modern = true;
        return true;
    }
    if (device_id >= VIRTIO_PCI_DEVICE_ID_LEGACY_MIN &&
        device_id <= VIRTIO_PCI_DEVICE_ID_LEGACY_MAX) {
        *out_modern = false;
        return true;
    }
    *out_modern = false;
    return false;
}

// Derive the canonical VIRTIO_DEVICE_ID_* from a PCI device_id.
//
// Modern (1040..107F): device_id - 0x1040 maps to virtio_device_id
// (VIRTIO 1.2 §4.1.2.1).
//
// Legacy (1000..103F): subsystem device ID carries the virtio_device_id
// per the pre-1.0 spec. Reading subsys requires another config-space
// load; do it here so the per-dev struct is self-contained.
static u16 derive_virtio_device_id(u16 device_id, void *cfg_kva) {
    if (device_id >= VIRTIO_PCI_DEVICE_ID_MODERN_MIN &&
        device_id <= VIRTIO_PCI_DEVICE_ID_MODERN_MAX) {
        return device_id - VIRTIO_PCI_DEVICE_ID_MODERN_MIN;
    }
    // Legacy: subsystem device ID. (Bounds-checked: cfg_kva is the 4 KiB
    // function config; PCI_CFG_SUBSYS_DEVICE = 0x2e is well within.)
    return mmio_read16((const u8 *)cfg_kva + PCI_CFG_SUBSYS_DEVICE);
}

// Probe one (bus, dev, fn). If device present and VirtIO, record it.
// Returns 1 if a virtio device was recorded, 0 otherwise.
// Sets *out_header_type so the caller can detect multifunction on fn=0.
static int probe_function(u8 bus, u8 dev, u8 fn, u8 *out_header_type) {
    void *cfg = pci_cfg_kva(bus, dev, fn);
    if (!cfg) return 0;

    u16 vendor_id = mmio_read16((const u8 *)cfg + PCI_CFG_VENDOR_ID);
    if (vendor_id == PCI_VENDOR_ABSENT) {
        if (out_header_type) *out_header_type = 0;
        return 0;
    }

    u16 device_id   = mmio_read16((const u8 *)cfg + PCI_CFG_DEVICE_ID);
    u8  header_type = mmio_read8 ((const u8 *)cfg + PCI_CFG_HEADER_TYPE);
    if (out_header_type) *out_header_type = header_type;

    bool is_modern = false;
    if (!is_virtio_pci(vendor_id, device_id, &is_modern)) {
        return 0;       // not a VirtIO device — skip
    }

    if (g_virtio_pci_dev_count >= VIRTIO_PCI_MAX_DEVS) {
        // Hit storage limit. Log + return so the caller can surface it
        // in the banner; do NOT extinct (operator may legitimately have
        // a VM with many VirtIO devices and prefer truncation).
        return 0;
    }

    struct virtio_pci_dev *d = &g_virtio_pci_devs[g_virtio_pci_dev_count++];
    d->bus               = bus;
    d->dev               = dev;
    d->fn                = fn;
    d->header_type       = header_type & PCI_HEADER_TYPE_MASK;
    d->vendor_id         = vendor_id;
    d->device_id         = device_id;
    d->subsys_vendor     = mmio_read16((const u8 *)cfg + PCI_CFG_SUBSYS_VENDOR);
    d->subsys_device     = mmio_read16((const u8 *)cfg + PCI_CFG_SUBSYS_DEVICE);
    d->class_code        = mmio_read8 ((const u8 *)cfg + PCI_CFG_CLASS_CODE);
    d->subclass          = mmio_read8 ((const u8 *)cfg + PCI_CFG_SUBCLASS);
    d->prog_if           = mmio_read8 ((const u8 *)cfg + PCI_CFG_PROG_IF);
    d->is_modern         = is_modern;
    d->virtio_device_id  = derive_virtio_device_id(device_id, cfg);
    d->cfg               = cfg;
    return 1;
}

// =============================================================================
// Public API.
// =============================================================================

void virtio_pci_init(void) {
    if (g_init_called) extinction("virtio_pci_init: double call");
    g_init_called = true;

    u64 ecam_base = 0, ecam_size = 0;
    if (!dtb_get_compat_reg("pci-host-ecam-generic", &ecam_base, &ecam_size)) {
        // No PCIe root complex in DTB. Not all targets have PCI (bare
        // metal Pi 5 boot path, future microvm targets); silent skip.
        uart_puts("  virtio_pci: no PCIe root complex in DTB (skip)\n");
        return;
    }
    g_ecam_pa   = (paddr_t)ecam_base;
    g_ecam_size = (size_t)ecam_size;

    // Map just bus 0's 1 MiB of config space (32 dev × 8 fn × 4 KiB).
    // Full ECAM mapping (256 MiB) would exhaust the 4 MiB MMIO vmalloc window;
    // bus 0 mapping suffices for QEMU virt's no-bridge config. v1.x
    // can extend to multi-bus by adding an L2 block mapping or a
    // dedicated PCI config aperture.
    void *kva = mmu_map_mmio(g_ecam_pa, VIRTIO_PCI_BUS_CFG_SIZE);
    if (!kva) {
        extinction("virtio_pci_init: mmu_map_mmio returned NULL "
                   "(vmalloc exhausted?)");
    }
    g_ecam_kva    = kva;
    g_ecam_mapped = true;

    // Walk bus 0. For each dev: probe fn 0; if multifunction bit set,
    // probe fns 1..7. PCI requires fn 0 to be present if any fn on
    // the device responds, so this is safe (devices without fn 0
    // return 0xFFFF on read, terminating the dev walk).
    u32 probed_funcs = 0;
    for (u8 dev_idx = 0; dev_idx < 32; dev_idx++) {
        u8 header_type = 0;
        (void)probe_function(0, dev_idx, 0, &header_type);
        probed_funcs++;
        if (header_type & PCI_HEADER_TYPE_MF) {
            for (u8 fn = 1; fn < 8; fn++) {
                u8 _unused;
                (void)probe_function(0, dev_idx, fn, &_unused);
                probed_funcs++;
            }
        }
    }

    // Boot banner. Keep the format symmetric with virtio.c's banner.
    uart_puts("  virtio_pci: ECAM at PA ");
    uart_puthex64((u64)g_ecam_pa);
    uart_puts(" size ");
    uart_puthex64((u64)g_ecam_size);
    uart_puts(" (");
    uart_putdec((u64)g_virtio_pci_dev_count);
    uart_puts(" VirtIO devices on bus 0, ");
    uart_putdec((u64)probed_funcs);
    uart_puts(" functions probed)\n");
}

int virtio_pci_dev_count(void) {
    return (int)g_virtio_pci_dev_count;
}

struct virtio_pci_dev *virtio_pci_dev_get(int idx) {
    if (idx < 0)                              return NULL;
    if ((u32)idx >= g_virtio_pci_dev_count)   return NULL;
    return &g_virtio_pci_devs[idx];
}

struct virtio_pci_dev *virtio_pci_find_by_device_id(u32 virtio_device_id) {
    for (u32 i = 0; i < g_virtio_pci_dev_count; i++) {
        if (g_virtio_pci_devs[i].virtio_device_id == (u16)virtio_device_id) {
            return &g_virtio_pci_devs[i];
        }
    }
    return NULL;
}
