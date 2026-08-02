---
id: sub-stratum-bdev
type: sub
parent: moc-stratum
title: "The block backend — Stratum drives virtio-blk in-process"
code:
  - "stratum: src/block/bdev_thylacine.c"
audit: hard
guarded-by: []
validated-by: [prose, gate-smp]
locks: []
abis: []
design: ["docs/reference/86-pouch-stratumd-boot.md", "docs/POUCH-DESIGN.md section 14"]
created: 2026-08-02
updated: 2026-08-02
---
## Purpose

Give Stratum a disk on Thylacine. `stratumd` holds `CAP_HW_CREATE` granted
by joey at spawn and drives the QEMU virt virtio-mmio block device
**directly, in-process** — the "stratumd-as-driver" choice. There is no
daemon protocol seam between the FS and its block device; mount and I/O
proceed entirely through the `stm_bdev` vtable.

The body is a port of Thylacine's one-shot Rust test driver
(`usr/virtio-blk-rw`) into a long-lived backend: same VIRTIO 1.2 init
recipe, same three-descriptor chain, same IRQ-driven completion.

## Contract

`stm_bdev_open_thylacine(path, opts, out)` → a `stm_bdev` whose vtable
serves blocking read/write/fsync/fdatasync plus synchronous-completion
`submit_*` stubs. `discard` and `resize` are `STM_ENOTSUPPORTED`.

The `path` argument is **purely informational** — the slot is found by
scanning MMIO, so joey passes `/dev/virtio-blk` as a label, not a lookup.

Six invariants are named in the file header (B-1 … B-6). The load-bearing
ones: offsets are sector multiples or `STM_EINVAL` (B-1); at most one
virtqueue request in flight per bdev, enforced by the per-bdev mutex (B-2);
every kobj handle acquired in open is released in close, with partial-init
failures unwinding cleanly (B-3); `DSB SY` before the doorbell guarantees
descriptor and ring writes are visible to the device (B-5).

## Mechanism

**Fixed user VAs, and why they are safe.** MMIO bank at `0x00500000` (4
pages), ring DMA at `0x00600000` (1 page), data DMA at `0x00700000`
(1 MiB). These are hardcoded rather than allocated because the kernel's
anonymous mapping allocator draws from `BURROW_VA_BASE` and never picks
below `0x01000000` — so the low region is reservation-safe by construction.
The mallocng heap, which requests anonymous mappings, cannot collide.

**The slot scan runs HIGH-to-LOW, and the direction is load-bearing.** QEMU
assigns virtio-mmio slots in *reverse* creation order, so the first
`-device` on the command line lands at slot 31. `tools/run-vm.sh` lists the
pool first, so `pool.img` is at 31 and `disk.img` at 30. This driver scans
downward and picks the pool; the legacy probe binaries scan upward and pick
the disk. Two devices, two scan directions, no coordination needed — see
[[sub-substrate-machine]] for the board side of the same arrangement.

**Three descriptors, and a fourth shape for FLUSH.** desc[0] is the request
header (OUT), desc[1] the data buffer, desc[2] the status byte (device-
writable). A FLUSH carries no data, so it repoints `desc[0].next` from 1 to
2, skipping desc[1] entirely. The head id and the terminal status
descriptor are unchanged, so every completion check holds identically.

**The partial-sector write is read-modify-write, and the naive version
corrupted a neighbour.** Stratum's extent layer writes block-aligned
plaintext plus an AEAD tag — 4096 + 32 = 4128 bytes — so a write length is
*never* a sector multiple. A block device cannot write sub-sector. The
original code rounded up and zero-padded the tail; Stratum packs an
adjacent object into that same sector, so the zero-pad **destroyed a
neighbour's bytes**, surfacing later as an AEAD failure (`STM_EBADTAG`) on
a read that had nothing to do with the write. The fix reads the on-disk
sector, overlays the real bytes, and writes it back, with the mutex held
across both halves so nothing can touch the sector or the shared DMA
buffer between them. A whole-sector zero-pad is correct **only** when the
length is a sector multiple. The posix backend never had this bug because
`pwrite` is byte-granular.

**The failure latch was unreachable-by-design and nobody noticed.** A
failed request latched `d->failed` permanently, on the stated assumption
that "Stratum tears down and re-opens" — but **no caller ever drove that**.
So one transient virtio hiccup killed the filesystem for the life of the
process. The fix does the recovery in place: up to
`DO_REQUEST_MAX_REINIT` (2) cycles of full VIRTIO reset — which drops any
in-flight request and resets both ends' idx to 0 — plus re-negotiate,
rebuild the descriptor chain, and re-submit. Only an exhausted budget, or
a re-init that itself fails, latches. A re-submit is idempotent by
construction: a READ re-reads the same LBA; a WRITE re-writes it from the
unchanged data DMA buffer, which the re-init does not touch.

