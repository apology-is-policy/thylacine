// VirtIO core — MMIO transport + split virtqueue (P4-F).
//
// Per ARCHITECTURE.md §13 + ROADMAP §6.1 + VIRTIO 1.2 specification
// (https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html).
// v1.0 P4-F lands the MMIO transport + per-device probe + virtqueue
// allocation infrastructure. Userspace driver bindings + IRQ delivery
// are P4-G/I+; this header is the substrate they call into.
//
// Naming: we keep the canonical "virtio" / "vring" terminology because
// this is the wire-protocol layer — the spec is the contract, and
// renaming would obscure cross-implementation comparison. (Plan 9
// renames stick where Plan 9 invented the concept; on industry-spec
// surfaces the spec name wins. Same precedent as keeping "9P" + "qid".)

#ifndef THYLACINE_VIRTIO_H
#define THYLACINE_VIRTIO_H

#include <thylacine/types.h>

// =============================================================================
// MMIO register offsets — VIRTIO 1.2 §4.2.2.
// =============================================================================

#define VIRTIO_MMIO_MAGIC_VALUE           0x000   // R   "virt" LE = 0x74726976
#define VIRTIO_MMIO_VERSION               0x004   // R   1=legacy, 2=modern
#define VIRTIO_MMIO_DEVICE_ID             0x008   // R   0 if no device
#define VIRTIO_MMIO_VENDOR_ID             0x00c   // R
#define VIRTIO_MMIO_DEVICE_FEATURES       0x010   // R
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL   0x014   // W
#define VIRTIO_MMIO_DRIVER_FEATURES       0x020   // W
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL   0x024   // W
#define VIRTIO_MMIO_QUEUE_SEL             0x030   // W
#define VIRTIO_MMIO_QUEUE_NUM_MAX         0x034   // R
#define VIRTIO_MMIO_QUEUE_NUM             0x038   // W
#define VIRTIO_MMIO_QUEUE_READY           0x044   // RW  1 = queue armed
#define VIRTIO_MMIO_QUEUE_NOTIFY          0x050   // W   write queue index to ring bell
#define VIRTIO_MMIO_INTERRUPT_STATUS      0x060   // R
#define VIRTIO_MMIO_INTERRUPT_ACK         0x064   // W
#define VIRTIO_MMIO_STATUS                0x070   // RW  device status bits below
#define VIRTIO_MMIO_QUEUE_DESC_LOW        0x080   // W   PA[31:0] of descriptor table
#define VIRTIO_MMIO_QUEUE_DESC_HIGH       0x084   // W   PA[63:32]
#define VIRTIO_MMIO_QUEUE_DRIVER_LOW      0x090   // W   PA[31:0] of available ring
#define VIRTIO_MMIO_QUEUE_DRIVER_HIGH     0x094   // W   PA[63:32]
#define VIRTIO_MMIO_QUEUE_DEVICE_LOW      0x0a0   // W   PA[31:0] of used ring
#define VIRTIO_MMIO_QUEUE_DEVICE_HIGH     0x0a4   // W   PA[63:32]
#define VIRTIO_MMIO_CONFIG_GENERATION     0x0fc   // R
#define VIRTIO_MMIO_CONFIG                0x100   // RW  device-specific

// Magic value: ASCII "virt" stored little-endian.
#define VIRTIO_MMIO_MAGIC                 0x74726976u

// Supported MMIO transport versions. v1.0 P4-F supports modern (v2)
// only; legacy v1 has different ring address registers + 32-bit PFN.
#define VIRTIO_MMIO_VERSION_LEGACY        1u
#define VIRTIO_MMIO_VERSION_MODERN        2u

// =============================================================================
// Device IDs — VIRTIO 1.2 §5.
// =============================================================================

#define VIRTIO_DEVICE_ID_INVALID          0
#define VIRTIO_DEVICE_ID_NET              1
#define VIRTIO_DEVICE_ID_BLOCK            2
#define VIRTIO_DEVICE_ID_CONSOLE          3
#define VIRTIO_DEVICE_ID_RNG              4
#define VIRTIO_DEVICE_ID_BALLOON          5
#define VIRTIO_DEVICE_ID_SCSI             8
#define VIRTIO_DEVICE_ID_GPU              16
#define VIRTIO_DEVICE_ID_INPUT            18
#define VIRTIO_DEVICE_ID_INPUT            18
#define VIRTIO_DEVICE_ID_VSOCK            19

// =============================================================================
// Device status bits — VIRTIO 1.2 §2.1.
// =============================================================================

