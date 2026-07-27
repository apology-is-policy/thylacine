// KObj_PCI impl (pci-1b) — claim a VirtIO-PCI function, assign its BARs, resolve
// its VIRTIO_PCI_CAP_* regions + INTx INTID, and hold it as a non-transferable
// hardware handle.
//
// Per <thylacine/pci_handle.h> + docs/VIRTIO-PCI-DESIGN.md. The structure mirrors
// kernel/mmio_handle.c: a static g_pci_claims table tracks every alive
// (bus,dev,fn, owner) tuple; claim scans for an existing claim before allocating;
// unref releases the slot. The per-BAR PA ranges are themselves KObj_MMIO claims,
// so BAR exclusivity rides KObj_MMIO's own overlap rejection.
//
// **Lock discipline**: g_pci_lock guards g_pci_claims AND the BAR bump arena
// (g_pci_bar_*) for all reads + writes. Acquired IRQ-safe (claim can run from
// kernel-context test code; unref runs from handle_close in process context).
// It is a LEAF lock with one exception: it is NEVER held across kobj_mmio_create
// (which takes g_mmio_lock) — pci_bar_alloc releases g_pci_lock before the
// caller calls kobj_mmio_create, so the order is g_pci_lock then (separately)
// g_mmio_lock, never nested. No cycle.

#include <thylacine/dtb.h>
#include <thylacine/extinction.h>
#include <thylacine/mmio_handle.h>
#include <thylacine/page.h>
#include <thylacine/pci_handle.h>
#include <thylacine/spinlock.h>
#include <thylacine/types.h>
#include <thylacine/virtio_pci.h>

#include "../arch/arm64/uart.h"
#include "../mm/slub.h"

// Claim table. A handful of PCI functions are claimable at v1.0 (net is the
// only one initially; blk-over-PCI is a v1.x seam). 8 leaves headroom.
#define KOBJ_PCI_MAX 8

struct pci_claim {
    struct KObj_PCI *owner;     // NULL = free
    u8               bus;
    u8               dev;
    u8               fn;
};

static struct pci_claim g_pci_claims[KOBJ_PCI_MAX];
static spin_lock_t      g_pci_lock = SPIN_LOCK_INIT;
static u64              g_pci_created;
static u64              g_pci_live;
static bool             g_pci_initialized;

// BAR bump arena: the host-bridge 32-bit MMIO window the kernel assigns BARs
// from (bare boot, no UEFI). Lazily seeded from dtb_pci_mem_window on the first
// allocation; advanced monotonically. The arena does NOT reclaim a freed BAR's
// PA range (g_mmio_claims does reclaim it — a re-claim just bump-allocates a
// fresh PA); with a ~768 MiB window and ~16 KiB BARs that is tens of thousands
// of claims of headroom, ample for v1.0. A reclaiming PA allocator is a v1.x
// item. All under g_pci_lock.
static bool g_pci_bar_inited;
static u64  g_pci_bar_base;
static u64  g_pci_bar_end;
static u64  g_pci_bar_next;

u64 kobj_pci_total_created(void) {
    return __atomic_load_n(&g_pci_created, __ATOMIC_RELAXED);
}
u64 kobj_pci_live_count(void) {
    return __atomic_load_n(&g_pci_live, __ATOMIC_RELAXED);
}

void kobj_pci_init(void) {
    if (__atomic_exchange_n(&g_pci_initialized, true, __ATOMIC_ACQ_REL)) {
        extinction("kobj_pci_init called twice");
    }
    // BSS already zeroed g_pci_claims (all-free) + the counters.
    uart_puts("kobj_pci: claims=");
    uart_putdec((u64)KOBJ_PCI_MAX);
    uart_puts(" slots\n");
}

// =============================================================================
// Claim-table helpers (caller holds g_pci_lock).
// =============================================================================

static int find_free_pci_slot(void) {
    for (int i = 0; i < KOBJ_PCI_MAX; i++) {
        if (!g_pci_claims[i].owner) return i;
    }
    return -1;
}

static int find_pci_slot_by_bdf(u8 bus, u8 dev, u8 fn) {
    for (int i = 0; i < KOBJ_PCI_MAX; i++) {
        if (g_pci_claims[i].owner &&
            g_pci_claims[i].bus == bus &&
            g_pci_claims[i].dev == dev &&
            g_pci_claims[i].fn  == fn) {
            return i;
        }
    }
    return -1;
}

