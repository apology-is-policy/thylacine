# Reference: virtio-gpu userspace driver (P4-L)

## Purpose

`/virtio-gpu` is the fourth composed userspace driver, after `/virtio-blk-probe` (P4-Ic5b2), the virtio-net family (P4-Ja / Jb / Jc), and `/virtio-input` (P4-K). Probe-only at v1.0: it proves the composed-hw-handle SVC substrate (MMIO + DMA + IRQ) generalizes to the VIRTIO GPU device class (DeviceID = 16) — the substrate gate for the Phase 8 Halcyon graphical shell.

What makes the GPU class distinct from blk/net/input:

- **Two virtqueues** (VIRTIO 1.2 §5.7.2): controlq (idx 0) for command/response traffic + cursorq (idx 1) for cursor updates. The probe is the first composed driver to configure more than one virtqueue. Every prior probe wrote `QUEUE_SEL = 0` exactly once (blk + input) or `QUEUE_SEL ∈ {0, 1}` for a single direction (net); virtio-gpu writes both selectors and brings both queues to `QUEUE_READY = 1` before `DRIVER_OK`. The cursorq stays empty in the probe (no avail.idx bumps); configuring it is necessary even when unused.
- **Command/response chain on controlq**: descriptor 0 = device-readable request header (24 B, `struct virtio_gpu_ctrl_hdr` per §5.7.6.6), descriptor 1 = device-writable response payload (408 B, `struct virtio_gpu_resp_display_info` per §5.7.6.1). Two-descriptor chain with `NEXT` linking; symmetric in shape to virtio-blk's 3-descriptor chain (req + data + status) but without the separate status byte — virtio-gpu folds status into `resp.hdr.type` (OK vs ERR variants).
- **Flat le32 config-space** at offset 0x100..0x110 (`events_read`, `events_clear`, `num_scanouts`, `num_capsets`). Distinct from virtio-input's selector-based config-space (§5.8.4) and from virtio-net's MAC+status flat struct (§5.1.4). No indirection: each field is read directly.

The probe lands the substrate generalization but does NOT configure any 2D resource. `VIRTIO_GPU_CMD_GET_DISPLAY_INFO` (§5.7.6.1) is the canonical "what scanouts does this device expose" command — used by every real virtio-gpu driver before any 2D surface setup. The probe issues it via controlq, waits for the IRQ, verifies `resp.hdr.type == VIRTIO_GPU_RESP_OK_DISPLAY_INFO` (= 0x1101), logs the per-scanout rectangles, and exits. Scanout setup (`RESOURCE_CREATE_2D`, `ATTACH_BACKING`, `TRANSFER_TO_HOST_2D`, `SET_SCANOUT`, `RESOURCE_FLUSH`) is a future P4-L-scanout sub-chunk and a Phase 8 Halcyon-prep deliverable.

## Public surface (driver crate)

The crate `usr/virtio-gpu/` builds the binary `/virtio-gpu`. It composes the existing hw-handle SVCs — `t_mmio_create` + `t_mmio_map` (P4-Ib + P4-Ic2), `t_dma_create` + `t_dma_map` (P4-Ic5b1b), `t_irq_create` + `t_irq_wait` (P4-G + P4-Ib) — and is run from the kernel test `kernel/test/test_virtio_gpu_probe.c` via `rfork_with_caps(CAP_HW_CREATE)`.

No new syscalls. No new kernel surface. The chunk is non-audit-bearing at the per-syscall layer (all four hw-handle SVCs were audited at R9 / R10 / R11 / Ic5b1b); audit-bearing at the userspace-driver discipline layer (self-audit clean across the 10 adversarial categories listed in CLAUDE.md).

## Implementation

### Device discovery

`find_gpu_slot()` iterates all 32 virtio-mmio slots starting at PA 0x0a000000, stride 0x200, and matches on `(MagicValue == "virt") AND (DeviceID == 16)`. QEMU virt currently places `virtio-gpu-device` at slot 28 (INTID 76) when added between the keyboard device and `virtio-rng-device,id=rng0` in the `qemu-system-aarch64` command-line. Slot assignment is informational; the probe scans all 32 slots either way.

### Two-virtqueue configuration

VIRTIO 1.2 §5.7.2 mandates that the GPU device exposes exactly two virtqueues:

| Index | Name | Purpose |
|---|---|---|
| 0 | controlq | 2D + 3D command/response traffic |
| 1 | cursorq | cursor updates (move + resource_set) |

The spec doesn't strictly require both be active before `DRIVER_OK`, but QEMU's virtio-gpu walks its queue list on the `DRIVER_OK` transition and treats an unset cursorq as a config error. Mirror Linux's `drm/virtio_gpu` init order:

1. `QUEUE_SEL = 0`; `QUEUE_NUM_MAX` check; `QUEUE_NUM = 16`; `QUEUE_DESC_*`, `QUEUE_DRIVER_*`, `QUEUE_DEVICE_*` PAs for controlq; `QUEUE_READY = 1`.
2. `QUEUE_SEL = 1`; same shape for cursorq (PAs pointing at the cursorq region of the DMA buffer; `avail.idx` left at 0 → device sees an empty queue).

The probe's `configure_queue()` helper factors out the per-queue MMIO writes. Both queues are configured BEFORE `STATUS |= DRIVER_OK`.

### controlq command/response

`submit_get_display_info()` builds a 2-descriptor chain in the controlq:

```
desc[0]: { addr = req_pa, len = 24, flags = NEXT, next = 1 }     (device reads)
desc[1]: { addr = resp_pa, len = 408, flags = WRITE, next = 0 }  (device writes)
```

The request header is `struct virtio_gpu_ctrl_hdr` (§5.7.6.6) populated with:

```c
struct virtio_gpu_ctrl_hdr {
    __le32 type;        // VIRTIO_GPU_CMD_GET_DISPLAY_INFO = 0x0100
    __le32 flags;       // 0
    __le64 fence_id;    // 0 (no fence)
    __le32 ctx_id;      // 0 (no 3D context)
    __u8   ring_idx;    // 0 (controlq)
    __u8   padding[3];
};
```

The response is the full `struct virtio_gpu_resp_display_info` (408 B = 24 B hdr + 16 × 24 B `display_one`). The driver pre-zeroes the response header so a missing device write surfaces as `type = 0` rather than uninitialized memory.

After writing descriptors + `avail.ring[0] = 0` + `avail.flags = 0`, a DSB orders against `avail.idx = 1`; another DSB orders before `QUEUE_NOTIFY = 0`. The device processes the request, writes the response, advances `used.idx`, and raises the IRQ.

### Wait + verify

`wait_and_verify()`:
1. `t_irq_wait(irq_handle)` — sleeps until the kernel's `kobj_irq_dispatch` increments `pending_count`.
2. Read `INTERRUPT_STATUS`; `INTERRUPT_ACK` writes the same bits back to deassert the line.
3. `used.idx == 1` check (one completion).
4. `used.ring[0].id == 0` check (descriptor head matches what we submitted).
5. `resp.hdr.type == VIRTIO_GPU_RESP_OK_DISPLAY_INFO` (= 0x1101) check — the load-bearing assertion. Any other value (e.g., `VIRTIO_GPU_RESP_ERR_*`) fails the probe.
6. Read `num_scanouts` + `num_capsets` from config space (informational).
7. Read `pmodes[0]` rect (width × height) + `enabled` flag (informational).

The probe doesn't fail on `pmodes[0].enabled == 0` or zero rect dimensions — QEMU virt with `-nographic` reports the device-side capability (`num_scanouts >= 1`) but the per-scanout `enabled` bit depends on whether a display backend is attached. The OK response type is the only load-bearing assertion.

## Data structures

### `struct virtio_gpu_ctrl_hdr` (VIRTIO 1.2 §5.7.6.6)

```c
struct virtio_gpu_ctrl_hdr {
    __le32 type;
    __le32 flags;
    __le64 fence_id;
    __le32 ctx_id;
    __u8   ring_idx;
    __u8   padding[3];
};
```

Total 24 bytes. Used as both request header (with type = command code) and response header (with type = OK / ERR response code). The probe doesn't define a Rust mirror — fields are written/read at fixed byte offsets via `write32`/`write64`/`write_u8`/`read32`.

### `struct virtio_gpu_resp_display_info` (VIRTIO 1.2 §5.7.6.1)

```c
struct virtio_gpu_rect {
    __le32 x, y, width, height;
};

struct virtio_gpu_display_one {
    struct virtio_gpu_rect r;
    __le32 enabled;
    __le32 flags;
};

#define VIRTIO_GPU_MAX_SCANOUTS 16

struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_display_one pmodes[VIRTIO_GPU_MAX_SCANOUTS];
};
```

