# Handoff 035 — P4-Ja + P4-Jb (virtio-net: probe + ARP round-trip)

**Tip**: `a084edb` (P4-Jb hash fixup) on `main`. **The thylacine sees the network.**

This window lands the virtio-net userspace driver in two coherent sub-chunks, mirroring the virtio-blk-probe → virtio-blk-rw split:

- **P4-Ja** (`c1b7845` substantive / `542f2a6` hash fixup) — virtio-net-probe: TX-only init + send broadcast ARP + verify TX completion via IRQ. First NIC frame on the wire.
- **P4-Jb** (`f53ad75` substantive / `a084edb` hash fixup) — virtio-net-arp: full TX + RX round-trip. Pre-publish 16 RX buffers, send ARP, parse slirp's reply, validate bit-exact. **First frame received from outside the guest.**

Both chunks share the composed-hw-handle SVC surface established at P4-Ic5b2 / P4-Ic7 (MMIO + IRQ + DMA). The substrate didn't change; this is purely a userspace effort proving the discipline generalizes.

---

## What's on the wire

Both binaries send the same broadcast ARP request: "who-has 10.0.2.2 tell 10.0.2.15" with our MAC 52:54:00:12:34:56. QEMU's user-mode network (slirp) responds at the host side regardless of whether RX is configured in-guest.

The captured pcap (via `THYLACINE_NET_DUMP=path tools/test.sh`) shows both frames bit-exact:

```
42-byte request:  ff:ff:ff:ff:ff:ff (broadcast)
                  52:54:00:12:34:56 (our MAC)
                  ethertype 0x0806
                  ARP: hw=1, proto=0x0800, hwlen=6, protolen=4, op=1
                  sender HW = 52:54:00:12:34:56, sender IP = 10.0.2.15
                  target HW = 00:00:00:00:00:00, target IP = 10.0.2.2

64-byte reply:    52:54:00:12:34:56 (us)
                  52:55:0a:00:02:02 (slirp gateway's encoded MAC)
                  ethertype 0x0806
                  ARP: op=2
                  sender HW = 52:55:0a:00:02:02, sender IP = 10.0.2.2
                  target HW = 52:54:00:12:34:56, target IP = 10.0.2.15
```

Note slirp's MAC encoding: `52:55` prefix + the IP `10.0.2.02` as the last 4 bytes. The reply is 64 bytes (60 ARP + 4 padding pushed by some ethernet layer) — pcap reports the full captured payload.

P4-Ja's TX-only binary doesn't see this reply (RX QueueReady=0 → device drops). P4-Jb's TX+RX binary pre-publishes 16 RX buffers and consumes the reply into descriptor 0, parses ethertype + opcode + sender_ip, validates against expected values, prints the result.

---

## Key design decisions

### Two binaries instead of one

The natural split (mirroring virtio-blk):
- P4-Ja: probe-shaped TX-only — exercises one mechanism (TX virtqueue + per-queue QueueNotify + IRQ-driven completion).
- P4-Jb: probe-shaped TX+RX — adds the second mechanism (RX virtqueue + pre-published buffers + ring drainage).

Keeps each chunk focused and provides clean reference binaries for "what subset of mechanisms you need for X."

### RX descriptor recycling deferred

Both binaries are single-shot: they exit after observing one frame (probe) or completing one round-trip (arp). RX descriptors aren't re-published after consumption — buffer leaks at the device level until process exit, when the kernel reaps via burrow_free / handle_release_obj.

**Why this is fine for v1.0**: the probes prove the mechanism; they're not steady-state drivers. P4-Jc (steady-state for 9P-over-TCP) will implement RX descriptor recycling — re-publish each consumed buffer to the avail ring with a fresh idx bump.

### TX-used.idx unchecked in P4-Jb

P4-Jb's `wait_and_round_trip` initially had explicit TX-completion tracking via `tx_done`, but a self-audit refinement removed it. **Insight**: receiving a valid ARP reply with sender_ip = 10.0.2.2 is itself proof the device delivered our TX frame to slirp — slirp wouldn't synthesize a reply otherwise. So the RX-completion-with-validated-IPs is the unified success signal; no separate TX-used.idx check needed.

This is cleaner state-machine-wise (one variable to track instead of two) and matches how a real driver would treat the round-trip: response observation is the operational signal; queue-internal bookkeeping is for descriptor reuse.

### pretouch_rodata_pages discipline

- **virtio-net-probe**: LOAD = 0xd01 (~3.2 KiB) → fits in page 0 alone → **no pretouch needed**.
- **virtio-net-arp**: LOAD = 0x15a6 (~5.4 KiB) → spans pages 0 + 1 → **pretouch page 1 only**.

