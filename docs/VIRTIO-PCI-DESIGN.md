# VIRTIO-PCI-DESIGN.md — the userspace virtio-PCI transport

**STATUS**: pci-0 charter (scripture; no code). Signed off 2026-06-15 (the DFS
fork off #140: preempt net-2, build the future-proof transport).

This document is the design pass for moving virtio devices from the QEMU
virtio-**mmio** bank to the PCIe root complex (virtio-**pci**), so that two
persistent userspace drivers can co-reside with real per-device isolation. It
is the scripture the pci-1/pci-2/pci-3 impl sub-chunks cite. Cross-refs:
`ARCHITECTURE.md §13` (the device layer), `docs/NET-DESIGN.md §17` (net-2 rides
this), `docs/reference/37-virtio_pci.md` (the as-built P4-H enumerator).

---

## 1. Why — the #140 problem, and why PCI rather than a workaround

net-1 landed a reusable virtio-net frame driver, but net-2 (`netd`, the
persistent `/net` server that owns virtio-net) is blocked by **#140**:

- QEMU's virtio-**mmio** bank packs **8 device slots per 4 KiB page** (`0x200`
  stride). On the `virt` machine, virtio-**net** lands on slot 30 and the
  Stratum-pool virtio-**blk** on slot 31 — **the same 4 KiB page `0x0a003000`**.
- The `KObj_MMIO` claim is **page-aligned + page-exclusive** (`mmio_handle.c`
  `kobj_mmio_create` rejects sub-page; `ranges_overlap` rejects a second claim
  on the page). So `netd` (net) and `stratumd` (blk) — both **long-lived** —
  cannot both hold the page. net-1 only dodged this because its probe runs and
  **exits** before stratumd starts; a persistent `netd` cannot.
- Underneath the bookkeeping is a hardware fact the MMU cannot fix: the granule
  is a hard **4 KiB** (`page.h:16`), but a device's register window is **512
  bytes**. To give a userspace driver its 512-byte window the kernel must map
  the whole 4 KiB page — which contains up to **7 other devices' registers**.
  **Sub-page device MMIO is inherently non-isolating**: whoever maps the page
  can touch every device on it. (This already exists in-tree: the kernel CSPRNG
  drives the rng slot on `0x0a003000`, and net-1's probe maps that page — a
  documented v1.0 residual, `mmio_handle.c:434`.)

Two ways out were weighed (see the §140 brief in the session log):

- **A kernel-mediated sub-page MMIO** (`SYS_MMIO_READ/WRITE`, kernel does the
  access against its own mapping). Sound, but it builds a **second MMIO
  mechanism that exists only to prop up the mmio bank** — a transport we want
  to leave. When PCI arrives anyway (for real hardware), it goes vestigial.
- **virtio-PCI** (this document). On PCIe each function gets its **own
  page-aligned BAR**, so the **existing** page-exclusive `KObj_MMIO` claim Just
  Works with real per-device isolation, **one device per page, no neighbors**.
  It is what Fuchsia and real ARM servers use (virtio over PCIe), so it is also
  the correct transport for the Lazarus portability arc — the mmio bank is a
  QEMU-`virt`-ism.