Total 408 bytes = 24 + 16 × 24. The probe reads `hdr.type` + `pmodes[0].r.width` + `.height` + `.enabled` at known offsets.

### DMA layout (single 4 KiB page)

```
0x000 .. 0x100   controlq desc[0..16]   (16 × 16 B; only desc[0..2] used)
0x100 .. 0x200   controlq avail         (header + ring + used_event)
0x200 .. 0x300   controlq used          (header + ring + avail_event)
0x300 .. 0x400   cursorq desc[0..16]    (16 × 16 B; unused but configured)
0x400 .. 0x500   cursorq avail          (idx stays 0)
0x500 .. 0x600   cursorq used           (unused)
0x600 .. 0x620   request header         (24 B virtio_gpu_ctrl_hdr)
0x620 .. 0x7c0   response payload       (408 B virtio_gpu_resp_display_info)
```

Pinned by compile-time `const _: () = { assert!(RESP_OFF + GPU_RESP_DISPLAY_INFO_LEN <= DMA_BUFSIZE) }` so any future layout change that breaks containment fails the userspace build.

## State machine

### VirtIO init (per device, mirrored across blk/net/input/gpu)

```
              RESET
               ↓
            ACKNOWLEDGE
               ↓
             DRIVER
               ↓
   read DeviceFeatures[1]; verify VERSION_1
               ↓
       write DriverFeatures
               ↓
           FEATURES_OK  (verify readback)
               ↓
   configure controlq (Q_SEL=0, Q_NUM, ring PAs, Q_READY)
               ↓
   configure cursorq  (Q_SEL=1, same shape; ring stays empty)
               ↓
           DRIVER_OK   (device is live)
               ↓
   submit_get_display_info  (controlq desc chain + kick)
               ↓
   t_irq_wait + ACK + parse used.ring + verify resp.hdr.type
               ↓
              exit
```

If any step fails, `STATUS |= FAILED` is written and the probe exits 1.

## Spec cross-reference

- **`scheduler.tla`** (I-9 NoMissedWakeup): `t_irq_wait` follows the same wait-then-wake protocol audited at P4-G and proven at P4-Ic6 (HardwareWakeProgress fairness). The device's IRQ assertion ↔ wakeup(irq_handle) ↔ t_irq_wait return path is the same kobj_irq SVC surface used by virtio-blk-probe + virtio-net-{probe,arp,loop} + irq-bench.
- **`handles.tla`** (I-2 caps monotonic, I-6 rights monotonic, HwHandleImpliesCap): the child holds CAP_HW_CREATE granted via `rfork_with_caps`; all four hw-handle SVCs (`t_mmio_create` / `t_irq_create` / `t_dma_create` / their `_map`+`_wait` counterparts) reject without it (covered by existing P4-Ib + P4-Ic2 + P4-Ic5b1b + P4-Ic3 tests).

## Tests

Suite: `kernel/test/test_virtio_gpu_probe.c::test_virtio_gpu_probe_rfork_with_caps`.
Registry: `userspace.virtio_gpu_probe_rfork_with_caps` in `kernel/test/test.c`.