static int find_pci_slot_by_owner(struct KObj_PCI *k) {
    for (int i = 0; i < KOBJ_PCI_MAX; i++) {
        if (g_pci_claims[i].owner == k) return i;
    }
    return -1;
}

// =============================================================================
// BAR bump arena.
// =============================================================================

// Allocate `align`-aligned `size` bytes of BAR PA. `size` (= the page-rounded
// BAR claim size) and `align` are powers of two; on QEMU virt they are equal
// (natural BAR alignment). Returns false if the arena is absent (no DTB window)
// or exhausted. Caller MUST NOT hold g_pci_lock — this takes it.
static bool pci_bar_alloc(u64 size, u64 *out_pa) {
    irq_state_t s = spin_lock_irqsave(&g_pci_lock);

    if (!g_pci_bar_inited) {
        u64 base = 0, len = 0;
        if (!dtb_pci_mem_window(&base, &len) || len == 0) {
            spin_unlock_irqrestore(&g_pci_lock, s);
            return false;
        }
        g_pci_bar_base = base;
        g_pci_bar_end  = base + len;       // dtb_pci_mem_window bounds this < IPS
        g_pci_bar_next = base;
        g_pci_bar_inited = true;
    }

    // Align next up to `size` (a power of two >= PAGE_SIZE).
    u64 a = (g_pci_bar_next + (size - 1)) & ~(size - 1);
    // Bounds + overflow: a >= base (alignment only advances), a + size must not
    // wrap and must fit the window.
    if (a < g_pci_bar_base || a + size < a || a + size > g_pci_bar_end) {
        spin_unlock_irqrestore(&g_pci_lock, s);
        return false;
    }
    g_pci_bar_next = a + size;
    spin_unlock_irqrestore(&g_pci_lock, s);

    *out_pa = a;
    return true;
}

// =============================================================================
// BAR sizing + assignment.
// =============================================================================

// Round a decoded BAR size up to a whole page (the granule kobj_mmio_create +
// the user mapping work at). `size` is a power of two; the result is a power of
// two (size itself when size >= PAGE_SIZE, else PAGE_SIZE).
static u64 bar_claim_size(u64 size) {
    if (size >= (u64)PAGE_SIZE) return size;
    return (u64)PAGE_SIZE;
}

// Decode a memory BAR's size from the all-ones-probe readback. `lo_mask` is the
// low dword with the 4 attribute bits already cleared; `hi_rb` is the high-dword
// readback (0 for a 32-bit BAR). Returns the size (a power of two), or 0 for an
// unimplemented BAR (mask all-zero) or a full-width (2^N) absurdity the caller
// rejects. The inversion is WIDTH-CORRECT: a 32-bit BAR's mask occupies only the
// low 32 bits and is inverted in 32-bit width -- inverting it as 64-bit would
// set the upper 32 bits and yield a multi-exabyte bogus size. Non-static +
// header-declared so a deterministic unit test pins the vectors.
u64 pci_bar_decode_size(u32 lo_mask, u32 hi_rb, bool is64) {
    if (is64) {
        u64 combined = ((u64)hi_rb << 32) | (u64)lo_mask;
        if (combined == 0) return 0;        // all 64 address bits writable (2^64)
        return ~combined + 1u;
    }
    if (lo_mask == 0) return 0;             // unimplemented / 2^32
    return (u64)(~lo_mask + 1u);            // 32-bit-width invert
}