Per the depth-first-dependencies principle (CLAUDE.md "pull the foundational
dependency forward; build it properly"), the user voted (2026-06-15) to
**preempt net-2 and build the virtio-PCI transport** — the future-proof spine —
rather than the kernel-mediated-mmio workaround. PCI subsumes the need for the
workaround entirely.

---

## 2. Decision + scope

| # | Decision | Rationale |
|---|---|---|
| **D-pci-1** | **Build the userspace virtio-PCI transport** (a per-device-capability kobj + kernel BAR assignment + cap-resolve + INTx routing; userspace drives the virtio-pci-modern register layout). | Future-proof transport; dissolves #140 by construction (per-device page-aligned BAR); real-hardware-correct; completes the deliberately-stubbed P4-H. |
| **D-pci-2** | **net-only scope.** Move *only* virtio-net to PCI; leave blk/rng/input on the mmio bank. | Dissolves #140 immediately (once net leaves the bank, blk is the lone *userspace* claimant on its page; rng is kernel-owned). Proves the transport on one device with **zero Stratum change**. blk→PCI (+ the Stratum `bdev_thylacine` port) is the recorded v1.x seam (§9). |
| **D-pci-3** | **INTx interrupts, not MSI-X.** | Forced, not preferred: MSI needs GIC **ITS** (v3) or **v2m** (v2), neither of which the kernel drives. INTx (the DTB `interrupt-map` → existing GIC INTIDs) needs no new interrupt controller. With net the only userspace PCI device, it lands on **one unshared INTID** → the existing single-waiter irqfwd works untouched. MSI-X is a clean v1.x upgrade (§9). |

No new `ARCH §28` invariant. The design **composes** existing scripture: I-5
(hardware-handle non-transferability) extends to the new `KObj_PCI`; the
exclusive per-function claim is the PCI analog of the existing
`HwResourceExclusive` (the mmio overlap rejection).

---

## 3. Ground truth (the DTB, read directly)

Resolved by dumping the actual DTB QEMU 10.0.2 generates for the `virt`
machine and decoding the `pcie@10000000` node (identical for `gic-version=2`
and `=3`):

- **ECAM** (config space): `reg = <0x40 0x10000000 0x00 0x10000000>` → PA
  `0x40_1000_0000`, size **256 MiB**. Already reserved (`pci-host-ecam-generic`,
  `mmio_handle.c:387`) and mapped bus-0 by the P4-H enumerator.
- **BAR window** (`ranges`): a 32-bit MMIO window at **PA `0x1000_0000`,
  ~768 MiB** (`0x2eff0000`), plus a 64-bit prefetchable window at `0x80_0000_0000`
  (512 GiB). The 32-bit window is **not** kernel-reserved → a BAR allocated
  there is userspace-claimable. virtio-pci modern BARs are page-aligned and
  ≥16 KiB → **a per-device BAR is page-aligned and isolating**.
- **INTx routing** (`interrupt-map` + `interrupt-map-mask = <0x1800 0 0 0x07>`):
  the standard swizzle. The 4 INTx pins map to **GIC SPIs 3–6 = INTID 35–38** by
  `SPI = 3 + ((slot + pin − 1) mod 4)`. A single device on pin INTA gets one
  specific INTID with **no sharing**.
- **MSI is present in hardware** (`arm,gic-v3-its` / `arm,gic-v2m-frame` + the
  pcie `msi-map`) **but Thylacine drives neither ITS nor v2m** → MSI-X stays a
  seam; INTx is the v1.0 path.
- **BARs start unassigned** (we boot bare — no UEFI/EDK2 doing PCI resource
  assignment), so **the kernel must assign BARs** from the host-bridge MMIO
  window. Standard, bounded (§4.3).

---

## 4. Mechanism

The split: **the kernel owns config space** (ECAM is bus-wide and
un-isolatable — you cannot page-isolate one function's 4 KiB config from the
bus) and mediates per-function setup; **userspace owns the per-device,
page-aligned BAR** (reached only through the owned capability) and drives the
virtio protocol over it. The heavy resources (IRQ, DMA) reuse the existing,
audited kobjs.

### 4.1 The kernel half (config space, never handed to userspace)

The P4-H enumerator (`kernel/virtio_pci.c`) already maps bus-0 config space
into kernel VA and discovers virtio-pci devices (BDF, vendor/device id). It is
reused as-is for discovery; pci-1 adds the per-function operations on top.

### 4.2 The new capability: `KObj_PCI`

A new, thin handle representing **exclusive ownership of one PCI function**.
Non-transferable (joins I-5's `KObj_MMIO`/`IRQ`/`DMA`/`Loom` set; `handle_dup`
and any transfer reject it). Backed by a small per-function claim table (the
PCI analog of `g_mmio_claims`); exactly one owner per `(bus, dev, fn)`.

The new syscall surface (shape is design intent; exact numbers/structs pinned
at pci-1, ABI-confirmed there):

- **`SYS_PCI_CLAIM(selector) → KObj_PCI`** — selector is a `virtio_device_id`
  (e.g. `VIRTIO_DEVICE_ID_NET`) or an explicit BDF. Gated by `CAP_HW_CREATE`
  (same gate as `SYS_MMIO_CREATE`). The kernel: verifies the function is a
  virtio-pci device + unclaimed → records exclusive ownership → **assigns +
  enables BARs** (§4.3) → **walks the virtio PCI capabilities** (`VIRTIO_PCI_CAP_*`,
  VIRTIO 1.2 §4.1.4) to resolve the common-cfg / notify / ISR / device-cfg
  regions → returns the handle. Full rollback on any failure (release the
  claim, free nothing partial).
- **`SYS_PCI_MAP_BAR(KObj_PCI, bar_index, vaddr, prot) → 0/-err`** — maps that
  BAR's **page-aligned** region into the caller, at a **kernel-controlled PA**
  the driver never names. W^X (RW only; I-12). This is the per-device-isolated
  MMIO: a Proc can map a BAR **only** through a `KObj_PCI` it owns, so it cannot
  reach a function it did not claim.
- **`SYS_PCI_INFO(KObj_PCI, *out) → 0/-err`** — returns the resolved region map
  (per region: bar index + offset + length; + the notify `cap.length` /
  `notify_off_multiplier`) and **the device's INTID** (the swizzled SPI from the
  `interrupt-map`). The driver feeds the INTID to the existing
  `SYS_IRQ_CREATE`/`SYS_IRQ_WAIT`.