**`VIRTIO_BLK_F_FLUSH` makes durability self-contained.** Negotiating it
lets `op_fsync` issue a real `VIRTIO_BLK_T_FLUSH` barrier, so Stratum's
write-then-fsync commit ordering is real on-device *regardless of the
launch cache mode* — previously it silently depended on the harness
passing `cache=writethrough`. When the device offers no writeback cache the
feature is absent, every completed write is already durable, and `op_fsync`
is correctly a no-op. The flag is re-derived on every re-init so a
recovered device keeps it accurate.

**Diagnostics go through raw `write(2)`, deliberately.** I/O runs on server
worker threads, so `BDEVDIAG` uses a bounded stack buffer and one write
syscall — thread-safe and allocation-free where stdio is neither. A bdev
that latches dead silently is undiagnosable in the field, so each recovery
attempt and the permanent latch each emit one line naming the failing arm.

## Data structures

`thyla_bdev` embeds `stm_bdev` first (the downcast contract). Carries the
mutex, three kobj handles (`-1` sentinel = unallocated), the two
kernel-stamped DMA bus addresses, the selected slot/VA/INTID, the
monotonic `avail_idx`, the `failed` latch, `reinit_count`, and
`flush_supported`.

Five `_Static_assert`s pin the ring layout — desc before avail before used
before the inline request header before the status byte, all inside one
page. Drift there would silently corrupt the device's view of the rings.

## Concurrency

One mutex serialises every request (B-2). The FS's worker pool means
several threads reach the vtable, so the lock is real, not ceremonial. It
is held across the whole RMW pair and across the IRQ wait — the request is
synchronous end to end.

## Invariants enforced

None of §28 directly. It *consumes* the hardware-capability model: the
MMIO/IRQ/DMA handles are minted under `CAP_HW_CREATE` and are
non-transferable by kind, so the claimer is the driver and a driver cannot
leak its device (the I-5 property; no registry note yet — the handle
surface is unswept). The 1 MiB DMA region and the 4-page MMIO bank are the
only kernel memory it commands.

## Error paths

Every phase of `open` unwinds through one `fail:` label. `find_blk_slot`
failure returns `STM_ENODEV` distinctly — no device is not the same as a
broken one. In steady state, `do_request` returns `STM_EIO` after the
recovery budget; `op_fsync` fails closed on a latched device rather than
reporting a flush it cannot honour, keeping the latch consistent across
the whole vtable rather than just read/write.

## Performance

One outstanding request; 1 MiB per virtqueue transaction (2048 sectors);
`queue_depth = 1`. Pipelining is a stated v1.x enhancement — the mount
path is single-threaded, so the bottleneck is elsewhere.

## Prosecution

- Any new op must take the mutex; B-2 is what makes the single shared
  descriptor chain safe.
- A new write path must preserve RMW for partial tails. Round-and-pad is
  correct *only* for exact sector multiples, and the failure mode is a
  neighbour's corruption surfacing as an unrelated read's AEAD error.
- The recovery budget must stay bounded and the latch must stay one-way; a
  device that cannot re-negotiate is dead, and re-publishing onto rings
  whose state was abandoned is the original R4-F1 hazard.
- The fixed VAs depend on the kernel never placing an anonymous mapping
  below `0x01000000`. A change to `BURROW_VA_BASE` breaks this silently.
- Feature negotiation must never request a bit the device did not offer,
  or `FEATURES_OK` can be refused on our account.

## Seams

[[seam-kobj-handle-release]] — there is no syscall to release an
individual `KOBJ_MMIO`/`IRQ`/`DMA` handle, so `op_close` frees the struct
and leaves kernel-side cleanup to process exit. Correct for a single-bdev
daemon; a multi-bdev context would leak.

## Caveats

- No discard, no resize, no multi-queue. Stated v1.x followups.
- The async `submit_*` ops fire their callback *inside* submit. This is
  sound because the published contract is "completions **may** fire on a
  thread other than the submitter" — the submitter-thread case satisfies
  it — but a caller that assumes asynchrony will find none.
- `op_wait` returns 0 rather than blocking. A caller reaching it has a
  logic bug; blocking would deadlock the mount path, so it returns
  honestly-nothing instead of hanging.

## Provenance

[[chg-2026-08-02-stratum-sweep]].
