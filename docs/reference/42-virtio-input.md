# Reference: virtio-input userspace driver (P4-K)

## Purpose

`/virtio-input` is the third composed userspace driver, after `/virtio-blk-probe` (P4-Ic5b2) and the virtio-net family (P4-Ja / Jb / Jc). Probe-only at v1.0: it proves the composed-hw-handle SVC substrate (MMIO + DMA, with IRQ available but not waited on) generalizes to the VIRTIO INPUT device class (DeviceID = 18).

What makes the INPUT class distinct from blk/net:
- **Selector-based device-specific config space** (VIRTIO 1.2 §5.8.4). The driver writes a `select` byte + a `subsel` byte; the device responds by populating `size` + the `u` union with the requested data. virtio-blk uses direct register layout; virtio-net uses a flat `mac[6] + status[2]` layout; virtio-input is the first device class to exercise the selector indirection.
- **RX-only mechanics on queue 0** (eventq). The driver pre-publishes N empty WRITE-able descriptors; the device fills them as events arrive. virtio-net-arp (P4-Jb) introduced RX, but on queue 0 alongside TX on queue 1. virtio-input uses *only* queue 0, and uses it RX-direction — symmetric to net's RX but a different device class and event size (8 B per event vs ~2 KiB per frame).
- **8-byte event records**: `struct virtio_input_event { __le16 type; __le16 code; __le32 value; }` per VIRTIO 1.2 §5.8.6.2 — a fixed-size record format that maps directly to Linux `struct input_event` (sans timestamp).

The probe lands the substrate generalization but does NOT consume events. QEMU virt has no host-side input injection in v1.0 (would require `-monitor send-key` or QMP wiring on every CI run); waiting on `t_irq_wait` would hang indefinitely. Event consumption is a future P4-K-events sub-chunk (or a Phase 8 Halcyon-prep deliverable when real input becomes load-bearing for the GUI).

## Public surface (driver crate)

The crate `usr/virtio-input/` builds the binary `/virtio-input`. It composes the existing hw-handle SVCs — `t_mmio_create` + `t_mmio_map` (P4-Ib + P4-Ic2), `t_dma_create` + `t_dma_map` (P4-Ic5b1b), with `t_irq_create` deferred — and is run from the kernel test `kernel/test/test_virtio_input_probe.c` via `rfork_with_caps(CAP_HW_CREATE)`.

No new syscalls. No new kernel surface. The chunk is non-audit-bearing at the per-syscall layer (all four hw-handle SVCs were audited at R9 / R10 / R11 / Ic5b1b); audit-bearing at the userspace-driver discipline layer (self-audit clean across the 10 adversarial categories listed in CLAUDE.md).

## Implementation

### Device discovery

`find_input_slot()` iterates all 32 virtio-mmio slots starting at PA 0x0a000000, stride 0x200, and matches on `(MagicValue == "virt") AND (DeviceID == 18)`. QEMU virt currently places `virtio-keyboard-device` at slot 29 (INTID 77) when added between `net_flags` and `virtio-rng-device,id=rng0` in the `qemu-system-aarch64` command-line — the same reverse-creation-order discipline established at P4-Ja for virtio-net. Slot assignment is informational; the probe scans all 32 slots either way.

### Selector-based config-space

VIRTIO 1.2 §5.8.4 lays out the device-specific config region (relative to MMIO offset 0x100):

| Offset | Field | Direction |
|---|---|---|
| 0 | u8 select | driver-writes |
| 1 | u8 subsel | driver-writes |
| 2 | u8 size | device-writes |
| 3..8 | u8 reserved[5] | — |
| 8..136 | union (string / bitmap / absinfo / devids) | device-writes |