`KObj_PCI` close (handle release / Proc exit): releases the function claim and
quiesces the device (write `0` to the virtio `device_status`, disable BAR
decode + bus-master in the command register) **before** the BAR mapping tears
down — symmetric with the virtio device-death quiesce discipline (RW-7 R3-F1).

### 4.3 BAR assignment (the kernel is the PCI resource allocator)

Booting bare, BARs are unprogrammed. At `SYS_PCI_CLAIM`, for each
memory BAR the kernel: sizes it (write all-ones, read back the size mask,
restore), **allocates a naturally-aligned region from the host-bridge 32-bit
MMIO window** (`0x1000_0000`+, a simple bump allocator over the `ranges`
window — linear, no remapping, matching the ARCH §13.2 "linear BAR allocation"
intent), writes the PA into the BAR, and enables `MEM_SPACE` + `BUS_MASTER` in
the command register. The allocated region is page-aligned (BAR size ≥ a page)
→ `SYS_PCI_MAP_BAR` maps whole pages → isolating.

### 4.4 The userspace half (the virtio-pci-modern driver)

`libthyla-rs::hardware` gains a `PciDev` over the three syscalls. The driver
maps its BAR(s) via `SYS_PCI_MAP_BAR`, reads the region map via `SYS_PCI_INFO`,
and drives **virtio-pci modern** (VIRTIO 1.2 §4.1): the common-cfg structure
(feature negotiation, queue setup — `queue_desc`/`queue_driver`/`queue_device`
physical addresses from a `KObj_DMA`), the notify region (queue-notify writes),
the ISR-status region (the INTx ack/demux read), and the device-cfg region
(e.g. the virtio-net MAC). IRQ via `SYS_IRQ_CREATE(INTID)`/`WAIT`; DMA via the
existing `KObj_DMA`.

### 4.5 The isolation property (#140 dissolved)

- net's BAR is its **own page-aligned region** in the BAR window — a different
  PA space from the mmio bank entirely. Once net is on PCI, **no two userspace
  drivers share a page**: blk is the lone userspace claimant on the mmio page;
  rng is kernel-owned. #140's contention is gone by construction.
- The BAR is reachable **only through an owned `KObj_PCI`** (kernel-controlled
  PA; `SYS_PCI_MAP_BAR` from the handle), and the function claim is **exclusive**
  — so a second Proc can neither claim net's function nor map its BAR. This is
  the genuine "you must own this device" capability the `mmio_handle.c:421`
  comment wished for, at function granularity. (Honest scope: `CAP_HW_CREATE`
  remains the hardware root of trust — the win is real per-device isolation for
  the cooperative TCB drivers + the structural exclusivity, not a new defense
  against a malicious `CAP_HW_CREATE` holder, which was never the threat model.)

---

## 5. Reuse — net-1 was not wasted

The virtqueue logic is transport-independent. **`netdev::ring`** (the audited
index/wrap math) and the frame/header logic carry over verbatim. What is new is
**PCI discovery + the register layer**: `virtio.rs`'s transport half (the mmio
register block + the mmio notify) gains a `pci` sibling that drives the
common-cfg/notify/isr/device-cfg regions instead. The `ring` + frame code is
shared between both transports.

---

## 6. Invariants

**No new `ARCH §28` invariant.** The design composes:

- **I-5** (hardware-handle non-transferability) — **extended to `KObj_PCI`**
  (static_assert + `handle_dup`/transfer rejection). The mapped BAR and the IRQ
  remain non-transferable via their existing kobjs.
