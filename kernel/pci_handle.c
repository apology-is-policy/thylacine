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

#include <thylacine/pci_irq.h>
#include <thylacine/dtb.h>
#include <thylacine/extinction.h>
#include <thylacine/mmio_handle.h>
#include <thylacine/page.h>
#include <thylacine/pci_handle.h>
#include <thylacine/spinlock.h>
#include <thylacine/types.h>
#include <thylacine/virtio_pci.h>

#include "../arch/arm64/uart.h"
#include "../arch/arm64/mmu.h"
#include "../arch/arm64/mmio.h"
#include "../arch/arm64/gic_msi.h"
#include "../arch/arm64/timer.h"
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

// Hardware backing survives capability claims. The enumerated function set is
// immutable (no hotplug); assigning each BAR once bounds both aperture use and
// future kernel MSI-X/reset mappings across arbitrarily many driver restarts.
// Geometry/placement are written only by the BDF's exclusive claim. Releasing
// and reacquiring g_pci_lock publishes them to a later claimant. Live access
// still requires fresh exclusive KObj_MMIO claims; these are not capabilities.
struct pci_backing {
    struct virtio_pci_dev *device; // immutable after init
    u64 pa[PCI_BAR_COUNT], size[PCI_BAR_COUNT];
    bool is64[PCI_BAR_COUNT];
    void *table, *common;
    u64 table_pa, table_size, common_pa;
    struct gic_msi_route routes[PCI_MSIX_VECTOR_MAX];
    bool routed[PCI_MSIX_VECTOR_MAX], retiring[PCI_MSIX_VECTOR_MAX];
#ifdef KERNEL_TESTS
    u32 test_msix_fail; // kernel-only one-shot readback-failure injection
#endif
};
static struct pci_backing backings[VIRTIO_PCI_MAX_DEVS];
static struct pci_backing *pci_backing(struct KObj_PCI *k) {
    for (u32 i = 0; i < VIRTIO_PCI_MAX_DEVS; i++)
        if (backings[i].device == k->vpd) return &backings[i];
    return NULL;
}