To read a device property, the driver:
1. Writes `select` (and `subsel` for sub-selectors like EV_BITS' event-type sub-selector).
2. Reads `size` to discover how many bytes the device populated in `u`.
3. Reads up to `size` bytes from `u` (the union starts at offset 8).

The probe walks two selectors:
- **`select = INPUT_CFG_ID_NAME (0x01)`, `subsel = 0`** — returns a NUL-terminated string in `u.string`; the device name (QEMU's `virtio-keyboard-device` reports "QEMU Virtio Keyboard").
- **`select = INPUT_CFG_EV_BITS (0x11)`, `subsel = ev_type`** for `ev_type ∈ {EV_SYN=0, EV_KEY=1, EV_REL=2}` — returns a bitmap whose `size` byte indicates how many bytes the device set. A keyboard has `key_bits > 0 && rel_bits == 0`; a mouse has `key_bits > 0 && rel_bits > 0` (mice claim button codes in EV_KEY too).

The classification path uses `key_bits` and `rel_bits` together: if both are zero, the device is neither a keyboard nor a pointer; the probe FAILs. QEMU's `virtio-keyboard-device` reports `key_bits = 29` (29 bytes covering ~231 key codes) and `rel_bits = 0`.

### eventq configuration

The eventq is queue 0, RX-direction. The probe configures it with QUEUE_SIZE = 16 descriptors, each pointing at one 8-byte slot in a 128-byte event pool that lives inside a single 4 KiB DMA buffer:

```
DMA layout (single 4 KiB page):
  0x000 .. 0x100   desc[0..16]           (16 × 16 B)
  0x100 .. 0x200   avail header + ring   (4 + 32 + 2 = 38 B, padded)
  0x200 .. 0x300   used  header + ring   (4 + 128 + 2 = 134 B, padded)
  0x300 .. 0x380   event pool            (16 × 8 B = 128 B)
```

Pinned by compile-time `const _: () = { assert!(EVENT_POOL_OFF + 16 * 8 <= DMA_BUFSIZE) }` so any future layout change that breaks containment fails the userspace build.

`populate_eventq()`:
- Zeros the event pool so a stale read shows obvious zeros.
- Builds `desc[k]` for k in 0..16 with `addr = event_pool_pa + k*8`, `len = 8`, `flags = VIRTQ_DESC_F_WRITE`, `next = 0` (no chaining).
- Writes `avail.ring[k] = k` for k in 0..16.
- DSB; then `avail.idx = 16` (publishes all 16 buffers at once); DSB.

`avail.idx = 16` is set BEFORE the DRIVER_OK status write. The VIRTIO 1.2 spec doesn't require pre-population strictly before DRIVER_OK, but doing so means the device can fill events from the very first one it would generate, with no race between DRIVER_OK and the driver's first buffer publication.

### Init sequence

`init_device()` follows VIRTIO 1.2 §3.1.1 strictly:
1. `STATUS = 0` (RESET).
2. `STATUS |= ACKNOWLEDGE`.
3. `STATUS |= DRIVER`.
4. Read `DeviceFeatures[bank=1]`; require `VIRTIO_F_VERSION_1`. Write `DriverFeatures[bank=0] = 0` (decline everything — input has no device-specific feature bits to negotiate in VIRTIO 1.2 §5.8). Write `DriverFeatures[bank=1] = VIRTIO_F_VERSION_1_BIT_BANK1`.
5. `STATUS |= FEATURES_OK`; read back; FAIL on rejection.
6. `QUEUE_SEL = 0` (eventq); `QUEUE_NUM_MAX` check; `QUEUE_NUM = 16`; `QUEUE_DESC_*`, `QUEUE_DRIVER_*`, `QUEUE_DEVICE_*` PAs; `populate_eventq()`; `QUEUE_READY = 1`.
7. `STATUS |= DRIVER_OK`.

After step 7, the device is live. The probe reads `INPUT_CFG_ID_NAME` + `INPUT_CFG_EV_BITS` (steps that don't require any particular STATUS, but are conventionally done after DRIVER_OK so the device's responses are stable) and exits 0 if the classification passes.

## Data structures

### `struct virtio_input_event` (VIRTIO 1.2 §5.8.6.2)

```c
struct virtio_input_event {
    __le16 type;
    __le16 code;
    __le32 value;
};
```

Total 8 bytes. The probe doesn't define a Rust mirror of this struct because it doesn't consume events; the event pool is treated as raw bytes that the device may write but the driver never reads in this chunk.

### `struct irq_bench_shared` not applicable

This is a probe-only chunk; no shared region between kernel and userspace. The DMA buffer is fully owned by the userspace child.

## State machine

### VirtIO init (per device, mirrored across blk/net/input)

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
   configure queue(s) + populate
               ↓
           DRIVER_OK   (device is live)
               ↓
        config-space reads
               ↓
              exit
```

If any step fails, `STATUS |= FAILED` is written and the probe exits 1.

## Spec cross-reference

- **`scheduler.tla`** (I-9 NoMissedWakeup): not exercised by this chunk because no `t_irq_wait` call is made. The same wakeup discipline applies to future event consumption.
- **`handles.tla`** (I-2 caps monotonic, I-6 rights monotonic, HwHandleImpliesCap): the child holds CAP_HW_CREATE granted via `rfork_with_caps`; both `t_mmio_create` and `t_dma_create` reject without it (covered by existing tests).

## Tests

Suite: `kernel/test/test_virtio_input_probe.c::test_virtio_input_probe_rfork_with_caps`.
Registry: `userspace.virtio_input_probe_rfork_with_caps` in `kernel/test/test.c`.

Skips gracefully if:
- `/virtio-input` is absent from the ramfs (the userspace crate wasn't built).
- No virtio-mmio slot reports DeviceID = 18 (`tools/run-vm.sh` lacks `-device virtio-keyboard-device`, or `THYLACINE_NO_INPUT=1` is set).

On success, the boot log shows:

```
[test] userspace.virtio_input_probe_rfork_with_caps ... /virtio-input size=10944 bytes; input_dev pa=0x000000000a003a00 → rfork_with_caps(CAP_HW_CREATE)
    exec_setup ok entry=0x0000000000400000 sp=0x0000000080000000 caps=0x1 → userland_enter
virtio-input: slot=29 intid=77 name="QEMU Virtio Keyboard" name_len=21 key_bits=29 rel_bits=0
virtio-input: PASS — config-space + eventq init reached DRIVER_OK (slot=29 intid=77)
    /virtio-input reaped pid=1317 status=0 — selector-based config-space + eventq RX init end-to-end
```

## Error paths

| Path | Behavior |
|---|---|
| `/virtio-input` not in ramfs | Test skips with notice (PASS, no measurement) |
| No DeviceID=18 slot | Test skips with notice (PASS) |
| Userspace `t_mmio_create` fails | Userspace logs + `t_exits(1)`; kernel test FAILs |
| Userspace `t_dma_create` fails | Userspace logs + `t_exits(1)`; kernel test FAILs |
| VirtIO `FEATURES_OK` rejected | Userspace logs + `t_exits(1)` |
| Device name empty (selector mechanism not responding) | Userspace logs + `t_exits(1)` |
| Device claims neither EV_KEY nor EV_REL | Userspace logs + `t_exits(1)` |

## Performance characteristics

Probe wall-time is dominated by:
- 32-page MMIO claim loop (~32 × MMIO_CREATE + MMIO_MAP SVCs).
- VirtIO init (handful of MMIO writes + readbacks).
- Three EV_BITS selector queries (~12 MMIO accesses each).

Total: well under 5 ms on QEMU TCG. Boot-time impact is negligible compared to the existing virtio-{blk,net}* probes.

## Status

- **Landed**: P4-K substantive at commit `7a72617`. New `usr/virtio-input/` crate (~310 LOC Rust) + `kernel/test/test_virtio_input_probe.c` (~110 LOC C) + reference doc + `tools/run-vm.sh` wires `-device virtio-keyboard-device` + `usr/.cargo/config.toml` adds `-z max-page-size=4096` (bonus: shrinks every Rust userspace ELF by ~60 KiB). 240 → 241 tests; PASS × default + UBSan.
- **Exit criterion partial**: ROADMAP §6.2 `Userspace virtio-input: keyboard input from VirtIO console reaches user processes via /dev/cons` is NOT closed by this chunk — the `/dev/cons` integration depends on FS-namespace + 9P client (Phase 5+). What this chunk closes: the substrate-side proof that the INPUT device class composes the same hw-handle surface as blk/net.

## Known caveats

- **No event consumption**. The probe stops at DRIVER_OK + config-space classification. A keypress on a real keyboard (or a `send-key` injected via QMP) would arrive in the eventq, but no userspace process is waiting; the device's event would land in `used.ring[0]` and stall there. This is intentional for the probe scope; future P4-K-events lifts it.
- **No IRQ subscription**. `t_irq_create` is intentionally skipped to keep the probe minimal. Adding it for the probe-only path would just claim + drop the handle at process exit; the value lies in the wait-loop, which depends on event injection.
- **Hard-coded slot scan**. The probe scans all 32 virtio-mmio slots and matches DeviceID; the actual slot (29 on current QEMU config) is informational. A future DTB-driven device enumeration is Phase 5+.
- **`-z max-page-size=4096`** is now in `usr/.cargo/config.toml`. This affects ALL Rust userspace ELFs (shrinks each by ~60 KiB by closing rust-lld's default 64-KiB file-offset gap). The kernel ELF loader handles 4 KiB-aligned LOAD segments unchanged (PAGE_SIZE in `arch/arm64/mmu.c` is already 4 KiB). The pre-P4-K 96-KiB blob caps on other test binaries are now over-reservations; tightening them is a separate "image-discipline" sub-chunk that can recover hundreds of KiB of kernel .bss.

## Naming rationale

The crate, binary, and registry name all share the literal `virtio-input` token — the canonical VIRTIO spec name. No thematic rename proposed; the spec name is what auditors will scan for. The kernel test name (`test_virtio_input_probe_rfork_with_caps`) parallels `test_virtio_net_probe_rfork_with_caps` exactly — discipline over creativity.