#define VIRTIO_STATUS_ACKNOWLEDGE         (1u << 0)   // 0x01: guest noticed device
#define VIRTIO_STATUS_DRIVER              (1u << 1)   // 0x02: guest knows how to drive
#define VIRTIO_STATUS_DRIVER_OK           (1u << 2)   // 0x04: driver setup complete
#define VIRTIO_STATUS_FEATURES_OK         (1u << 3)   // 0x08: features negotiated
#define VIRTIO_STATUS_DEVICE_NEEDS_RESET  (1u << 6)   // 0x40: device gave up
#define VIRTIO_STATUS_FAILED              (1u << 7)   // 0x80: guest gave up

// =============================================================================
// vring descriptor flags — VIRTIO 1.2 §2.7.
// =============================================================================

#define VRING_DESC_F_NEXT                 (1u << 0)   // descriptor chains
#define VRING_DESC_F_WRITE                (1u << 1)   // device-writable buffer
#define VRING_DESC_F_INDIRECT             (1u << 2)   // points to indirect descs

// =============================================================================
// Split virtqueue layout — VIRTIO 1.2 §2.7. Every type is little-endian
// on the wire; ARM64 matches that, so we use plain u8/u16/u32/u64.
// =============================================================================

struct vring_desc {
    u64 addr;       // guest-physical address of buffer
    u32 len;        // buffer size in bytes
    u16 flags;      // VRING_DESC_F_*
    u16 next;       // index of next descriptor in chain
};

_Static_assert(sizeof(struct vring_desc) == 16,
               "vring_desc pinned at 16 bytes per VIRTIO 1.2 §2.7.5");

struct vring_avail {
    u16 flags;
    u16 idx;        // monotonic write counter (mod 65536)
    u16 ring[];     // queue_size entries; trailing le16 used_event
};

struct vring_used_elem {
    u32 id;         // start of used descriptor chain
    u32 len;        // total bytes written by the device
};

_Static_assert(sizeof(struct vring_used_elem) == 8,
               "vring_used_elem pinned at 8 bytes per VIRTIO 1.2 §2.7.8");

struct vring_used {
    u16 flags;
    u16 idx;        // monotonic write counter (mod 65536)
    struct vring_used_elem ring[];
    /* trailing le16 avail_event */
};

// =============================================================================
// Public types.
// =============================================================================

// Queue size for v1.0 P4-F. Conservative 64-entry default; many devices
// expose much larger QueueNumMax (e.g., virtio-blk on QEMU exposes 256).
// We pick the smaller of (QueueNumMax, this) at virtio_virtqueue_create
// — keeps the per-queue memory footprint at 3 × 4 KiB pages.
#define VIRTIO_VQ_NUM_DEFAULT             64u

// Per-MMIO-device handle. Populated by virtio_init at boot; immutable
// thereafter (the underlying device may have its features renegotiated
// + queues reconfigured by individual drivers, but the transport
// identity stays fixed).
struct virtio_mmio_dev {
    void   *base;        // KVA of the MMIO range (mapped via mmu_map_mmio)
    paddr_t pa;          // PA of the MMIO range (for diagnostics)
    size_t  size;        // MMIO range size in bytes
    u32     version;     // VIRTIO_MMIO_VERSION value
    u32     device_id;   // VIRTIO_DEVICE_ID_*; 0 if slot empty
    u32     vendor_id;
};

// Per-virtqueue handle. Owned by the calling driver; must be freed via
// virtio_virtqueue_destroy. Three ring regions allocated as separate
// page-aligned chunks (modern MMIO transport requires QueueDesc /
// QueueDriver / QueueDevice to point at independent addresses).
struct virtio_virtqueue {
    struct virtio_mmio_dev *dev;
    u32                      qidx;        // queue selector
    u32                      size;        // negotiated queue size

    struct vring_desc       *desc;        // descriptor table KVA
    struct vring_avail      *avail;       // available ring KVA
    struct vring_used       *used;        // used ring KVA

    paddr_t                  desc_pa;     // PAs (written to MMIO regs)
    paddr_t                  avail_pa;
    paddr_t                  used_pa;

    struct page             *desc_pages;  // for free_pages
    struct page             *avail_pages;
    struct page             *used_pages;
};

// =============================================================================
// API.
// =============================================================================

// Bring up VirtIO. Walks DTB for "virtio,mmio" nodes; per node, maps
// MMIO via mmu_map_mmio + reads MagicValue / Version / DeviceID /
// VendorID; populates g_virtio_mmio_devs[]. Idempotent guard extincts
// on re-call. Must run after slub_init + dev_init (since the MMIO
// mapping uses the kernel direct map which is set up earlier).
void virtio_init(void);

// Number of MMIO devices probed (whether or not they have an attached
// device — slots with DeviceID=0 are still present in the count).
int virtio_mmio_dev_count(void);

