# 35 — VirtIO core: MMIO transport + split virtqueue (P4-F)

The kernel's hardware-I/O substrate. Per ARCH §13 + ROADMAP §6.1 + VIRTIO 1.2 specification (https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html). v1.0 P4-F lands the MMIO transport probe, register-access primitives, status state machine, and split virtqueue allocation. IRQ delivery (P4-G) and userspace driver bindings (P4-I+) are downstream chunks calling into this header.

---

## Purpose

VirtIO is the universal paravirtualization protocol — every QEMU-emulated device speaks it (block, net, input, gpu, console, rng), and physical hardware vendors increasingly support it. Building the kernel's virtio core right is the prerequisite for **every** v1.0 driver: the userspace virtio-blk + virtio-net + virtio-input + virtio-gpu drivers (P4-I/J/K/L) all share this transport.

The split virtqueue is the data path — three independent rings (descriptor table, available ring, used ring) the driver populates and the device consumes. Per VIRTIO 1.2 §2.7.4 the modern transport requires the three rings at independent physical addresses; v1.0 P4-F allocates a fresh page per ring.

---

## Public API — `<thylacine/virtio.h>`

```c
// Transport identity (probed at boot).
struct virtio_mmio_dev {
    void   *base;        // KVA of the MMIO range
    paddr_t pa;          // PA of the MMIO range
    size_t  size;
    u32     version;     // VIRTIO_MMIO_VERSION_LEGACY=1 or _MODERN=2
    u32     device_id;   // VIRTIO_DEVICE_ID_*; 0 if slot empty
    u32     vendor_id;
};

// Per-virtqueue handle.
struct virtio_virtqueue {
    struct virtio_mmio_dev *dev;
    u32                      qidx;
    u32                      size;
    struct vring_desc       *desc;
    struct vring_avail      *avail;
    struct vring_used       *used;
    paddr_t                  desc_pa, avail_pa, used_pa;
    struct page             *desc_pages, *avail_pages, *used_pages;
};

void virtio_init(void);
int  virtio_mmio_dev_count(void);
struct virtio_mmio_dev *virtio_mmio_dev_get(int idx);
struct virtio_mmio_dev *virtio_mmio_find_by_device_id(u32 device_id);

u32  virtio_mmio_read32(struct virtio_mmio_dev *dev, u32 off);
void virtio_mmio_write32(struct virtio_mmio_dev *dev, u32 off, u32 val);
u32  virtio_get_status(struct virtio_mmio_dev *dev);
void virtio_set_status(struct virtio_mmio_dev *dev, u32 status);
void virtio_add_status(struct virtio_mmio_dev *dev, u32 bits);
void virtio_reset(struct virtio_mmio_dev *dev);
bool virtio_negotiate_features(struct virtio_mmio_dev *dev, u32 want_features_lo);

struct virtio_virtqueue *virtio_virtqueue_create(struct virtio_mmio_dev *dev, u32 qidx);
void virtio_virtqueue_destroy(struct virtio_virtqueue *vq);
void virtio_vq_notify(struct virtio_virtqueue *vq);
```

### Vring types (VIRTIO 1.2 §2.7)

```c
struct vring_desc {            // 16 bytes; pinned by _Static_assert
    u64 addr;
    u32 len;
    u16 flags;                  // VRING_DESC_F_{NEXT,WRITE,INDIRECT}
    u16 next;
};

struct vring_avail {
    u16 flags;
    u16 idx;
    u16 ring[];                 // queue_size entries
    /* trailing le16 used_event */
};

struct vring_used_elem {       // 8 bytes; pinned by _Static_assert
    u32 id;
    u32 len;
};

struct vring_used {
    u16 flags;
    u16 idx;
    struct vring_used_elem ring[];
    /* trailing le16 avail_event */
};
```

ARM64 is little-endian; vring fields map directly to plain u8/u16/u32/u64 (no byte-swap helper needed).

---

## Boot flow

