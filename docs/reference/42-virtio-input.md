# Reference: virtio-input userspace driver (P4-K + P4-K-events)

## Purpose

`/virtio-input` is the third composed userspace driver, after `/virtio-blk-probe` (P4-Ic5b2) and the virtio-net family (P4-Ja / Jb / Jc). It proves the composed-hw-handle SVC substrate (MMIO + DMA + IRQ) generalizes to the VIRTIO INPUT device class (DeviceID = 18) and additionally consumes injected host-side events end-to-end.

What makes the INPUT class distinct from blk/net:
- **Selector-based device-specific config space** (VIRTIO 1.2 §5.8.4). The driver writes a `select` byte + a `subsel` byte; the device responds by populating `size` + the `u` union with the requested data. virtio-blk uses direct register layout; virtio-net uses a flat `mac[6] + status[2]` layout; virtio-input is the first device class to exercise the selector indirection.
- **RX-only mechanics on queue 0** (eventq). The driver pre-publishes N empty WRITE-able descriptors; the device fills them as events arrive. virtio-net-arp (P4-Jb) introduced RX, but on queue 0 alongside TX on queue 1. virtio-input uses *only* queue 0, and uses it RX-direction — symmetric to net's RX but a different device class and event size (8 B per event vs ~2 KiB per frame).
- **8-byte event records**: `struct virtio_input_event { __le16 type; __le16 code; __le32 value; }` per VIRTIO 1.2 §5.8.6.2 — a fixed-size record format that maps directly to Linux `struct input_event` (sans timestamp).

The two-stage scope:

- **P4-K** (probe; previously landed): RESET → DRIVER_OK → read NAME + EV_BITS for classification. Reaches `DRIVER_OK` but does not consume events.
- **P4-K-events** (this chunk): adds event consumption via `t_irq_create` + bounded busy-poll on `used.idx`. The kernel test pre-fires SPI 77 before spawning the child (P4-Ic-latency / P4-Ic5-IRQ-probe pre-pend pattern); the child consumes that one wake via a single `t_irq_wait`, prints the `AWAITING_QMP_KEY` sentinel, then transitions to a bounded busy-poll. `tools/test.sh` spawns a background QMP injector that observes the sentinel in the boot log and issues `send-key` with qcode `a`; QEMU's virtio-keyboard-device fills the eventq with `{type=EV_KEY, code=KEY_A=30, value=1}` (press) + SYN + `{value=0}` (release) + SYN; the driver drains, validates, recycles descriptors, and exits PASS. Without QMP injection (`tools/run-vm.sh` interactive run, or `THYLACINE_INPUT_INJECT=0`), the busy-poll caps out at 200M iterations (~few seconds on QEMU TCG) and exits SKIP — boot continues either way.

## Public surface (driver crate)

The crate `usr/virtio-input/` builds the binary `/virtio-input`. It composes the existing hw-handle SVCs — `t_mmio_create` + `t_mmio_map` (P4-Ib + P4-Ic2), `t_dma_create` + `t_dma_map` (P4-Ic5b1b), `t_irq_create` + `t_irq_wait` (P4-G + P4-Ib) — and is run from the kernel test `kernel/test/test_virtio_input_probe.c` via `rfork_with_caps(CAP_HW_CREATE)`.

No new syscalls. No new kernel surface. Non-audit-bearing at the per-syscall layer (all four hw-handle SVCs were audited at R9 / R10 / R11 / Ic5b1b); audit-bearing at the userspace-driver discipline layer (self-audit clean across the 10 adversarial categories listed in CLAUDE.md).

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

### Event drain shape (P4-K-events)

The eventq's used ring carries `struct virtq_used_elem { __le32 id; __le32 len; }` per VIRTIO 1.2 §2.7.8 — 8 bytes per entry, with `id` = descriptor head (= descriptor index for unchained eventq descriptors) and `len` = bytes written by the device (= `VIRTIO_INPUT_EVENT_LEN` = 8). The driver:

1. Reads `used.idx`. If unchanged from `last_used_idx`, busy-polls (yields per iteration).
2. For each new entry, parses the 8-byte event at `desc[id].addr` (which == `event_pool_pa + id * 8`).
3. Recycles the descriptor head by re-publishing `id` to `avail.ring[avail.idx % QUEUE_SIZE]` and bumping `avail.idx` with the standard DSB-before/after fencing.

Re-publishing avoids running out of buffers if the host injects more events than the queue size; though for a single `send-key` (4 events) this isn't load-bearing, the discipline matches virtio-net-loop's RX recycling.

## State machine

### VirtIO init + P4-K-events event consumption

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
   configure eventq (Q_SEL=0, ring PAs, populate 16 RX descs)
               ↓
           DRIVER_OK   (device is live)
               ↓
        config-space reads (NAME + EV_BITS)
               ↓
   t_irq_wait #1 (consumes kernel-prefired SPI 77 wake)
               ↓
   print "AWAITING_QMP_KEY" sentinel
               ↓
   busy-poll on used.idx (cap MAX_POLL_ITER = 200M iterations)
        ┌─────────────┴─────────────────────────────────┐
        ↓                                               ↓
   used.idx advanced                          poll cap reached
        ↓                                               ↓
   ACK InterruptStatus                          log "SKIP"
        ↓                                               ↓
   drain_used + recycle                          exit 0
        ↓
   target seen?
   ┌────┴────┐
   yes       no (continue)
   ↓
   log "PASS"
   exit 0
```

If any init step fails, `STATUS |= FAILED` is written and the probe exits 1.

## Spec cross-reference

- **`scheduler.tla`** (I-9 NoMissedWakeup): the first `t_irq_wait` consumes the kernel-prefired SPI 77 wake. After the sentinel print, the driver transitions to busy-polling on `used.idx` — the IRQ wake path isn't load-bearing for event detection (DMA-visible used.idx advance is). The IRQ does still fire on every device-side used.idx bump; ACKing InterruptStatus keeps the line clean even though no one is waiting on it.
- **`handles.tla`** (I-2 caps monotonic, I-6 rights monotonic, HwHandleImpliesCap): the child holds CAP_HW_CREATE granted via `rfork_with_caps`; all four hw-handle SVCs reject without it (covered by existing tests).

## Tests

Suite: `kernel/test/test_virtio_input_probe.c::test_virtio_input_probe_rfork_with_caps`.
Registry: `userspace.virtio_input_probe_rfork_with_caps` in `kernel/test/test.c`.

Skips gracefully if:
- `/virtio-input` is absent from the ramfs (the userspace crate wasn't built).
- No virtio-mmio slot reports DeviceID = 18 (`tools/run-vm.sh` lacks `-device virtio-keyboard-device`, or `THYLACINE_NO_INPUT=1` is set).

CI verification (P4-K-events): `tools/test.sh` spawns a background QMP injector that polls the boot log for the `virtio-input: AWAITING_QMP_KEY` sentinel; on match, it connects to QEMU's QMP Unix socket (`build/qmp.sock`), negotiates `qmp_capabilities`, and issues `send-key` with qcode `"a"`. QEMU's virtio-keyboard-device translates that into 4 eventq writes (KEY press + SYN + KEY release + SYN); the driver drains, validates `{type=EV_KEY, code=KEY_A, value=1}`, and exits PASS. After boot completes, `tools/test.sh` greps the log for `virtio-input: saw target key` — absence (when injection was expected) fails the run. Disabled via `THYLACINE_INPUT_INJECT=0` or `THYLACINE_NO_QMP=1`.

On success, the boot log shows:

```
[test] userspace.virtio_input_probe_rfork_with_caps ... /virtio-input size=12536 bytes; input_dev pa=0x000000000a003a00 slot=29 intid=77 → pre-pend SPI + rfork_with_caps(CAP_HW_CREATE)
    exec_setup ok entry=0x0000000000400000 sp=0x0000000080000000 caps=0x1 → userland_enter