// Get the idx-th probed MMIO device. Returns NULL if idx out of range.
struct virtio_mmio_dev *virtio_mmio_dev_get(int idx);

// Find the first MMIO device with the given DeviceID, or NULL.
struct virtio_mmio_dev *virtio_mmio_find_by_device_id(u32 device_id);

// MMIO register read/write helpers. Memory-mapped IO is volatile; the
// helpers use volatile loads/stores to defeat compiler reordering.
u32  virtio_mmio_read32(struct virtio_mmio_dev *dev, u32 off);
void virtio_mmio_write32(struct virtio_mmio_dev *dev, u32 off, u32 val);

// Status register helpers. Set, clear, and check status bits per the
// VIRTIO 1.2 §2.1 driver state machine.
u32  virtio_get_status(struct virtio_mmio_dev *dev);
void virtio_set_status(struct virtio_mmio_dev *dev, u32 status);
void virtio_add_status(struct virtio_mmio_dev *dev, u32 bits);
void virtio_reset(struct virtio_mmio_dev *dev);

// RW-7 R3-F1: reset every probed MMIO transport whose register slot falls
// within [pa, pa+size). The proc-death device-quiesce hook (kernel/proc.c)
// calls this for each KObj_MMIO a dying driver held, BEFORE the driver's
// KObj_DMA pages are freed back to the buddy allocator -- a still-armed
// device would otherwise DMA a virtqueue completion into recycled memory
// (silent cross-Proc corruption; I-7 device-stop clause unmet on abnormal
// death). Resetting a slot the driver never armed is a harmless status
// write; the page-exclusive KObj_MMIO claim (mmio_handle.c overlap
// rejection) guarantees no other live driver owns a slot in the range.
// Returns the number of transports reset.
int virtio_mmio_reset_in_range(u64 pa, size_t size);

// Standard initialization sequence per VIRTIO 1.2 §3.1.1 steps 1-5:
//   reset → ACKNOWLEDGE → DRIVER → (caller selects features here) →
//   FEATURES_OK → re-read STATUS to verify FEATURES_OK still set.
//
// `want_features_lo` is the 32-bit feature mask the caller wants
// (legacy device features bank 0). At v1.0 P4-F we don't yet support
// 64-bit feature masks via DEVICE_FEATURES_SEL=1; that lands when a
// driver actually needs it.
//
// Returns true on success (FEATURES_OK still set after re-read);
// false on negotiation failure (device cleared FEATURES_OK, indicating
// caller's feature set is unacceptable).
bool virtio_negotiate_features(struct virtio_mmio_dev *dev,
                                u32 want_features_lo);

// Negotiated split-virtqueue size for a device-reported QueueNumMax:
// min(num_max, VIRTIO_VQ_NUM_DEFAULT), rejected to 0 when num_max is 0 or
// the clamped result is not a power of two. The split-ring index
// arithmetic masks with the size, so a non-pow2 size (a malformed/hostile
// device -- VIRTIO 1.2 §2.7 mandates QueueNumMax be a power of 2) is
// unsupported rather than armed (RW-7 R3-F6). virtio_virtqueue_create
// returns NULL when this returns 0.
u32 virtio_vq_size_for(u32 num_max);

// Allocate + register a virtqueue at queue index `qidx`. Steps:
//   1. Write QUEUE_SEL = qidx
//   2. Read QUEUE_NUM_MAX; pick min(QUEUE_NUM_MAX, VIRTIO_VQ_NUM_DEFAULT)
//   3. Allocate desc / avail / used as separate page-aligned chunks
//   4. Write QUEUE_NUM, QUEUE_DESC_LOW/HIGH, QUEUE_DRIVER_LOW/HIGH,
//      QUEUE_DEVICE_LOW/HIGH
//   5. Write QUEUE_READY = 1
//
// Returns NULL on:
//   - dev NULL or invalid
//   - QUEUE_NUM_MAX == 0 (queue index not supported by device)
//   - alloc_pages OOM for any of the three regions
//
// Caller frees via virtio_virtqueue_destroy.
struct virtio_virtqueue *virtio_virtqueue_create(struct virtio_mmio_dev *dev,
                                                  u32 qidx);

// Tear down a virtqueue. Writes QUEUE_READY = 0 + frees all three
// ring regions. Safe to call with NULL.
void virtio_virtqueue_destroy(struct virtio_virtqueue *vq);

// Notify the device that the available ring has new entries. Writes
// QUEUE_NOTIFY = qidx. Must be preceded by a memory barrier so the
// avail-ring writes are visible to the device.
void virtio_vq_notify(struct virtio_virtqueue *vq);

#endif  // THYLACINE_VIRTIO_H