// BAR aperture allocator. Each enumerated hardware BAR consumes one placement,
// retained in its backing across capability claims. No per-restart advance.
// All cursor accesses are under g_pci_lock.
static bool g_pci_bar_inited;
static u64  g_pci_bar_base;
static u64  g_pci_bar_end;
static u64  g_pci_bar_next;
// The 64-bit MMIO arena (#166, audit F3). A BAR too large for the 32-bit
// window -- a `hostmem=N` virtio-gpu presents a multi-GiB one -- is
// structurally unplaceable there (QEMU virt's 32-bit window is ~752 MiB),
// so the claim aborted before userspace's "claim but leave unmapped"
// policy could ever run: the whole point of #166. Seeded lazily +
// independently; absent on a DTB without a 64-bit range, in which case an
// oversized BAR still fails the claim (honestly, as before).
static bool g_pci_bar64_inited;
static bool g_pci_bar64_absent;
static u64  g_pci_bar64_base;
static u64  g_pci_bar64_end;
static u64  g_pci_bar64_next;

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
    // Before any shared wire is enabled, unclaimed supported functions must
    // not assert INTx or master the bus. Enumeration is complete and immutable.
    for (int i = 0; i < virtio_pci_dev_count(); i++) {
        struct virtio_pci_dev *d = virtio_pci_dev_get(i);
        backings[i].device = d;
        u16 cmd = virtio_pci_cfg_read16(d, PCI_CFG_COMMAND);
        cmd = (cmd | PCI_CMD_INTX_DISABLE) & ~(u16)(PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER);
        virtio_pci_cfg_write16(d, PCI_CFG_COMMAND, cmd);
        (void)virtio_pci_cfg_read16(d, PCI_CFG_COMMAND);
    }
    __asm__ volatile("dsb sy" ::: "memory");
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
// `is64` is load-bearing (round-2 F4): a 32-bit BAR can only be programmed
// with a 32-bit address, so it must NEVER be placed in the high arena --
// pci_assign_one_bar writes only the low dword for it, silently truncating
// a high PA to something the device would decode inside RAM while the
// kernel's claim sat on the untruncated address.
static bool pci_bar_alloc(u64 size, bool is64, u64 *out_pa) {
    irq_state_t s = spin_lock_irqsave(&g_pci_lock);

    if (!g_pci_bar_inited) {
        u64 base = 0, len = 0;
        if (dtb_pci_mem_window(&base, &len) && len != 0) {
            g_pci_bar_base = base;
            g_pci_bar_end  = base + len;   // dtb_pci_mem_window bounds this < IPS
            g_pci_bar_next = base;
            g_pci_bar_inited = true;
        }
    }
    if (!g_pci_bar64_inited && !g_pci_bar64_absent) {
        u64 base = 0, len = 0;
        if (dtb_pci_mem_window64(&base, &len) && len != 0) {
            g_pci_bar64_base = base;
            g_pci_bar64_end  = base + len;
            g_pci_bar64_next = base;
            g_pci_bar64_inited = true;
        } else {
            g_pci_bar64_absent = true;
        }
    }

    // Pick the arena by SIZE, not by the BAR's 64-bit-capable bit: a small
    // 64-bit BAR still places fine below 4 GiB, so every existing driver
    // keeps its current addresses. Only a BAR that cannot fit the 32-bit
    // window at all is routed high.
    // Prefer the 32-bit arena while it can still SERVE the request (round-2
    // F11: the old test compared against the window's total span, so once
    // the arena was exhausted every later BAR failed there instead of
    // falling through to the 512 GiB one). A 64-bit-capable BAR may then
    // fall high; a 32-bit BAR may not, and fails honestly as it did before
    // the arena existed.
    u64 avail32 = 0;
    if (g_pci_bar_inited) {
        u64 a32 = (g_pci_bar_next + (size - 1)) & ~(size - 1);
        if (a32 >= g_pci_bar_base && a32 + size >= a32 && a32 + size <= g_pci_bar_end)
            avail32 = 1;
    }
    bool fits32 = avail32 != 0;
    if (!fits32 && (!is64 || !g_pci_bar64_inited)) {
        spin_unlock_irqrestore(&g_pci_lock, s);   // no arena may hold it
        return false;
    }
    u64 *next   = fits32 ? &g_pci_bar_next : &g_pci_bar64_next;
    u64 lo      = fits32 ? g_pci_bar_base  : g_pci_bar64_base;
    u64 hi      = fits32 ? g_pci_bar_end   : g_pci_bar64_end;

    // Align up to `size` (a power of two >= PAGE_SIZE); bounds + overflow.
    u64 a = (*next + (size - 1)) & ~(size - 1);
    if (a < lo || a + size < a || a + size > hi) {
        spin_unlock_irqrestore(&g_pci_lock, s);
        return false;
    }
    *next = a + size;
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
    struct pci_backing *backing = pci_backing(k);
    if (!backing) return -1;
    if (backing->size[i]) {
        if (backing->size[i] != size || backing->is64[i] != is64) return -1;
        pa = backing->pa[i];
    } else {
        if (!pci_bar_alloc(claim_size, is64, &pa)) return -1;
        backing->pa[i] = pa; backing->size[i] = size; backing->is64[i] = is64;
    }

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

// Parse before publishing the claim. Capabilities are untrusted: each extent
// must fit conventional config space, and a repeated pointer is a hard error.
int pci_walk_caps(struct KObj_PCI *k, struct virtio_pci_dev *d) {
    u16 status = virtio_pci_cfg_read16(d, PCI_CFG_STATUS);
    if (!(status & PCI_STATUS_CAP_LIST)) return 0;
    u32 ptr = virtio_pci_cfg_read8(d, PCI_CFG_CAP_PTR) & 0xFCu;
    u64 visited = 0;
    while (ptr) {
        if (ptr < 0x40u || ptr > 0xFCu) return -1;
        u64 bit = 1ull << (ptr / 4u);
        if (visited & bit) return -1;
        visited |= bit;
        u8 id = virtio_pci_cfg_read8(d, ptr);
        u32 next = virtio_pci_cfg_read8(d, ptr + 1u) & 0xFCu;
        if (id == PCI_CAP_ID_MSIX) {
            if (ptr > 0x100u - 12u || k->msix.cap_offset) return -1;
            u16 ctrl = virtio_pci_cfg_read16(d, ptr + 2u);
            u32 table = virtio_pci_cfg_read32(d, ptr + 4u);
            u32 pba = virtio_pci_cfg_read32(d, ptr + 8u);
            u32 tb = table & 7u, pb = pba & 7u;
            u64 to = table & ~7u, po = pba & ~7u;
            u32 entries = (ctrl & 0x7ffu) + 1u;
            u64 tl = (u64)entries * 16u;
            u64 pl = ((entries + 63u) / 64u) * 8u;
            if (tb >= PCI_BAR_COUNT || pb >= PCI_BAR_COUNT ||
                !k->bars[tb].present || !k->bars[pb].present) return -1;
            if (to > k->bars[tb].size || tl > k->bars[tb].size - to ||
                po > k->bars[pb].size || pl > k->bars[pb].size - po) return -1;
            if (tb == pb && to < po + pl && po < to + tl) return -1;
            k->msix = (struct pci_msix){ .cap_offset = (u16)ptr,
                .entries = (u16)entries, .table_bar = (u8)tb, .pba_bar = (u8)pb,
                .table_offset = (u32)to, .pba_offset = (u32)po };
        } else if (id == PCI_CAP_ID_VNDR) {
            u32 len = virtio_pci_cfg_read8(d, ptr + 2u);
            if (len < 4u || len > 0x100u - ptr) return -1;
            u8 type = virtio_pci_cfg_read8(d, ptr + 3u);
            if (type >= 1u && type <= 4u) {
                if (len < (type == VIRTIO_PCI_CAP_NOTIFY_CFG ? 20u : 16u)) return -1;
                u8 bar = virtio_pci_cfg_read8(d, ptr + 4u);
                u32 off = virtio_pci_cfg_read32(d, ptr + 8u);
                u32 size = virtio_pci_cfg_read32(d, ptr + 12u);
                if (bar >= PCI_BAR_COUNT || !k->bars[bar].present ||
                    (u64)off + size > k->bars[bar].size) return -1;
                struct pci_region *r = &k->regions[type - 1u];
                if (!r->present) {
                    *r = (struct pci_region){ .present = true, .bar = bar,
                        .offset = off, .length = size };
                    if (type == VIRTIO_PCI_CAP_NOTIFY_CFG)
                        k->notify_off_multiplier = virtio_pci_cfg_read32(d, ptr + 16u);
                }
            } else if (type == VIRTIO_PCI_CAP_SHARED_MEMORY_CFG) {
                if (len < 24u) return -1;
                u8 bar = virtio_pci_cfg_read8(d, ptr + 4u);
                u8 shmid = virtio_pci_cfg_read8(d, ptr + 5u);
                u64 off = (u64)virtio_pci_cfg_read32(d, ptr + 8u)
                        | ((u64)virtio_pci_cfg_read32(d, ptr + 16u) << 32);
                u64 size = (u64)virtio_pci_cfg_read32(d, ptr + 12u)
                         | ((u64)virtio_pci_cfg_read32(d, ptr + 20u) << 32);
                if (bar >= PCI_BAR_COUNT || !k->bars[bar].present ||
                    off > k->bars[bar].size || size > k->bars[bar].size - off) return -1;
                for (u32 i = 0; i < PCI_SHM_COUNT; i++) {
                    if (k->shm[i].present) continue;
                    k->shm[i] = (struct pci_shm){ .present = true, .bar = bar,
                        .shmid = shmid, .offset = off, .length = size };
                    break;
                }
            }
        }
        ptr = next;
    }
    // Essential transport registers sharing a routing page cannot be directly
    // mapped safely, even in INTx mode. Fail this unsupported layout at claim.
    for (u32 i = 0; i < VIRTIO_PCI_CAP_REGION_COUNT; i++) {
        const struct pci_region *r = &k->regions[i];
        if (!r->present || !r->length) continue;
        u64 lo = (u64)r->offset & ~(u64)(PAGE_SIZE - 1u);
        u64 hi = ((u64)r->offset + r->length + PAGE_SIZE - 1u) & ~(u64)(PAGE_SIZE - 1u);
        if (!kobj_pci_user_range(k, r->bar, lo, hi - lo)) return -1;
    }
    return 0;
}

// Rounded reserved intervals. Two intervals may share pages; callers must
// subtract their union rather than assume distinct BARs or distinct pages.
static void pci_msix_span(const struct KObj_PCI *k, u32 which,
                          u32 *bar, u64 *lo, u64 *hi) {
    const struct pci_msix *m = &k->msix;
    u64 off = which ? m->pba_offset : m->table_offset;
    u64 len = which ? ((m->entries + 63u) / 64u) * 8u : (u64)m->entries * 16u;
    *bar = which ? m->pba_bar : m->table_bar;
    *lo = off & ~(u64)(PAGE_SIZE - 1u);
    *hi = (off + len + PAGE_SIZE - 1u) & ~(u64)(PAGE_SIZE - 1u);
}

bool kobj_pci_user_range(const struct KObj_PCI *k, u32 bar, u64 off, u64 len) {
    if (!k || bar >= PCI_BAR_COUNT || !k->bars[bar].present || !len ||
        ((off | len) & (PAGE_SIZE - 1u))) return false;
    u64 size = k->bars[bar].size;
    if (size > ~0ull - (PAGE_SIZE - 1u)) return false;
    size = (size + PAGE_SIZE - 1u) & ~(u64)(PAGE_SIZE - 1u);
    if (off > size || len > size - off) return false;
    if (k->msix.cap_offset) {
        for (u32 i = 0; i < 2u; i++) {
            u32 b; u64 lo, hi;
            pci_msix_span(k, i, &b, &lo, &hi);
            if (b == bar && off < hi && lo < off + len) return false;
        }
    }
    return true;
}

u32 kobj_pci_map_windows(const struct KObj_PCI *k,
                         struct pci_map_window out[PCI_MAP_WINDOW_MAX]) {
    u32 count = 0;
    for (u32 bar = 0; bar < PCI_BAR_COUNT; bar++) {
        if (!k->bars[bar].present) continue;
        u64 size = k->bars[bar].size;
        if (size > ~0ull - (PAGE_SIZE - 1u)) return 0;
        size = (size + PAGE_SIZE - 1u) & ~(u64)(PAGE_SIZE - 1u);
        u64 pos = 0;
        while (pos < size) {
            u64 next = size, skip = pos;
            if (k->msix.cap_offset) {
                for (u32 i = 0; i < 2u; i++) {
                    u32 b; u64 lo, hi;
                    pci_msix_span(k, i, &b, &lo, &hi);
                    if (b != bar || hi <= pos) continue;
                    if (lo <= pos) { if (hi > skip) skip = hi; }
                    else if (lo < next) next = lo;
                }
            }
            if (skip > pos) { pos = skip; continue; }
            if (count >= PCI_MAP_WINDOW_MAX) extinction("PCI window bound");
            out[count++] = (struct pci_map_window){ .bar = bar,
                .offset = pos, .length = next - pos, .reserved = 0 };
            pos = next;
        }
    }
    return count;
}

// =============================================================================
// Lifecycle.
// =============================================================================

// Kernel mappings belong to immutable hardware backing, not a capability
// incarnation. Stable BAR placement bounds the permanent vmalloc footprint.
static bool pci_map_control(struct KObj_PCI *k) {
    struct pci_backing *b = pci_backing(k);
    if (!b) return false;
    struct pci_region *common = &k->regions[VIRTIO_PCI_CAP_COMMON_CFG - 1];
    if (common->present && common->length >= 56) {
        u64 pa = k->bars[common->bar].pa + common->offset;
        if (pa & 7u) return false; // common queue addresses require 64-bit alignment
        if (b->common && b->common_pa != pa) return false;
        if (!b->common) {
            b->common = mmu_map_mmio(pa, 56);
            if (!b->common) return false;
            b->common_pa = pa;
        }
        k->common_cfg = b->common;
    }
    if (k->msix.cap_offset) {
        u64 pa = k->bars[k->msix.table_bar].pa + k->msix.table_offset;
        u64 size = (u64)k->msix.entries * 16;
        if (b->table && (b->table_pa != pa || b->table_size != size)) return false;
        if (!b->table) {
            b->table = mmu_map_mmio(pa, (size_t)size);
            if (!b->table) return false;
            b->table_pa = pa; b->table_size = size;
        }
        k->msix_table = b->table;
    }
    return true;
}
// No spinlock across reset completion. Caller first masks function delivery and
// clears bus mastering, while keeping MEM decoding for the status readback.
static bool pci_reset_transport(struct KObj_PCI *k) {
    if (!k->common_cfg) return false;
    volatile u8 *status = (volatile u8 *)k->common_cfg + 20;
    io_write8(status, 0);
    __asm__ volatile("dsb sy" ::: "memory");
    u64 deadline = timer_now_ns() + 10000000ull;
    do {
        if (io_read8(status) == 0) {
            __asm__ volatile("dsb sy" ::: "memory");
            return true;
        }
        __asm__ volatile("yield" ::: "memory");
    } while (timer_now_ns() < deadline);
    return false;
}
static void pci_retire_route(struct KObj_PCI *k, u32 index, bool source_quiesced) {
    struct pci_backing *b = pci_backing(k);
    if (!b || index >= PCI_MSIX_VECTOR_MAX) return;
    irq_state_t flags = spin_lock_irqsave(&k->cfg_lock);
    if (!b->routed[index] || b->retiring[index]) {
        spin_unlock_irqrestore(&k->cfg_lock, flags); return;
    }
    b->retiring[index] = true;
    struct gic_msi_route route = b->routes[index];
    spin_unlock_irqrestore(&k->cfg_lock, flags);
    bool retired = gic_msi_retire(&route, source_quiesced);
    flags = spin_lock_irqsave(&k->cfg_lock);
    if (retired) b->routed[index] = false;
    b->retiring[index] = false;
    spin_unlock_irqrestore(&k->cfg_lock, flags);
}
static void pci_retire_routes(struct KObj_PCI *k, bool source_quiesced) {
    for (u32 i = 0; i < PCI_MSIX_VECTOR_MAX; i++) pci_retire_route(k, i, source_quiesced);
}
void kobj_pci_msix_retire(struct KObj_PCI *k, u32 index) {
    irq_state_t flags = spin_lock_irqsave(&k->cfg_lock);
    bool proof = k->revoked && k->reset_complete;
    spin_unlock_irqrestore(&k->cfg_lock, flags);
    pci_retire_route(k, index, proof);
}
bool kobj_pci_msix_mask(struct KObj_PCI *k, u32 index, bool masked) {
    if (!k->msix_table || index >= k->msix.entries || index >= PCI_MSIX_VECTOR_MAX) return false;
    irq_state_t flags = spin_lock_irqsave(&k->cfg_lock);
    bool ok = !k->revoked && (masked || !k->irq_faulted);
    if (ok) {
        volatile u32 *entry = (volatile u32 *)k->msix_table + index * 4;
        io_write32(entry + 3, masked ? 1u : 0u);
        ok = (io_read32(entry + 3) & 1u) == (masked ? 1u : 0u);
#ifdef KERNEL_TESTS
        struct pci_backing *b = pci_backing(k);
        if (b && (b->test_msix_fail & 2u)) { b->test_msix_fail &= ~2u; ok = false; }
#endif
        __asm__ volatile("dsb sy" ::: "memory");
    }
    // Revocation already masks/disables the entire function before removing
    // MEM decode. Never touch a BAR after that terminal transition.
    spin_unlock_irqrestore(&k->cfg_lock, flags);
    return ok;
}
int kobj_pci_msix_program(struct KObj_PCI *k, u32 index, const struct gic_msi_route *route) {
    struct pci_backing *b = pci_backing(k);
    if (!b || !route || !k->msix_table || !k->common_cfg ||
        index >= k->msix.entries || index >= PCI_MSIX_VECTOR_MAX) return false;
    irq_state_t flags = spin_lock_irqsave(&k->cfg_lock);
    if (k->revoked || k->irq_faulted || b->routed[index] || b->retiring[index]) {
        spin_unlock_irqrestore(&k->cfg_lock, flags); return false;
    }
    // Retain the lease before touching the table. A failed mask/readback is
    // not proof that no message escaped; it must survive for quarantine/reset.
    b->routes[index] = *route; b->routed[index] = true;
    volatile u32 *entry = (volatile u32 *)k->msix_table + index * 4;
    io_write32(entry + 3, 1);
    bool ok = (io_read32(entry + 3) & 1u) != 0;
    if (ok) {
        io_write32(entry, (u32)route->address); io_write32(entry + 1, (u32)(route->address >> 32));
        io_write32(entry + 2, route->data);
        __asm__ volatile("dsb sy" ::: "memory");
        ok = io_read32(entry) == (u32)route->address && io_read32(entry + 1) == (u32)(route->address >> 32) &&
             io_read32(entry + 2) == route->data && (io_read32(entry + 3) & 1u);
    }
#ifdef KERNEL_TESTS
    if (b->test_msix_fail & 1u) { b->test_msix_fail &= ~1u; ok = false; }
#endif
    if (ok) {
        u32 off = k->msix.cap_offset + 2u;
        u16 ctrl = virtio_pci_cfg_read16(k->vpd, off);
        ctrl = (ctrl | PCI_MSIX_ENABLE) & ~(u16)PCI_MSIX_MASK_ALL;
        virtio_pci_cfg_write16(k->vpd, off, ctrl);
        ok = (virtio_pci_cfg_read16(k->vpd, off) & (PCI_MSIX_ENABLE | PCI_MSIX_MASK_ALL)) == PCI_MSIX_ENABLE;
        __asm__ volatile("dsb sy" ::: "memory");
    }

    spin_unlock_irqrestore(&k->cfg_lock, flags);
    return ok ? 1 : -1;
}

void kobj_pci_msix_off(struct KObj_PCI *k) {
    if (!k->msix.cap_offset) return;
    irq_state_t flags = spin_lock_irqsave(&k->cfg_lock);
    u32 off = k->msix.cap_offset + 2u;
    u16 ctrl = virtio_pci_cfg_read16(k->vpd, off);
    virtio_pci_cfg_write16(k->vpd, off, (ctrl | PCI_MSIX_MASK_ALL) & ~(u16)PCI_MSIX_ENABLE);
    (void)virtio_pci_cfg_read16(k->vpd, off);
    __asm__ volatile("dsb sy" ::: "memory");
    spin_unlock_irqrestore(&k->cfg_lock, flags);
}

// Quiesce the device, drop every assigned BAR claim, and release the
// exclusivity slot. Shared by free_internal (last unref) and the claim-failure
// rollback (where ref is still 1). The g_pci_claims slot was installed before
// any path that reaches here, so a missing slot is corruption.
// Stop the device decoding + mastering. Idempotent, and safe to call from
// the Proc-death quiesce BEFORE the handle table is torn down (audit F8):
// a PCI driver's registers are BAR-decoded, so the virtio-MMIO reset sweep
// cannot reach them, and a still-mastering device would DMA into pages the
// exit path has already returned to the buddy.
// Config completion pairs a same-function readback with the device barrier.
// Caller holds cfg_lock (or the claim is not yet published).
static void pci_command_write(struct KObj_PCI *k, u16 command) {
    virtio_pci_cfg_write16(k->vpd, PCI_CFG_COMMAND, command);
    (void)virtio_pci_cfg_read16(k->vpd, PCI_CFG_COMMAND);
    __asm__ volatile("dsb sy" ::: "memory");
}
bool kobj_pci_intx_set(struct KObj_PCI *k, bool enable) {
    irq_state_t s = spin_lock_irqsave(&k->cfg_lock);
    bool ok = !enable || (!k->revoked && !k->irq_faulted);
    if (ok) {
        u16 cmd = virtio_pci_cfg_read16(k->vpd, PCI_CFG_COMMAND);
        if (enable) cmd &= ~(u16)PCI_CMD_INTX_DISABLE;
        else cmd |= PCI_CMD_INTX_DISABLE;
        pci_command_write(k, cmd);
    }
    spin_unlock_irqrestore(&k->cfg_lock, s);
    return ok;
}
bool kobj_pci_intx_asserted(struct KObj_PCI *k) {
    irq_state_t s = spin_lock_irqsave(&k->cfg_lock);
    bool asserted = (virtio_pci_cfg_read16(k->vpd, PCI_CFG_STATUS) & PCI_STATUS_INTERRUPT) != 0;
    spin_unlock_irqrestore(&k->cfg_lock, s);
    return asserted;
}
#ifdef KERNEL_TESTS
// Only the regression suite can request this hook; it has no syscall or
// userspace control. The real MMIO write/read still occurs before failure.
void pci_test_msix_fail(struct KObj_PCI *k, u32 stages);
void pci_test_msix_fail(struct KObj_PCI *k, u32 stages) {
    irq_state_t f = spin_lock_irqsave(&k->cfg_lock);
    struct pci_backing *b = pci_backing(k);
    if (b) b->test_msix_fail = stages;
    spin_unlock_irqrestore(&k->cfg_lock, f);
}
#endif
bool kobj_pci_irq_usable(struct KObj_PCI *k) {
    irq_state_t f = spin_lock_irqsave(&k->cfg_lock);
    bool usable = !k->revoked && !k->irq_faulted;
    spin_unlock_irqrestore(&k->cfg_lock, f); return usable;
}
void kobj_pci_irq_fault(struct KObj_PCI *k) {
    irq_state_t f = spin_lock_irqsave(&k->cfg_lock);
    k->irq_faulted = true;
    if (k->msix.cap_offset) {
        u32 off = k->msix.cap_offset + 2u;
        u16 ctrl = virtio_pci_cfg_read16(k->vpd, off);
        virtio_pci_cfg_write16(k->vpd, off, (ctrl | PCI_MSIX_MASK_ALL) & ~(u16)PCI_MSIX_ENABLE);
        (void)virtio_pci_cfg_read16(k->vpd, off);
    }
    u16 cmd = virtio_pci_cfg_read16(k->vpd, PCI_CFG_COMMAND);
    pci_command_write(k, cmd | PCI_CMD_INTX_DISABLE);
    spin_unlock_irqrestore(&k->cfg_lock, f);
}
bool kobj_pci_is_live(struct KObj_PCI *k) {
    irq_state_t s = spin_lock_irqsave(&k->cfg_lock);
    bool live = !k->revoked;
    spin_unlock_irqrestore(&k->cfg_lock, s);
    return live;
}
static bool pci_quiesce(struct KObj_PCI *k, bool keep_decode) {
    if (!k || k->magic != KOBJ_PCI_MAGIC || !k->vpd) return false;
    irq_state_t s = spin_lock_irqsave(&k->cfg_lock);
    k->revoked = true;
    u16 cmd = virtio_pci_cfg_read16(k->vpd, PCI_CFG_COMMAND);
    bool decode = (cmd & PCI_CMD_MEM_SPACE) != 0;
    bool changed = (cmd & (PCI_CMD_BUS_MASTER | (keep_decode ? 0 : PCI_CMD_MEM_SPACE))) != 0;
    if (k->msix.cap_offset) {
        u32 off = k->msix.cap_offset + 2u;
        u16 ctrl = virtio_pci_cfg_read16(k->vpd, off);
        virtio_pci_cfg_write16(k->vpd, off, (ctrl | PCI_MSIX_MASK_ALL) & ~(u16)PCI_MSIX_ENABLE);
        (void)virtio_pci_cfg_read16(k->vpd, off);
    }
    pci_command_write(k, (cmd & ~(u16)PCI_CMD_BUS_MASTER) | PCI_CMD_INTX_DISABLE);
    spin_unlock_irqrestore(&k->cfg_lock, s);
    bool reset = decode && pci_reset_transport(k);
    s = spin_lock_irqsave(&k->cfg_lock);
    if (reset) k->reset_complete = true;
    if (!keep_decode) {
        cmd = virtio_pci_cfg_read16(k->vpd, PCI_CFG_COMMAND);
        pci_command_write(k, cmd & ~(u16)PCI_CMD_MEM_SPACE);
    }
    bool proof = k->reset_complete;
    spin_unlock_irqrestore(&k->cfg_lock, s);
    pci_irq_revoke_function(k);
    pci_retire_routes(k, proof);
    return changed;
}
bool kobj_pci_quiesce(struct KObj_PCI *k) { return pci_quiesce(k, false); }
bool kobj_pci_quiesce_dma_only(struct KObj_PCI *k) { return pci_quiesce(k, true); }

static void pci_release_bars_and_claim(struct KObj_PCI *k) {
    // Quiesce: disable MEM-decode + bus-master before releasing the BAR PA
    // claims (which may be re-handed-out). Config space is kernel-owned, so the
    // write is always valid; harmless if decode was never enabled (rollback).
    (void)kobj_pci_quiesce(k);

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
    spin_lock_init(&k->cfg_lock);
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
    if (!pci_map_control(k)) { kobj_pci_unref(k); return NULL; }

    // POLLED by default: routing pages remain protected, MSI-X disabled and
    // function INTx disabled until a PCI endpoint is explicitly armed.
    if (k->msix.cap_offset) {
        u32 off = k->msix.cap_offset + 2u;
        u16 ctrl = virtio_pci_cfg_read16(d, off);
        virtio_pci_cfg_write16(d, off, (ctrl | PCI_MSIX_MASK_ALL) & ~(u16)PCI_MSIX_ENABLE);
        (void)virtio_pci_cfg_read16(d, off);
    }
    // Decode registers with mastering still disabled. Reset completion makes
    // a prior incarnation's retained MSI leases safe to drain/reclaim.
    u16 cmd = virtio_pci_cfg_read16(d, PCI_CFG_COMMAND);
    pci_command_write(k, (cmd | PCI_CMD_MEM_SPACE | PCI_CMD_INTX_DISABLE) & ~(u16)PCI_CMD_BUS_MASTER);
    if (k->common_cfg && !pci_reset_transport(k)) { kobj_pci_unref(k); return NULL; }
    if (k->common_cfg) pci_retire_routes(k, true);
    if (k->msix_table) {
        // Every unallocated entry remains masked even if the owner supplies
        // that local vector index to a queue. Remove old routing addresses.
        volatile u32 *table = k->msix_table;
        for (u32 i = 0; i < k->msix.entries; i++) {
            io_write32(table + i * 4 + 3, 1);
            io_write32(table + i * 4, 0); io_write32(table + i * 4 + 1, 0); io_write32(table + i * 4 + 2, 0);
            if (!(io_read32(table + i * 4 + 3) & 1u) || io_read32(table + i * 4) ||
                io_read32(table + i * 4 + 1) || io_read32(table + i * 4 + 2)) {
                kobj_pci_unref(k); return NULL;
            }
        }
        __asm__ volatile("dsb sy" ::: "memory");
    }
    pci_command_write(k, cmd | PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER | PCI_CMD_INTX_DISABLE);

    // INTx routing (INTA). Non-fatal if the DTB interrupt-map is absent — a
    // driver can poll; intid_valid records the outcome.
    u32 intid = 0;
    u8 pin = virtio_pci_cfg_read8(d, PCI_CFG_INT_PIN);
    if (pin >= 1 && pin <= 4 && dtb_pci_intx_route(d->dev, pin, &intid)) {
        k->intid       = intid;
        k->intid_valid = true;
    }

    return k;
}