```
QEMU virt provides 32 virtio-mmio slots in DTB.
Test config adds: -device virtio-rng-device,id=rng0
                  (attaches one slot with DeviceID=4).
   ↓
boot_main: dtb_init parses DTB → virtio,mmio nodes visible.
   ↓
phys_init: zone init runs (P3-Bb direct map covers all PA so MMIO
           ranges are reachable via mmu_map_mmio + vmalloc).
   ↓
dev_init: bestiary Devs registered (8 Devs).
   ↓
virtio_init:
   - dtb_for_each_compat_reg("virtio,mmio", probe_cb, NULL)
   - per slot: page-align the slot PA + map via mmu_map_mmio
               + read MagicValue / Version / DeviceID / VendorID
   - record into g_virtio_mmio_devs[]
   - print "virtio: N MMIO slots probed (M with attached devices, S
            skipped due to magic mismatch or cap)"
```

QEMU virt's virtio-mmio slots are 0x200 (512) bytes apart starting at 0x0a000000 — only every 8th slot starts a fresh 4 KiB page. The probe page-aligns each slot's PA, calls `mmu_map_mmio(page_pa, page_span_rounded_up)`, and computes the slot's KVA as `page_kva + (slot_pa - page_pa)`. Multiple slots within the same 4 KiB page yield separate KVAs (mmu_map_mmio's documented "same args → different kvas, both mapping same PA" behavior); v1.0 acceptable, future optimization could share KVAs.

---

## Driver state machine (VIRTIO 1.2 §3.1.1)

The device-init handshake is a closed sequence:

```
1. RESET           — write STATUS = 0
2. ACKNOWLEDGE     — STATUS |= ACKNOWLEDGE  (guest noticed)
3. DRIVER          — STATUS |= DRIVER       (guest knows how to drive)
4. negotiate features:
   - read DEVICE_FEATURES (bank 0)
   - write DRIVER_FEATURES = (want_lo & device_lo)
   - bank 1 negotiated to 0 at v1.0
5. FEATURES_OK     — STATUS |= FEATURES_OK
   - re-read STATUS; if FEATURES_OK still set, negotiation accepted;
     if cleared, the device rejected the proposed feature set
6. (driver sets up virtqueues here)
7. DRIVER_OK       — STATUS |= DRIVER_OK    (driver setup complete)
```

`virtio_negotiate_features(dev, want_lo)` runs steps 1-5; the caller is responsible for steps 6-7.

If the device sets `DEVICE_NEEDS_RESET` (bit 6) at any point, the driver MUST go back to step 1. v1.0 P4-F doesn't yet handle the device-needs-reset signal — callers won't see it because virtio_negotiate_features is the only state-machine wrapper at this level, and rng's device-features set is acceptable.

---

## Virtqueue allocation

`virtio_virtqueue_create(dev, qidx)`:

1. Write `QUEUE_SEL = qidx` (selects which queue's registers are live).
2. Read `QUEUE_NUM_MAX`. Zero ⇒ this queue index unsupported by the device. Pick `min(QUEUE_NUM_MAX, VIRTIO_VQ_NUM_DEFAULT=64)`.
3. Allocate three page-aligned regions (one page each at `size <= 64`):
   - desc table: 16 × size bytes
   - avail ring: 6 + 2×size bytes (header + ring + used_event)
   - used ring: 6 + 8×size bytes (header + ring + avail_event)
4. Write `QUEUE_NUM = size`, `QUEUE_DESC_LOW/HIGH = desc_pa`,
   `QUEUE_DRIVER_LOW/HIGH = avail_pa`, `QUEUE_DEVICE_LOW/HIGH = used_pa`.
5. Write `QUEUE_READY = 1`.

Allocation rolls back cleanly on any partial failure. The struct virtio_virtqueue is kmalloc'd; kfree on destroy. Per-region pages allocated via `alloc_pages(0, KP_ZERO)`; free_pages on destroy.

`virtio_virtqueue_destroy` writes `QUEUE_READY = 0` (under the same QUEUE_SEL) before freeing — prevents the device from posting completions to about-to-be-freed memory.

---

## Implementation

| File | LOC | Scope |
|---|---|---|
| `kernel/include/thylacine/virtio.h` | ~190 | Register offsets + device IDs + status bits + vring types + struct virtio_mmio_dev / virtio_virtqueue + API. _Static_assert pins on vring_desc (16 B) + vring_used_elem (8 B). |
| `kernel/virtio.c` | ~270 | g_virtio_mmio_devs[] table, MMIO read/write helpers, status state machine, DTB probe via dtb_for_each_compat_reg, virtqueue create/destroy/notify. |
| `lib/dtb.c` (+ `dtb_for_each_compat_reg`) | ~60 | Multi-match enumeration. Walks DTB structure block; per-match callback gets (match_idx, reg_base, reg_size). |
| `<thylacine/dtb.h>` | +5 | dtb_compat_cb typedef + extern decl. |
| `tools/run-vm.sh` | +1 | `-device virtio-rng-device,id=rng0` flag. |
| `kernel/test/test_virtio.c` | ~140 | 7 tests covering probe, magic, version range, RNG presence, negotiation, virtqueue alloc, lookup. |

---

## Spec cross-reference

P4-F is impl-only — no new TLA+ module. The VirtIO spec is the contract; the impl is a direct reading. Per ARCH §25.4 the VirtIO core is an audit-trigger surface: the closing audit (P4-Z) will prosecute the transport against:

- Magic / version / device-id read consistency
- Status state machine correctness vs VIRTIO 1.2 §3.1.1
- Memory barrier discipline at virtio_vq_notify (currently `dsb sy` — full barrier; could relax to `dsb st` per spec but `sy` is conservative)
- Virtqueue address writes vs the 32/64-bit cell split (modern transport)
- Reset semantics on virtqueue_destroy (QUEUE_READY=0 before free)
- Feature-bank-1 behavior (we negotiate to 0 explicitly)

---

## Tests

`kernel/test/test_virtio.c` — 7 tests:

| Test | Covers |
|---|---|
| `virtio.mmio_probe` | dev_count >= 1, <= 32 (cap respected). |
| `virtio.magic_value` | First slot's `MagicValue == 0x74726976` ("virt"). |
| `virtio.version_modern` | Every slot's version is 1 (legacy) or 2 (modern). |
| `virtio.rng_present` | At least one slot has DeviceID=4 (`-device virtio-rng-device`). |
| `virtio.negotiate_features_smoke` | Reset → ACK → DRIVER → FEATURES_OK accepted with empty driver mask. |
| `virtio.virtqueue_alloc_destroy` | Create on rng queue 0; size in (0, 64]; rings page-aligned; PAs distinct; destroy clean. |
| `virtio.find_by_device_id` | Lookup of unattached id returns NULL; out-of-range idx returns NULL. |

---

## Status

| Component | State |
|---|---|
| `kernel/include/thylacine/virtio.h` | Landed (P4-F) |
| `kernel/virtio.c` (probe + register access + status state machine + virtqueue alloc) | Landed (P4-F) |
| `dtb_for_each_compat_reg` (multi-match DTB enumeration) | Landed (P4-F) |
| `tools/run-vm.sh -device virtio-rng-device` | Landed (P4-F) |
| In-kernel tests | 7 covering probe + register reads + virtqueue alloc end-to-end |
| Boot banner: `virtio: N slots probed (M attached, S skipped)` | Landed (P4-F) |
| IRQ forwarding to drivers (irqfwd) | Held to P4-G |
| PCI transport (kernel/virtio_pci.c) | Held to P4-H |
| Userspace driver bindings (handle table → driver process) | Held to P4-I+ |
| 64-bit feature bank 1 negotiation | Held to first driver that needs it (likely virtio-net for VIRTIO_NET_F_MQ etc.) |
| Indirect descriptors (VRING_DESC_F_INDIRECT) | Held — current impl supports only single-descriptor + chained-via-NEXT |
| Event-index suppression (used_event / avail_event) | Held — current impl ignores the trailing 16-bit fields |
| Modern transport's SHM regions (VIRTIO 1.2 §4.2.2.10) | Held — only block / GPU need it |

---

## Known caveats / footguns

### Page-aligned mapping wastes vmalloc KVA

QEMU virt's 32 virtio-mmio slots are 0x200 bytes apart, i.e., 8 slots per 4 KiB page. Our probe calls `mmu_map_mmio` per slot; the KVA allocator gives a fresh KVA each call, so 32 mappings consume 32 KVAs (32 × 4 KiB = 128 KiB of vmalloc space) for what could fit in 4 KVAs (4 × 4 KiB = 16 KiB) if shared. Acceptable at v1.0 (vmalloc has GiBs of headroom); future optimization is to detect "this PA's page already mapped" + return the cached KVA.

### Virtqueue size capped at 64

`VIRTIO_VQ_NUM_DEFAULT = 64`. Fits each ring in one 4 KiB page. Many devices expose much larger `QUEUE_NUM_MAX` (block: 256, net: 1024). Larger queues need multi-page allocation; v1.0 P4-F's single-page-per-ring path stays simple. Bump or switch to dynamic when a driver demands it.

### `dsb sy` is conservative

`virtio_vq_notify` uses `dsb sy` (full system barrier) before the QUEUE_NOTIFY write. Per VIRTIO 1.2 §2.7.13.2, the required barrier is "a memory barrier that ensures the available ring writes are visible to the device" — `dsb st` (store barrier) suffices. `sy` is safe but possibly slower. Future audit may relax.

### No legacy v1 transport support

The `QUEUE_DESC_LOW/HIGH` / `QUEUE_DRIVER_LOW/HIGH` / `QUEUE_DEVICE_LOW/HIGH` register split is modern (v2) only. Legacy transport uses a different layout (`QueuePFN` with implicit page-shifted PA). v1.0 P4-F doesn't support legacy; if QEMU virt presents a legacy slot the virtqueue_alloc path writes wrong registers (the writes land on reserved offsets; device may ignore or report DEVICE_NEEDS_RESET).

### Queue setup before `DRIVER_OK`

Per VIRTIO 1.2 §3.1.1 step 6 the virtqueues are configured AFTER FEATURES_OK is accepted but BEFORE DRIVER_OK is set. Our `virtio_virtqueue_create` matches this — but doesn't check the device's STATUS state. A caller that creates a virtqueue without first calling `virtio_negotiate_features` violates the spec; the device may ignore the writes. Tests verify the proper-order path; future hardening adds an internal state check.

### QUEUE_READY=0 reset on destroy

`virtio_virtqueue_destroy` writes `QUEUE_READY=0` before freeing, but doesn't issue a barrier between the write and the free. On strongly-ordered architectures this is safe; on the loose ARM64 ordering, an aggressive compiler optimization could conceivably reorder the QUEUE_READY write past the free_pages calls. v1.0 the `kfree(vq)` immediately after acts as a compiler barrier in practice; future hardening might add an explicit `dsb sy`.

---

## References

- VIRTIO 1.2 specification — the contract: https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html
- `docs/ARCHITECTURE.md` §13 — MMIO + VirtIO design.
- `docs/ROADMAP.md` §6.1 — Phase 4 deliverables.
- `docs/reference/30-dev-spoor.md` — Dev vtable (this Dev's substrate when virtio devices get bestiary entries via userspace drivers, P4-I+).
- `arch/arm64/mmu.h::mmu_map_mmio` — vmalloc-style MMIO mapping.
- `lib/dtb.c::dtb_for_each_compat_reg` — DTB multi-match enumeration helper added in P4-F.
- `mm/phys.h::alloc_pages` — physical page allocator used for ring backing.