// Probe + assign BAR slot `i`. On a present, assignable memory BAR: sizes it,
// bump-allocates a PA, programs the BAR registers, and creates the exclusive
// KObj_MMIO claim. Returns 1 (assigned; *out_is64 set), 0 (slot empty /
// unimplemented / an I/O BAR we don't map — skip), or -1 (malformed device or
// resource failure — abort the claim).
//
// Sizing dance (PCI Local Bus 6.2.5.1): with MEM-decode OFF (the command
// register stays disabled until every BAR is assigned), write all-ones, read
// back the writable address bits, invert + 1. A 64-bit BAR spans this slot
// (low) + the next (high).
static int pci_assign_one_bar(struct KObj_PCI *k, struct virtio_pci_dev *d,
                              u32 i, bool *out_is64) {
    *out_is64 = false;
    u32 off = PCI_CFG_BAR0 + 4u * i;

    u32 orig = virtio_pci_cfg_read32(d, off);

    // I/O BARs are not mappable as MMIO (virtio-modern uses memory BARs); skip.
    if (orig & PCI_BAR_IO) return 0;

    bool is64 = ((orig & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_64);
    // A 64-bit BAR needs a high half; one in the last slot is malformed.
    if (is64 && i + 1u >= PCI_BAR_COUNT) return -1;

    // Size probe (decode is off, so the transient all-ones decode is inert).
    virtio_pci_cfg_write32(d, off, 0xFFFFFFFFu);
    u32 lo_rb = virtio_pci_cfg_read32(d, off);
    u32 hi_rb = 0;
    if (is64) {
        virtio_pci_cfg_write32(d, off + 4u, 0xFFFFFFFFu);
        hi_rb = virtio_pci_cfg_read32(d, off + 4u);
    }

    u32 lo_mask = lo_rb & ~PCI_BAR_ATTR_MASK;
    // Unimplemented BAR: hardwired to 0 (writes ignored, reads 0). No restore
    // needed — the all-ones write was a no-op on the device.
    if (lo_mask == 0 && hi_rb == 0) return 0;

    u64 size = pci_bar_decode_size(lo_mask, hi_rb, is64);
    if (size == 0) return -1;           // a flagged-but-undecodable / full-width BAR

    u64 claim_size = bar_claim_size(size);

    u64 pa = 0;
    if (!pci_bar_alloc(claim_size, &pa)) return -1;   // window exhausted / no DTB

    // Program the BAR with the assigned PA. The PA is claim_size-aligned (>=
    // page), so its low attribute bits are 0; the device's read-only attribute
    // bits are unaffected by the write.
    virtio_pci_cfg_write32(d, off, (u32)(pa & 0xFFFFFFFFu));
    if (is64) {
        virtio_pci_cfg_write32(d, off + 4u, (u32)(pa >> 32));
    }

    // The exclusive PA-range claim (also the I-30/I-5 anchor for the mapping).
    struct KObj_MMIO *m = kobj_mmio_create(pa, (size_t)claim_size);
    if (!m) return -1;                  // PA overlap / OOM / table full

    k->bars[i].pa      = pa;
    k->bars[i].size    = size;          // the DECODED size; regions validate vs this
    k->bars[i].mmio    = m;
    k->bars[i].present = true;
    k->bars[i].is_64   = is64;
    *out_is64 = is64;
    return 1;
}

static int pci_assign_bars(struct KObj_PCI *k, struct virtio_pci_dev *d) {
    for (u32 i = 0; i < PCI_BAR_COUNT; ) {
        bool is64 = false;
        int r = pci_assign_one_bar(k, d, i, &is64);
        if (r < 0) return -1;
        i += (r == 1 && is64) ? 2u : 1u;   // a 64-bit BAR consumes the high slot
    }
    return 0;
}

// =============================================================================
// VIRTIO_PCI_CAP capability walk (VIRTIO 1.2 §4.1.4).
// =============================================================================

// Resolve common/notify/isr/device config regions from the vendor-capability
// list into k->regions[]. Bounded (cap pointers live in [0x40, 0x100); a 48-hop
// guard breaks any loop). Validates each region's BAR index + extent against the
// assigned BAR's decoded size (a region into an unassigned BAR or past its size
// is hostile/malformed → reject the claim). Returns 0 (resolved what was
// present, validated) or -1 (malformed). Non-static + header-declared so a
// deterministic unit test drives the hostile-layout rejection branches (cap
// loop / OOB bar / unassigned bar / oversized region) over a synthetic config.
int pci_walk_caps(struct KObj_PCI *k, struct virtio_pci_dev *d) {
    u16 status = virtio_pci_cfg_read16(d, PCI_CFG_STATUS);
    if (!(status & PCI_STATUS_CAP_LIST)) return 0;   // no caps (non-modern) — not an error

    u32 ptr = virtio_pci_cfg_read8(d, PCI_CFG_CAP_PTR) & 0xFCu;   // dword-aligned
    int guard = 0;
    while (ptr != 0) {
        if (ptr < 0x40u || ptr > 0xFCu) break;       // outside the cap window
        if (++guard > 48) break;                     // loop guard (192 B / 4)

        u8 cap_vndr = virtio_pci_cfg_read8(d, ptr + 0u);
        u32 cap_next = virtio_pci_cfg_read8(d, ptr + 1u) & 0xFCu;

        if (cap_vndr == PCI_CAP_ID_VNDR) {
            u8  cfg_type = virtio_pci_cfg_read8(d, ptr + 3u);
            if (cfg_type >= VIRTIO_PCI_CAP_COMMON_CFG &&
                cfg_type <= VIRTIO_PCI_CAP_DEVICE_CFG) {
                u8  bar    = virtio_pci_cfg_read8 (d, ptr + 4u);
                u32 offset = virtio_pci_cfg_read32(d, ptr + 8u);
                u32 length = virtio_pci_cfg_read32(d, ptr + 12u);

                if (bar >= PCI_BAR_COUNT) return -1;            // hostile BAR index
                if (!k->bars[bar].present) return -1;          // references an unassigned BAR
                if ((u64)offset + (u64)length > k->bars[bar].size) return -1;  // OOB region

                struct pci_region *r = &k->regions[cfg_type - 1u];
                if (!r->present) {     // first cap of a kind wins
                    r->present = true;
                    r->bar     = bar;
                    r->offset  = offset;
                    r->length  = length;
                    if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                        k->notify_off_multiplier =
                            virtio_pci_cfg_read32(d, ptr + 16u);
                    }
                }
            }
            // cfg_type 5 (PCI_CFG) / 8 (SHARED_MEMORY) are not mapped here.
        }
        ptr = cap_next;
    }
    return 0;
}