virtio-input: slot=29 intid=77 name="QEMU Virtio Keyboard" name_len=21 key_bits=29 rel_bits=0
virtio-input: AWAITING_QMP_KEY
virtio-input: event type=1 code=30 value=1
virtio-input: event type=0 code=0 value=0
virtio-input: saw target key (EV_KEY code=30 value=1)
virtio-input: PASS — event consumption end-to-end (slot=29 intid=77 events=2)
    /virtio-input reaped pid=1317 status=0 — config-space + eventq init + IRQ wake + bounded poll for QMP-injected key
```

On the interactive (`tools/run-vm.sh`) path without QMP injection, the boot log shows the SKIP variant instead:

```
virtio-input: AWAITING_QMP_KEY
virtio-input: SKIP — no EV_KEY/30/1 observed within 200000000 poll iterations (events_seen=0); QMP injection likely absent (...)
```

Either way the userspace child exits 0; the kernel test's `TEST_EXPECT_EQ(status, 0, ...)` passes. The PASS-vs-SKIP enforcement lives in `tools/test.sh` (post-boot grep).

## Error paths

| Path | Behavior |
|---|---|
| `/virtio-input` not in ramfs | Test skips with notice (PASS, no measurement) |
| No DeviceID=18 slot | Test skips with notice (PASS) |
| Userspace `t_mmio_create` fails | Userspace logs + `t_exits(1)`; kernel test FAILs |
| Userspace `t_dma_create` fails | Userspace logs + `t_exits(1)`; kernel test FAILs |
| Userspace `t_irq_create` fails | Userspace logs + `t_exits(1)`; kernel test FAILs |
| VirtIO `FEATURES_OK` rejected | Userspace logs + `t_exits(1)` |
| Device name empty (selector mechanism not responding) | Userspace logs + `t_exits(1)` |
| Device claims neither EV_KEY nor EV_REL | Userspace logs + `t_exits(1)` |
| QMP injector script fails (no python3 / socket unreachable / send-key declined) | `tools/test.sh` greps log; SKIP-marker present + PASS-marker absent → exits 1 with diagnostic |
| Used-ring entry has `id >= QUEUE_SIZE` | Userspace logs WARN + skips that entry (defensive — malformed device output shouldn't crash the driver) |

## Performance characteristics

Wall-time is dominated by:
- 32-page MMIO claim loop (~32 × MMIO_CREATE + MMIO_MAP SVCs).
- VirtIO init (handful of MMIO writes + readbacks).
- Three EV_BITS selector queries (~12 MMIO accesses each).
- One `t_irq_wait` (returns immediately from the kernel-prefired wake).
- The bounded busy-poll on `used.idx`: in the CI path (P4-K-events), QMP injection lands ~100-300 ms after the sentinel print, so the driver exits PASS within ~25-50M iterations (out of 200M cap). In the interactive (no-injection) path, the poll runs the full cap, ~3-5 sec on QEMU TCG.

Total in CI: ~1.6 sec boot (P4-K probe baseline) + ~300-500 ms for the QMP-injected event round-trip = ~2.1 sec.

## Status

- **P4-K (probe; landed)** at commit `7a72617` / `bcb6642` (hash fixup). New `usr/virtio-input/` crate (~310 LOC Rust) + `kernel/test/test_virtio_input_probe.c` (~110 LOC C) + `tools/run-vm.sh` wires `-device virtio-keyboard-device` + `usr/.cargo/config.toml` adds `-z max-page-size=4096`. Substrate gate proved: DeviceID=18 dispatch, selector-based config-space, eventq RX-only init. 240 → 241 tests; PASS × default + UBSan.
- **P4-K-events (this chunk; landed)**: full event consumption pipeline. New mechanisms: (1) kernel test pre-fires SPI 77 via `gic_set_pending_spi` before rfork (interactive-mode forward-progress guarantee — child consumes a guaranteed first wake then SKIPs after the poll cap rather than hanging indefinitely); (2) driver adds `t_irq_create` + a single `t_irq_wait` (consumes the pre-fire wake) + bounded busy-poll (`MAX_POLL_ITER = 200M iterations`); (3) used-ring drain helper that parses 8-byte event records, validates `(type=EV_KEY, code=KEY_A=30, value=1)` as the target, and recycles descriptors via the standard avail-ring re-publish pattern with DSB-fenced idx bump; (4) `tools/run-vm.sh` adds `-qmp unix:build/qmp.sock,server,nowait`; (5) `tools/test.sh` spawns a background injector that polls the log for the `AWAITING_QMP_KEY` sentinel and issues `send-key` for qcode `"a"` via QMP; (6) post-boot grep enforces that `virtio-input: saw target key` appears when injection was expected (otherwise the run fails). **Does NOT close ROADMAP §6.2** `Userspace virtio-input: keyboard input from VirtIO console reaches user processes via /dev/cons` — that exit criterion requires a `/dev/cons` 9P surface bridging the userspace driver's eventq output to other processes (Phase 5+ or P4-Id). What this chunk DOES close: the event-injection-to-userspace-consumption end-to-end pipeline (CI-visible PASS). Binary 10944 → 12536 B (within 16-KiB blob cap). 243/243 tests PASS × default + UBSan; test count unchanged.

## Known caveats

- **`/dev/cons` integration deferred**. The userspace driver consumes events from the virtio device directly; no `/dev/cons` Spoor exposes those events to other procs. ROADMAP §6.2's full criterion (keyboard input visible via `/dev/cons`) remains open pending P4-Id (driver-as-9P-server) or Phase 5+ FS-namespace work.
- **Busy-poll rather than blocking wait**. After the kernel-prefired `t_irq_wait` returns, the driver polls `used.idx` rather than calling `t_irq_wait` again. The reason is interactive-mode forward-progress: a second `t_irq_wait` would block indefinitely if QMP injection doesn't fire. The busy-poll burns ~3-5 sec of QEMU TCG wall-clock in the worst-case SKIP path. A future "alarm-style" syscall (time-bounded wait) could replace this; not load-bearing for v1.0.
- **Single target key check**. The driver matches exactly `(type=EV_KEY, code=KEY_A=30, value=1)`. QMP `send-key` for a different qcode would not register as the target; tests pinning a different key would need either a parameter or a multi-key match. Sufficient for the substrate test.
- **No multi-event invariance check**. Each `send-key` delivers 4 events (KEY press + SYN + KEY release + SYN), but the driver exits on first target match (typically after 2 events: press + first SYN). The release + final SYN land in the used ring after process exit; the kernel reaps the DMA handle, so they're harmless. A future test that exercises descriptor recycling under sustained load would need to drain all 4 + verify per-event types.
- **Hard-coded slot scan**. The probe scans all 32 virtio-mmio slots and matches DeviceID; the actual slot (29 on current QEMU config) is informational. A future DTB-driven device enumeration is Phase 5+.
- **`-z max-page-size=4096`** is now in `usr/.cargo/config.toml`. This affects ALL Rust userspace ELFs (shrinks each by ~60 KiB by closing rust-lld's default 64-KiB file-offset gap). The kernel ELF loader handles 4 KiB-aligned LOAD segments unchanged.

## Naming rationale

The crate, binary, and registry name all share the literal `virtio-input` token — the canonical VIRTIO spec name. No thematic rename proposed; the spec name is what auditors will scan for. The kernel test name (`test_virtio_input_probe_rfork_with_caps`) parallels `test_virtio_net_probe_rfork_with_caps` exactly — discipline over creativity. The function name retains the `_probe_` token even after P4-K-events extended it past probe-only scope (renaming the symbol would require a parallel rename in `kernel/test/test.c` registry + status doc references; deferred until a holistic test-naming refresh, if ever).