Pretouching pages NOT in the LOAD segment is itself a fault (P4-Ja's initial pretouch covered pages 1+2+3 and faulted; trimmed). The pretouch bound MUST match the binary's actual LOAD span; if the binary grows past 0x402000, extend in lockstep.

Rule of thumb after objdump: `pretouch covers page k iff (LOAD_top > k*PAGE_SIZE)`.

---

## Self-audit findings

Per CLAUDE.md categories on the substrate-level (post per-syscall audits R9/R10/R11/Ic5b1b — those covered the kernel syscalls; this chunk audits the userspace-driver discipline layer):

- **Lock ordering**: N/A (single-threaded userspace).
- **Lifetime**: handles (irq, mmio, ring DMA, rxpool DMA) leak at process exit; kernel `handle_release_obj` reaps. ✓
- **Error-path cleanups**: every failure path returns `Err(...)` or calls `t_exits(1)`. No multi-step state to roll back. ✓
- **Idempotency**: probe runs once per process; no retry semantics needed. ✓
- **State-machine guards**: VirtIO 1.2 init sequence strict (RESET → ACK → ... → DRIVER_OK); failures set STATUS_FAILED. ✓
- **Compile-time invariants**: layout offsets all < DMA_BUFSIZE; descriptor + frame counts bounded by QUEUE_SIZE. ✓
- **Boundary conditions**:
  - QUEUE_SIZE 16 ≤ QUEUE_NUM_MAX (QEMU default 256). ✓
  - RX_BUF_LEN 2048 > virtio_net_hdr 12 + max Eth frame 1514 = 1526. ✓
  - RX pool 32 KiB ≤ KOBJ_DMA_MAX_SIZE (1 MiB). ✓
  - rx_seen_idx vs rx_used_idx comparison via `!=` is correct under u16 wraparound (uses `wrapping_add`). ✓
- **IRQ ack pattern**: read InterruptStatus, write same bits back to InterruptAck — VIRTIO 1.2 §4.2.2 compliant. ✓

**Audit-bearing per trigger-surface table**: NOT audit-bearing at per-syscall layer (already audited). Audit-bearing at the userspace-driver-discipline layer; self-audit clean.

---

## Verify on session pickup

```bash
cd /Users/northkillpd/projects/thylacine
git log --oneline -10        # expect a084edb at top

# Default build + test:
tools/build.sh all
tools/test.sh                 # expect 230/230 PASS, ~448 ms boot
rm -rf build/kernel-undefined && tools/test.sh --sanitize=undefined
                              # expect 230/230 PASS, ~470 ms

# Inspect the wire:
THYLACINE_NET_DUMP=/tmp/net.pcap tools/test.sh
xxd /tmp/net.pcap | head -10
# Expect: 42-byte ARP request + 64-byte slirp reply visible

# Boot output (default 16 MiB disk; in order):
#   kobj_mmio: reserved kernel range × 4
#   virtio: 32 MMIO slots probed (3 with attached devices, 0 skipped)
#   mmio-probe: PASS
#   irq-probe: PASS
#   virtio-blk-probe: PASS — slot=31 intid=79 sig=THYLACINE-DISK-1
#   virtio-blk-rw: PASS — read(A)+write(B)+readback(B) over 7340032 bytes per pass × 3 passes
#   virtio-net-probe: slot=30 intid=78 mac=52:54:00:12:34:56 link=1
#   virtio-net-probe: PASS — broadcast ARP TX completed (slot=30 intid=78)
#   virtio-net-arp: slot=30 intid=78 mac=52:54:00:12:34:56 link=1
#   virtio-net-arp: rx_reply sender_mac=52:55:0a:00:02:02 sender_ip=10.0.2.2 target_ip=10.0.2.15
#   virtio-net-arp: PASS — full ARP request/reply round-trip (slot=30 intid=78)
#   tests: 230/230 PASS
#   Thylacine boot OK

# 30-boot KASLR stress:
pass=0; for i in $(seq 1 30); do
  tools/test.sh > /tmp/k.log 2>&1
  grep -q "Thylacine boot OK" /tmp/k.log && pass=$((pass+1))
done
echo "$pass / 30"             # expect ~27-30/30 (pre-existing 2-4% host flake)

# P4-Ic7 stress still works (default 2 GiB disk; ~57s build + ~3s test):
THYLACINE_DISK_SIZE=2G tools/build.sh kernel
THYLACINE_DISK_SIZE=2G BOOT_TIMEOUT=180 tools/test.sh
# Expect 230/230 PASS at ~3s with full 1 GiB R + 1 GiB W + 1 GiB readback

# Specs: unchanged from handoff 034
```

---

## State summary

- Tip: `a084edb` on `main`. Working tree clean. 68 commits ahead of `origin/main`.
- Tests: 230/230 PASS × default (~448 ms) + UBSan (~470 ms).
- KASLR: 27/30 distinct (pre-existing 2-4% host flake; not regressed).
- Image_size: 1448 KiB; 88 KiB headroom under image+firmware ≤ 2 MiB.
- Specs: unchanged (10 cfgs verified from handoff 033/034).

---

## What's next

### Immediate: P4-Jc (steady-state virtio-net driver) — RECOMMENDED

Foundation for 9P-over-TCP in Phase 5 (= ROADMAP §7 Stratum integration). Extends P4-Jb's single-shot RX with descriptor recycling: re-publish each consumed RX buffer to the avail ring after the frame is processed, enabling continuous packet reception.

Scope choices for P4-Jc:
- **Conservative**: just RX recycling + multi-frame consumption + frame-count benchmark (~300 LOC additional). Proves steady-state RX without committing to a TCP architecture.
- **Ambitious**: add basic ICMP echo (ping responder); proves L3 layer + checksum. ~500 LOC.
- **Aggressive**: start the TCP state machine (handshake + a single connection). Bridges into P4-Id design. ~1500 LOC; spans multiple chunks.

### Alternative: P4-Id (driver-as-9P-server)

Each driver exposes a 9P endpoint that other processes can attach to. Net-driver's endpoint serves `/net/dial`, `/net/listen`, `/net/clone` (Plan 9 network model). Pairs naturally with P4-Jc — once RX recycling exists, the driver can serve multiple consumers.

### Phase 4 closure exit criteria still pending

Per ROADMAP §6.2:
- **Driver crash recovery**: kill driver mid-IO; supervisor restarts; subsequent I/O resumes. (P4-M)
- **IRQ-to-userspace handler latency p99 < 5 µs**: dedicated benchmark. (P4-Ic-latency)

### Open invariants

Cumulative deferred audit items (none blocking):
- R12-pol (P4-Ic5b1a virtio-mmio policy relax) — Phase-5+-revisit-flagged.
- R12-FP (P4-Ic5-FP eager FP save/restore) — formal pass deferred.
- R12-DMA (P4-Ic5b1b DMA syscalls) — formal pass deferred.
- R12-gic-edge (P4-Ic5b2 ICFGR edge-trigger transition) — formal pass deferred.
- R12-sched / R12-sched-impl / R12-wfi-cfg — **CLOSED** at P4-Ic6 trilogy.
- R12-bss-2mib (P4-Ic7) — kernel.ld assert is conservative-low.
- R12-uaccess (P4-Ic7) — SYS_PUTS uaccess-emulation needed.

### New: P4-Ja / P4-Jb follow-ups

- **RX descriptor recycling** (P4-Jc scope).
- **Steady-state IRQ ack pattern**: current pattern reads + writes InterruptStatus once per IRQ. For high-volume RX, this needs careful interleave with the descriptor loop to avoid lost interrupts. Spec-level invariant: the IRQ ack is single-write at end-of-loop, not per-frame. Document in P4-Jc when implementing.
- **Slirp-specific assumptions**: hardcoded SLIRP_GUEST_IP = 10.0.2.15 + SLIRP_GATEWAY_IP = 10.0.2.2 are QEMU-virt-with-default-slirp specifics. A Phase 5+ DHCP probe would replace these with discovery.

---

## Common pitfalls for future sessions

- **pretouch must match LOAD bounds.** Trimming `pretouch_rodata_pages()` to only cover pages within LOAD is a non-negotiable rule. If a binary's LOAD grows past page N, extend the pretouch in the same commit. If a binary shrinks, trim.
- **DMA buffer alignment.** Each `t_dma_create` returns a page-aligned PA. Multi-page allocations (P4-Jb's 32 KiB rxpool) are contiguous in PA-space by construction (alloc_pages with KP_ZERO + appropriate order at the kernel side). Don't assume contiguity across separate `t_dma_create` calls — they're independent.
- **Descriptor table layout.** Each VIRTIO descriptor is exactly 16 bytes: `le64 addr; le32 len; le16 flags; le16 next`. Off-by-one on the field offsets silently corrupts the chain. The probe binaries' write64/write32/write16 sequence is byte-accurate.
- **Avail ring vs Used ring layout.** Both have `le16 flags; le16 idx;` headers, then arrays. But the array element sizes DIFFER: avail.ring[k] is `le16` (descriptor index); used.ring[k] is `{le32 id; le32 len}` (8 bytes per entry). Always 8-byte stride for used; 2-byte stride for avail.
- **u16 wraparound on rx_seen_idx.** The VIRTIO spec says all idx counters are u16 with implicit wraparound; comparison should always use `!=`, never `<`, and increments should use `wrapping_add(1)`. P4-Jb's drain loop already does this.

---

## Predecessor handoff

Handoff 034 (P4-Ic7): `docs/handoffs/034-p4ic7.md`.