// =============================================================================
// Lifecycle.
// =============================================================================

// Quiesce the device, drop every assigned BAR claim, and release the
// exclusivity slot. Shared by free_internal (last unref) and the claim-failure
// rollback (where ref is still 1). The g_pci_claims slot was installed before
// any path that reaches here, so a missing slot is corruption.
static void pci_release_bars_and_claim(struct KObj_PCI *k) {
    // Quiesce: disable MEM-decode + bus-master before releasing the BAR PA
    // claims (which may be re-handed-out). Config space is kernel-owned, so the
    // write is always valid; harmless if decode was never enabled (rollback).
    if (k->vpd) {
        u16 cmd = virtio_pci_cfg_read16(k->vpd, PCI_CFG_COMMAND);
        cmd &= ~(u16)(PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER);
        virtio_pci_cfg_write16(k->vpd, PCI_CFG_COMMAND, cmd);
    }

    for (u32 i = 0; i < PCI_BAR_COUNT; i++) {
        if (k->bars[i].present && k->bars[i].mmio) {
            // A live user mapping holds an independent burrow ref (the #847 dual
            // lifetime), so this unref frees the PA only once that mapping is
            // also gone — releasing the g_mmio_claims slot for re-use.
            kobj_mmio_unref(k->bars[i].mmio);
            k->bars[i].mmio    = NULL;
            k->bars[i].present = false;
        }
    }

    irq_state_t s = spin_lock_irqsave(&g_pci_lock);
    int slot = find_pci_slot_by_owner(k);
    if (slot < 0) {
        spin_unlock_irqrestore(&g_pci_lock, s);
        extinction("pci_release_bars_and_claim: no claim slot for owner (UAF or double-free?)");
    }
    g_pci_claims[slot].owner = NULL;
    g_pci_claims[slot].bus   = 0;
    g_pci_claims[slot].dev   = 0;
    g_pci_claims[slot].fn    = 0;
    spin_unlock_irqrestore(&g_pci_lock, s);
}

static void kobj_pci_free_internal(struct KObj_PCI *k) {
    if (k->magic != KOBJ_PCI_MAGIC)
        extinction("kobj_pci_free_internal of corrupted KObj_PCI");
    if (k->ref != 0)
        extinction("kobj_pci_free_internal with ref > 0");

    pci_release_bars_and_claim(k);

    k->magic = 0;       // UAF defense before the SLUB freelist write
    kfree(k);
    __atomic_fetch_sub(&g_pci_live, 1u, __ATOMIC_RELAXED);
}

void kobj_pci_ref(struct KObj_PCI *k) {
    if (!k)                          extinction("kobj_pci_ref(NULL)");
    if (k->magic != KOBJ_PCI_MAGIC)  extinction("kobj_pci_ref of corrupted KObj_PCI");
    int old = __atomic_fetch_add(&k->ref, 1, __ATOMIC_RELAXED);
    if (old <= 0) extinction("kobj_pci_ref of zero-ref KObj_PCI (already freed?)");
}

void kobj_pci_unref(struct KObj_PCI *k) {
    if (!k) return;
    if (k->magic != KOBJ_PCI_MAGIC)
        extinction("kobj_pci_unref of corrupted KObj_PCI");
    int old = __atomic_fetch_sub(&k->ref, 1, __ATOMIC_ACQ_REL);
    if (old <= 0) extinction("kobj_pci_unref of zero-ref KObj_PCI (double-free?)");
    if (old == 1) kobj_pci_free_internal(k);
}

