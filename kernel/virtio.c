// VirtIO core — MMIO transport + split virtqueue (P4-F).
//
// Per ARCHITECTURE.md §13 + ROADMAP §6.1 + VIRTIO 1.2 spec. Walks DTB
// for "virtio,mmio" nodes; per node, maps the MMIO range via the
// kernel's vmalloc-style mmu_map_mmio + reads MagicValue / Version /
// DeviceID. Provides per-virtqueue allocation that drivers (P4-I+
// userspace + future kernel-side) call into.
//
// IRQ delivery is NOT in P4-F's scope — P4-G adds the irqfwd path
// from hardware IRQ to KObj_IRQ blocker wakeups. Until then, drivers
// poll Used.idx to detect completion (acceptable for the v1.0
// virtio-rng probe; userspace virtio-blk + net + input + gpu need
// real IRQs and land after P4-G).

#include <thylacine/dtb.h>
#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/types.h>
#include <thylacine/virtio.h>

#include "../arch/arm64/mmu.h"
#include "../arch/arm64/uart.h"
#include "../mm/phys.h"
#include "../mm/slub.h"

// =============================================================================
// Probed device table.
// =============================================================================

#define VIRTIO_MMIO_DEV_MAX 32

static struct virtio_mmio_dev g_virtio_mmio_devs[VIRTIO_MMIO_DEV_MAX];
static int                    g_virtio_mmio_dev_count;
static bool                   g_virtio_init_done;

// =============================================================================
// MMIO register access.
// =============================================================================

u32 virtio_mmio_read32(struct virtio_mmio_dev *dev, u32 off) {
    if (!dev || !dev->base) return 0;
    return *(volatile u32 *)((u8 *)dev->base + off);
}

void virtio_mmio_write32(struct virtio_mmio_dev *dev, u32 off, u32 val) {
    if (!dev || !dev->base) return;
    *(volatile u32 *)((u8 *)dev->base + off) = val;
}

// =============================================================================
// Status helpers.
// =============================================================================

u32 virtio_get_status(struct virtio_mmio_dev *dev) {
    return virtio_mmio_read32(dev, VIRTIO_MMIO_STATUS);
}

void virtio_set_status(struct virtio_mmio_dev *dev, u32 status) {
    virtio_mmio_write32(dev, VIRTIO_MMIO_STATUS, status);
}

void virtio_add_status(struct virtio_mmio_dev *dev, u32 bits) {
    u32 cur = virtio_get_status(dev);
    virtio_set_status(dev, cur | bits);
}

void virtio_reset(struct virtio_mmio_dev *dev) {
    // Writing 0 to status resets the device.
    virtio_set_status(dev, 0);
}