Skips gracefully if:
- `/virtio-gpu` is absent from the ramfs (the userspace crate wasn't built).
- No virtio-mmio slot reports DeviceID = 16 (`tools/run-vm.sh` lacks `-device virtio-gpu-device`, or `THYLACINE_NO_GPU=1` is set).

On success, the boot log shows:

```
[test] userspace.virtio_gpu_probe_rfork_with_caps ... /virtio-gpu size=11648 bytes; gpu_dev pa=0x000000000a003800 → rfork_with_caps(CAP_HW_CREATE)
    exec_setup ok entry=0x0000000000400000 sp=0x0000000080000000 caps=0x1 → userland_enter
virtio-gpu: slot=28 intid=76 num_scanouts=1 num_capsets=0 pmodes[0]=1280x800 enabled=1
virtio-gpu: PASS — controlq GET_DISPLAY_INFO round-trip ok (slot=28 intid=76)
    /virtio-gpu reaped pid=1318 status=0 — two-queue setup + controlq command/response end-to-end
```

## Error paths

| Path | Behavior |
|---|---|
| `/virtio-gpu` not in ramfs | Test skips with notice (PASS, no measurement) |
| No DeviceID=16 slot | Test skips with notice (PASS) |
| Userspace `t_mmio_create` fails | Userspace logs + `t_exits(1)`; kernel test FAILs |
| Userspace `t_irq_create` fails | Userspace logs + `t_exits(1)` |
| Userspace `t_dma_create` fails | Userspace logs + `t_exits(1)` |
| VirtIO `FEATURES_OK` rejected | Userspace logs + `t_exits(1)` |
| `QUEUE_NUM_MAX` below 16 for either queue | Userspace sets STATUS_FAILED + `t_exits(1)` |
| `t_irq_wait` returns error | Userspace logs + `t_exits(1)` |
| `used.idx != 1` after IRQ | Userspace logs + `t_exits(1)` |
| `used.ring[0].id != 0` | Userspace logs + `t_exits(1)` |
| `resp.hdr.type != OK_DISPLAY_INFO` | Userspace logs (with received type) + `t_exits(1)` |

## Performance characteristics

Probe wall-time is dominated by:
- 32-page MMIO claim loop (~32 × MMIO_CREATE + MMIO_MAP SVCs).
- VirtIO init (handful of MMIO writes + readbacks; cursorq adds ~6 register writes over the single-queue probes).
- One controlq round-trip: descriptor writes + 2 DSBs + Notify + IRQ wait + response parse.

Total: well under 5 ms on QEMU TCG. Boot-time impact is similar to the existing virtio-{blk,net}* probes (single-digit ms).

## Status

- **Landed**: P4-L substantive at commit `(pending)`. New `usr/virtio-gpu/` crate (~430 LOC Rust) + `kernel/test/test_virtio_gpu_probe.c` (~115 LOC C) + reference doc + `tools/run-vm.sh` wires `-device virtio-gpu-device`. 241 → 242 tests; PASS × default + UBSan.
- **Halcyon dependency closed (substrate-side)**: ROADMAP §6.2's GPU-related deliverables — "Userspace virtio-gpu driver" plus the substrate-side proof that the GPU device class composes the same hw-handle surface as blk/net/input — are now landed. The scanout-side deliverable (actually putting pixels on a framebuffer + observing them) remains open and is a Phase 8 Halcyon-prep sub-chunk.

## Known caveats

- **Probe-only scope**. The probe stops at `GET_DISPLAY_INFO` parsing. No 2D resource is created; no scanout backing is attached; no pixels are transferred. A real graphical driver would issue `RESOURCE_CREATE_2D` + `ATTACH_BACKING` + `TRANSFER_TO_HOST_2D` + `SET_SCANOUT` + `RESOURCE_FLUSH` — work deferred to Halcyon prep.
- **cursorq configured but unused**. The probe configures cursorq to make QEMU accept `DRIVER_OK` (QEMU rejects the transition if cursorq is left at `QUEUE_READY = 0`), but submits nothing to it. Future cursor-bearing drivers will populate it.
- **`pmodes[0].enabled` not load-bearing**. With QEMU's `-nographic` the per-scanout enabled bit may be 0 or 1 depending on the version's display-backend default. The probe logs the value but doesn't FAIL on either case; the load-bearing assertion is `resp.hdr.type == OK_DISPLAY_INFO`.
- **Decline EDID + VIRGL + RESOURCE_UUID + RESOURCE_BLOB + CONTEXT_INIT**. The probe accepts only `VIRTIO_F_VERSION_1`. Future scanout work may want EDID for monitor identity, RESOURCE_BLOB for the modern resource model, or VIRGL/CONTEXT_INIT for 3D — each opens a different request/response framing.
- **Slot allocation depends on `-device` ordering in `tools/run-vm.sh`**. QEMU virt assigns virtio-mmio slots in reverse-creation order; adding a new `-device` between gpu and rng would shift gpu's slot. The probe's all-slots scan is robust to this; the slot number in the boot log is informational.

## Naming rationale

The crate, binary, and registry name all share the literal `virtio-gpu` token — the canonical VIRTIO spec name. No thematic rename proposed; the spec name is what auditors will scan for. The kernel test name (`test_virtio_gpu_probe_rfork_with_caps`) parallels `test_virtio_net_probe_rfork_with_caps` and `test_virtio_input_probe_rfork_with_caps` exactly — discipline over creativity.