struct KObj_MMIO *kobj_pci_bar_mmio(struct KObj_PCI *k, u32 bar_index) {
    if (!k || k->magic != KOBJ_PCI_MAGIC) return NULL;
    if (bar_index >= PCI_BAR_COUNT)       return NULL;
    if (!k->bars[bar_index].present)      return NULL;
    return k->bars[bar_index].mmio;
}

// Read-only (bus,dev,fn) resolution for a (virtio_device_id, nth) pair -- the
// SAME nth match kobj_pci_claim will pick (the device table is built once at
// boot + never mutated, so this is deterministic and agrees with the
// subsequent claim). The SYS_PCI_CLAIM allowance gate resolves the function
// HERE, before claiming, so it checks the EXACT (bus,dev,fn) the claim
// resolves -- and so a not-permitted device is never enabled (bus-master) only
// to be rolled back. `nth` (0-based, enumeration order) reaches a second
// same-id function (G-7c: two virtio-input functions, keyboard + tablet);
// nth 0 is the historical first-match behavior.
// Returns 0 + fills bus/dev/fn on a match; -1 if no such device / not inited.
int kobj_pci_resolve_bdf(u32 virtio_device_id, u32 nth, u8 *bus, u8 *dev, u8 *fn) {
    if (!g_pci_initialized) return -1;
    struct virtio_pci_dev *d = virtio_pci_find_by_device_id(virtio_device_id, nth);
    if (!d || !d->cfg) return -1;
    if (bus) *bus = d->bus;
    if (dev) *dev = d->dev;
    if (fn)  *fn  = d->fn;
    return 0;
}

struct KObj_PCI *kobj_pci_claim(u32 virtio_device_id, u32 nth) {
    if (!g_pci_initialized) return NULL;

    struct virtio_pci_dev *d = virtio_pci_find_by_device_id(virtio_device_id, nth);
    if (!d || !d->cfg) return NULL;

    struct KObj_PCI *k = kmalloc(sizeof(*k), KP_ZERO);
    if (!k) return NULL;
    k->magic            = KOBJ_PCI_MAGIC;
    k->ref              = 1;
    k->vpd              = d;
    k->bus              = d->bus;
    k->dev              = d->dev;
    k->fn               = d->fn;
    k->virtio_device_id = d->virtio_device_id;

    // Install the (bus,dev,fn) exclusivity slot first; reject a double-claim and
    // a full table here, before any device mutation. After the slot is in,
    // EVERY failure path routes through kobj_pci_unref (ref 1 -> free_internal ->
    // pci_release_bars_and_claim releases this slot + any assigned BAR).
    irq_state_t s = spin_lock_irqsave(&g_pci_lock);
    if (find_pci_slot_by_bdf(d->bus, d->dev, d->fn) >= 0) {
        spin_unlock_irqrestore(&g_pci_lock, s);
        k->magic = 0; kfree(k);
        return NULL;
    }
    int slot = find_free_pci_slot();
    if (slot < 0) {
        spin_unlock_irqrestore(&g_pci_lock, s);
        k->magic = 0; kfree(k);
        return NULL;
    }
    g_pci_claims[slot].owner = k;
    g_pci_claims[slot].bus   = d->bus;
    g_pci_claims[slot].dev   = d->dev;
    g_pci_claims[slot].fn    = d->fn;
    spin_unlock_irqrestore(&g_pci_lock, s);

    __atomic_fetch_add(&g_pci_created, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_pci_live,    1u, __ATOMIC_RELAXED);

    // Assign + size the BARs (decode still off), then resolve the capability
    // regions against the assigned BAR sizes. Either failing rolls everything
    // back via unref.
    if (pci_assign_bars(k, d) < 0) { kobj_pci_unref(k); return NULL; }
    if (pci_walk_caps(k, d)  < 0)  { kobj_pci_unref(k); return NULL; }

    // Enable MEM-decode + bus-master now that the BARs decode at real PAs. The
    // device still won't DMA until a driver completes the virtio handshake +
    // DRIVER_OK over the mapped BAR.
    u16 cmd = virtio_pci_cfg_read16(d, PCI_CFG_COMMAND);
    virtio_pci_cfg_write16(d, PCI_CFG_COMMAND,
                           cmd | PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER);

    // INTx routing (INTA). Non-fatal if the DTB interrupt-map is absent — a
    // driver can poll; intid_valid records the outcome.
    u32 intid = 0;
    if (dtb_pci_intx_route(d->dev, PCI_INT_PIN_INTA, &intid)) {
        k->intid       = intid;
        k->intid_valid = true;
    }

    return k;
}