- **HwResourceExclusive** (the existing mmio claim property) — the PCI
  per-function claim is its analog: exactly one owner per `(bus, dev, fn)`.
- **I-12** (W^X) — `SYS_PCI_MAP_BAR` maps RW only (device MMIO; never X).
- **I-2 / I-6** — `KObj_PCI` carries no transferable rights; the claim is gated
  by `CAP_HW_CREATE` exactly like `SYS_MMIO_CREATE` (monotonic, no expansion).

---

## 7. Audit surfaces (reserved; pci-3)

The pci-1 kernel surface is audit-bearing and joins the `ARCH §25.4` +
CLAUDE.md trigger table. Prosecute at pci-3:

- `KObj_PCI` lifecycle — exclusive-claim correctness; no UAF/double-free/leak on
  every error path (claim → assign → enable → cap-walk rollback); close quiesce
  ordering (device stopped before BAR unmap).
- **Config-space mediation** — userspace never gets raw ECAM; `SYS_PCI_INFO`
  returns only resolved, bounds-checked region descriptors; a hostile BAR/cap
  layout (cap-pointer loop, out-of-range bar index, oversized length) is
  rejected, never trusted into an OOB.
- **BAR assignment** — the allocator stays within the host-bridge window; no
  overlap between two functions' BARs; size/alignment math cannot overflow;
  page-alignment guaranteed before `SYS_PCI_MAP_BAR`.
- **`SYS_PCI_MAP_BAR`** — maps only a page-aligned BAR owned by the caller's
  handle; W^X; VA bounds; rollback on copy/map fault.
- **INTx routing** — the swizzle is computed from the DTB `interrupt-map`, not
  hardcoded; the returned INTID is the device's actual line; I-5 (the GIC stays
  kernel-reserved — a PCI driver gets a forwarded INTID via `KObj_IRQ`, never
  the distributor).
- I-5 non-transferability of `KObj_PCI` (static_assert + the dup/transfer paths).

---

## 8. Sub-chunk phasing

| Sub-chunk | Scope |
|---|---|
| **pci-0** | This charter (scripture; no code). |
| **pci-1** | Kernel: `KObj_PCI` + exclusive claim table + `SYS_PCI_CLAIM`/`MAP_BAR`/`INFO` + BAR assignment + virtio cap-resolve + INTx swizzle. Reuses the P4-H enumerator. Unit tests + the I-5/exclusive-claim asserts. |
| **pci-2** | Userspace: `libthyla-rs::hardware::PciDev` + the virtio-pci-modern register layer; port `netdev`'s transport half (reuse `ring`); `run-vm.sh` `virtio-net-device` → `virtio-net-pci`; `netdev-test` PASS over PCI. |
| **pci-3** | Focused audit (§7) + SMP gate + reference docs (`37-virtio_pci.md` update + a `115-pci-claim.md`). Then **net-2 resumes** on the PCI net transport. |

---

## 9. Recorded v1.x seams (deferred, by design)

- **blk → PCI migration** — move virtio-blk to `virtio-blk-pci` and port
  Stratum's `bdev_thylacine` driver. Retires the mmio bank for the second
  userspace driver. The design point: if blk's swizzled INTID collides with
  net's, irqfwd needs **multi-waiter fan-out** (the RW-7 #30 single-waiter
  territory) OR deliberate slot placement to give distinct lines — resolved
  then, not now (net-only has one unshared line).
- **MSI-X** — needs a kernel GIC **ITS** (v3) / **v2m** (v2) driver. The
  hardware is present (§3); the upgrade is per-queue interrupts (no INTx
  sharing). Clean win once a driver wants it.
- **Multi-bus / PCIe bridges** — P4-H maps bus 0 only; QEMU `virt` has no
  bridges. Multi-bus is a v1.x extension (Type 1 headers + secondary-bus walk).
- **rng / input** — rng is kernel-owned (stays mmio; no userspace contention).
  virtio-input (Halcyon, Phase 10) can move to PCI under the same `KObj_PCI`
  mechanism if a contention or real-HW need appears.

---

## 10. Status

- **pci-0 (this charter): LANDED** — the DFS fork off #140 (user-voted
  2026-06-15) is bound; the mechanism is grounded in the DTB ground truth; the
  net-only + INTx scope is fixed; the seams are recorded.
- **pci-1..pci-3: not started.** Preempts net-2; net-2 resumes on the PCI net
  transport once pci-3 closes.

The thylacine hunts on whatever bus the hardware actually exposes.