bool virtio_negotiate_features(struct virtio_mmio_dev *dev,
                                u32 want_features_lo) {
    if (!dev) return false;

    // Steps 1-3: reset → ACKNOWLEDGE → DRIVER (per VIRTIO 1.2 §3.1.1).
    virtio_reset(dev);
    virtio_add_status(dev, VIRTIO_STATUS_ACKNOWLEDGE);
    virtio_add_status(dev, VIRTIO_STATUS_DRIVER);

    // Step 4: read the device's feature bank 0 + announce intersection.
    virtio_mmio_write32(dev, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
    u32 dev_feat_lo = virtio_mmio_read32(dev, VIRTIO_MMIO_DEVICE_FEATURES);

    virtio_mmio_write32(dev, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    virtio_mmio_write32(dev, VIRTIO_MMIO_DRIVER_FEATURES,
                        want_features_lo & dev_feat_lo);

    // Bank 1 (high 32 bits): we don't yet support; explicitly negotiate
    // 0 so the device knows we've seen both banks.
    virtio_mmio_write32(dev, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
    (void)virtio_mmio_read32(dev, VIRTIO_MMIO_DEVICE_FEATURES);
    virtio_mmio_write32(dev, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    virtio_mmio_write32(dev, VIRTIO_MMIO_DRIVER_FEATURES, 0);

    // Step 5: write FEATURES_OK + re-read STATUS to verify still set.
    virtio_add_status(dev, VIRTIO_STATUS_FEATURES_OK);
    u32 status = virtio_get_status(dev);
    return (status & VIRTIO_STATUS_FEATURES_OK) != 0;
}

// =============================================================================
// DTB enumeration.
// =============================================================================

static int virtio_mmio_probe_cb(u32 match_idx, u64 reg_base, u64 reg_size,
                                 void *arg) {
    (void)arg;
    if (g_virtio_mmio_dev_count >= VIRTIO_MMIO_DEV_MAX) return 1;
    if (reg_size == 0) return 0;     // skip nodes with empty reg

    struct virtio_mmio_dev *d = &g_virtio_mmio_devs[g_virtio_mmio_dev_count];
    d->pa     = (paddr_t)reg_base;
    d->size   = (size_t)reg_size;

    // QEMU virt's virtio-mmio slots are 0x200 (512) bytes apart so
    // most slots' PAs are NOT page-aligned (only every 8th slot starts
    // a fresh page). mmu_map_mmio requires a page-aligned PA + size
    // rounded up to a page; we map the page containing the slot, then
    // compute the slot's KVA as base + intra-page-offset.
    paddr_t page_pa  = d->pa & ~(paddr_t)(PAGE_SIZE - 1);
    size_t  page_off = (size_t)(d->pa - page_pa);
    size_t  span     = page_off + d->size;
    span = (span + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);

    void *page_kva = mmu_map_mmio(page_pa, span);
    if (!page_kva) {
        // mmu_map_mmio extincts on its own failure modes; this branch is
        // defensive against a future relaxation. v1.0 it's unreachable.
        return 1;
    }
    d->base = (u8 *)page_kva + page_off;

    // Read the immutable transport identity. Per VIRTIO 1.2 §4.2.3.1
    // a slot with no device still publishes MagicValue + Version, but
    // DeviceID = 0.
    u32 magic = virtio_mmio_read32(d, VIRTIO_MMIO_MAGIC_VALUE);
    if (magic != VIRTIO_MMIO_MAGIC) {
        // DTB advertised a virtio,mmio range but the magic doesn't
        // match — either a stale DTB entry, or QEMU mapped the slot
        // differently. Drop the probe; don't panic.
        return 0;
    }

    d->version   = virtio_mmio_read32(d, VIRTIO_MMIO_VERSION);
    d->device_id = virtio_mmio_read32(d, VIRTIO_MMIO_DEVICE_ID);
    d->vendor_id = virtio_mmio_read32(d, VIRTIO_MMIO_VENDOR_ID);

    g_virtio_mmio_dev_count++;
    (void)match_idx;
    return 0;
}

void virtio_init(void) {
    if (g_virtio_init_done) extinction("virtio_init called twice");
    g_virtio_init_done = true;

    u32 matched = dtb_for_each_compat_reg("virtio,mmio",
                                           virtio_mmio_probe_cb, NULL);

    // Count devices with non-zero DeviceID for the boot banner.
    int present = 0;
    for (int i = 0; i < g_virtio_mmio_dev_count; i++) {
        if (g_virtio_mmio_devs[i].device_id != 0) present++;
    }

    uart_puts("  virtio: ");
    uart_putdec((u64)g_virtio_mmio_dev_count);
    uart_puts(" MMIO slots probed (");
    uart_putdec((u64)present);
    uart_puts(" with attached devices, ");
    uart_putdec((u64)matched - (u64)g_virtio_mmio_dev_count);
    uart_puts(" skipped due to magic mismatch or cap)\n");
}

int virtio_mmio_dev_count(void) {
    return g_virtio_mmio_dev_count;
}

struct virtio_mmio_dev *virtio_mmio_dev_get(int idx) {
    if (idx < 0 || idx >= g_virtio_mmio_dev_count) return NULL;
    return &g_virtio_mmio_devs[idx];
}

struct virtio_mmio_dev *virtio_mmio_find_by_device_id(u32 device_id) {
    for (int i = 0; i < g_virtio_mmio_dev_count; i++) {
        if (g_virtio_mmio_devs[i].device_id == device_id) {
            return &g_virtio_mmio_devs[i];
        }
    }
    return NULL;
}

int virtio_mmio_reset_in_range(u64 pa, size_t size) {
    if (size == 0) return 0;
    if (size > (u64)-1 - pa) return 0;          // overflow ⇒ caller mis-use
    u64 end = pa + (u64)size;
    int reset = 0;
    for (int i = 0; i < g_virtio_mmio_dev_count; i++) {
        u64 dpa = (u64)g_virtio_mmio_devs[i].pa;
        if (dpa < pa || dpa >= end) continue;
        // RW-7 round-2 F2: the kernel CSPRNG (random.c) DRIVES the virtio-rng
        // slot under g_rng_dev_lock. It is co-paged with sibling slots and
        // deliberately NOT page-reserved, so a driver whose own slot shares the
        // rng's page legitimately claims that page (rng + a userspace blk driver
        // both in page 0 is the live config). A driver's death must NOT reset the
        // rng -- that races random.c's in-flight pull (this path holds no rng
        // lock). The rng is kernel-owned, never a dying driver's device to stop.
        if (g_virtio_mmio_devs[i].device_id == VIRTIO_DEVICE_ID_RNG) continue;
        virtio_reset(&g_virtio_mmio_devs[i]);
        // RW-7 round-3 F2: read STATUS back -- the same VIRTIO 1.2 §2.4.2 /
        // §4.2.3.2 synchronization point virtio_virtqueue_destroy uses, so the
        // status=0 store lands (device-side reset processed) BEFORE the caller
        // frees the KObj_DMA pages. Dormant where MMIO stores trap synchronously
        // (QEMU TCG/HVF), load-bearing on a substrate with posted device writes.
        (void)virtio_get_status(&g_virtio_mmio_devs[i]);
        reset++;
    }
    return reset;
}

// =============================================================================
// Virtqueue allocation.
// =============================================================================

// RW-7 R3-F6: virtio_virtqueue_create gives each split-ring exactly one
// page (alloc_pages(0)); the default queue size MUST keep every ring within
// that page, and the vring index arithmetic requires a power-of-two size
// (VIRTIO 1.2 §2.7). desc table = 16*N; used ring = 6 + 8*N (+2 trailing).
_Static_assert((VIRTIO_VQ_NUM_DEFAULT & (VIRTIO_VQ_NUM_DEFAULT - 1u)) == 0,
               "VIRTIO_VQ_NUM_DEFAULT must be a power of 2 (VIRTIO 1.2 §2.7)");
_Static_assert(16u * VIRTIO_VQ_NUM_DEFAULT <= PAGE_SIZE,
               "descriptor table (16*N) must fit one page");
_Static_assert(6u + 8u * VIRTIO_VQ_NUM_DEFAULT <= PAGE_SIZE,
               "used ring (6+8*N) must fit one page");

// Min(a, b) for u32.
static u32 u32_min(u32 a, u32 b) { return a < b ? a : b; }

u32 virtio_vq_size_for(u32 num_max) {
    if (num_max == 0) return 0;
    u32 size = u32_min(num_max, VIRTIO_VQ_NUM_DEFAULT);
    // The vring index masks with `size`; a non-power-of-two size has
    // undefined wraparound. VIRTIO 1.2 §2.7 guarantees QueueNumMax is a
    // power of 2, so a non-pow2 here is a malformed/hostile device --
    // fail closed rather than arm an unsound ring.
    if ((size & (size - 1u)) != 0) return 0;
    return size;
}

struct virtio_virtqueue *virtio_virtqueue_create(struct virtio_mmio_dev *dev,
                                                  u32 qidx) {
    if (!dev) return NULL;

    // Step 1: select the queue.
    virtio_mmio_write32(dev, VIRTIO_MMIO_QUEUE_SEL, qidx);

    // Step 2: device tells us its max. virtio_vq_size_for clamps to our
    // per-ring single-page default AND rejects (->0) a zero or non-pow2
    // QueueNumMax (RW-7 R3-F6) -- the ring index arithmetic needs pow2.
    u32 num_max = virtio_mmio_read32(dev, VIRTIO_MMIO_QUEUE_NUM_MAX);
    u32 size = virtio_vq_size_for(num_max);
    if (size == 0) return NULL;

    struct virtio_virtqueue *vq = kmalloc(sizeof(*vq), KP_ZERO);
    if (!vq) return NULL;

    vq->dev  = dev;
    vq->qidx = qidx;
    vq->size = size;

    // Step 3: allocate three page-aligned regions. v1.0 each gets its
    // own page (4 KiB) — sufficient for size <= 64 (desc table = 1024
    // bytes; avail ring = 6+128 = 134 bytes; used ring = 6+512 = 518
    // bytes). Per VIRTIO 1.2 §2.7.4 the modern transport allows the
    // three rings at independent physical addresses.
    vq->desc_pages  = alloc_pages(0, KP_ZERO);
    vq->avail_pages = alloc_pages(0, KP_ZERO);
    vq->used_pages  = alloc_pages(0, KP_ZERO);
    if (!vq->desc_pages || !vq->avail_pages || !vq->used_pages) {
        if (vq->desc_pages)  free_pages(vq->desc_pages, 0);
        if (vq->avail_pages) free_pages(vq->avail_pages, 0);
        if (vq->used_pages)  free_pages(vq->used_pages, 0);
        kfree(vq);
        return NULL;
    }

    vq->desc_pa  = page_to_pa(vq->desc_pages);
    vq->avail_pa = page_to_pa(vq->avail_pages);
    vq->used_pa  = page_to_pa(vq->used_pages);

    vq->desc  = (struct vring_desc  *)pa_to_kva(vq->desc_pa);
    vq->avail = (struct vring_avail *)pa_to_kva(vq->avail_pa);
    vq->used  = (struct vring_used  *)pa_to_kva(vq->used_pa);

    // Step 4: register addresses with the device.
    virtio_mmio_write32(dev, VIRTIO_MMIO_QUEUE_NUM, size);

    virtio_mmio_write32(dev, VIRTIO_MMIO_QUEUE_DESC_LOW,
                        (u32)(vq->desc_pa & 0xFFFFFFFFu));
    virtio_mmio_write32(dev, VIRTIO_MMIO_QUEUE_DESC_HIGH,
                        (u32)(vq->desc_pa >> 32));

    virtio_mmio_write32(dev, VIRTIO_MMIO_QUEUE_DRIVER_LOW,
                        (u32)(vq->avail_pa & 0xFFFFFFFFu));
    virtio_mmio_write32(dev, VIRTIO_MMIO_QUEUE_DRIVER_HIGH,
                        (u32)(vq->avail_pa >> 32));

    virtio_mmio_write32(dev, VIRTIO_MMIO_QUEUE_DEVICE_LOW,
                        (u32)(vq->used_pa & 0xFFFFFFFFu));
    virtio_mmio_write32(dev, VIRTIO_MMIO_QUEUE_DEVICE_HIGH,
                        (u32)(vq->used_pa >> 32));

    // Step 5: arm the queue.
    virtio_mmio_write32(dev, VIRTIO_MMIO_QUEUE_READY, 1);

    return vq;
}

void virtio_virtqueue_destroy(struct virtio_virtqueue *vq) {
    if (!vq) return;

    // Disarm the queue before freeing its ring pages. Writing QUEUE_READY=0
    // un-arms it; the readback is the VIRTIO 1.2 §4.2.3.2 synchronization
    // point (forces the write to land before we free the pages).
    //
    // RW-7 R3-F2: QUEUE_READY=0 does NOT drain a DMA already in the device's
    // pipeline -- only a full device reset (virtio_reset, status=0) guarantees
    // no further used-ring write. A caller that frees these pages back to a
    // SHARED allocator MUST reset the device first: random.c does before its
    // destroy; the proc-death quiesce (virtio_mmio_reset_in_range) does for a
    // dying driver. This per-queue disarm is the belt to that suspenders.
    if (vq->dev) {
        virtio_mmio_write32(vq->dev, VIRTIO_MMIO_QUEUE_SEL, vq->qidx);
        virtio_mmio_write32(vq->dev, VIRTIO_MMIO_QUEUE_READY, 0);
        (void)virtio_mmio_read32(vq->dev, VIRTIO_MMIO_QUEUE_READY);
    }

    if (vq->desc_pages)  free_pages(vq->desc_pages, 0);
    if (vq->avail_pages) free_pages(vq->avail_pages, 0);
    if (vq->used_pages)  free_pages(vq->used_pages, 0);

    kfree(vq);
}

void virtio_vq_notify(struct virtio_virtqueue *vq) {
    if (!vq || !vq->dev) return;
    // Memory barrier so the avail ring writes are visible to the device.
    __asm__ __volatile__("dsb sy" ::: "memory");
    virtio_mmio_write32(vq->dev, VIRTIO_MMIO_QUEUE_NOTIFY, vq->qidx);
}
