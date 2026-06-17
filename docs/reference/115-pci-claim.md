# Reference: KObj_PCI — the virtio-PCI function claim (pci-1b/1c/2)

## Purpose

`KObj_PCI` is the kernel object that hands a single VirtIO-PCI function to a
userspace driver as a non-transferable hardware capability. It sits one layer
above the P4-H ECAM enumerator (`docs/reference/37-virtio_pci.md`): the
enumerator discovers + maps config space bus-wide (kernel-only, un-isolatable);
`KObj_PCI` mediates that config space and hands out per-BAR MMIO mappings so a
driver process can drive virtio-pci-modern over its own page-aligned BARs.

This is the #140 resolution (`docs/VIRTIO-PCI-DESIGN.md`): the virtio-mmio bank
packs 8 device slots into one 4 KiB page, so two persistent userspace drivers
(netd + the Stratum-pool stratumd) could not get sound isolated co-residency
under the hard 4 KiB MMU granule. On PCIe each function carries its own
page-aligned BAR, so the existing page-exclusive `KObj_MMIO` claim isolates two
drivers at the MMU granule — one device per page.

Scope at v1.0: **net-only**, **INTx** (MSI-X needs a GIC ITS/v2m driver — a
recorded v1.x seam). blk-over-PCI is a v1.x seam (needs the Stratum bdev port).

## Public API

### `<thylacine/pci_handle.h>` (kernel)

```c
// Claim the first VirtIO-PCI function matching virtio_device_id: assign + enable
// its memory BARs (from the dtb_pci_mem_window bump arena), walk the
// VIRTIO_PCI_CAP_* list into regions[], resolve the INTx GIC INTID, enable
// MEM-decode + bus-master. Returns a ref=1 KObj_PCI, or NULL on any failure
// (every partial side effect rolled back; the device left quiesced + re-claimable).
struct KObj_PCI *kobj_pci_claim(u32 virtio_device_id);

void kobj_pci_ref(struct KObj_PCI *k);
void kobj_pci_unref(struct KObj_PCI *k);   // last unref quiesces + frees; NULL-safe

// Resolve a BAR index to its KObj_MMIO (the SYS_PCI_MAP_BAR handler wraps it in a
// Burrow, which takes its own ref). Does NOT bump — caller holds k alive.
struct KObj_MMIO *kobj_pci_bar_mmio(struct KObj_PCI *k, u32 bar_index);

// Exposed for the deterministic width-correct-inversion unit test.
u64 pci_bar_decode_size(u32 lo_mask, u32 hi_rb, bool is64);
```

### Syscalls (pci-1c)

```
SYS_PCI_CLAIM   = 76   (virtio_device_id)                  -> handle / -1
SYS_PCI_MAP_BAR = 77   (handle, vaddr, bar_index, prot)    -> 0 / -1
SYS_PCI_INFO    = 78   (handle, info_va)                   -> 0 / -1
```

All three require `CAP_HW_CREATE`. `SYS_PCI_CLAIM` mints a handle with the
kernel-FIXED rights `R | W | MAP` (no `TRANSFER`) — `KObj_PCI` is
non-transferable (I-5), so there is no partial-rights / transfer use case.

### `libthyla_rs::hardware::PciDev` (userspace)

```rust
// Claim a function by virtio_device_id, read its topology, map every present BAR
// at bar_window + i*PCI_BAR_VA_STRIDE (1 MiB stride).
unsafe fn claim(virtio_device_id: u32, bar_window: u64) -> Result<PciDev, PciError>;
fn region(&self, kind: PciRegion) -> Option<(u64, u32)>;  // (mapped VA, byte len)
fn intid(&self) -> Option<u32>;
fn notify_off_multiplier(&self) -> u32;
fn virtio_device_id(&self) -> u16;
fn bdf(&self) -> (u8, u8, u8);
```

`Drop` closes the handle; the BAR mappings survive to proc exit (the #847 dual
lifetime). Non-transferable per I-5 — the type adds no `Transfer` impl.

## Implementation

### `kernel/pci_handle.c`

- **Claim table** (`g_pci_claims[KOBJ_PCI_MAX=8]`): one alive `(bus,dev,fn,owner)`
  tuple per slot; `kobj_pci_claim` installs the exclusivity slot FIRST (rejecting
  a double-claim / full table before any device mutation), so every later failure
  routes through `kobj_pci_unref` -> `pci_release_bars_and_claim`.
- **BAR bump arena** (`pci_bar_alloc`): assigns BARs linearly from
  `dtb_pci_mem_window` (bare boot, no UEFI). Bounds + overflow guarded
  (`a < base || a+size < a || a+size > end`). Does not reclaim a freed BAR's PA
  (re-claim bump-allocates fresh; ~768 MiB window vs KiB BARs = ample headroom).
- **BAR sizing** (`pci_assign_one_bar` + `pci_bar_decode_size`): the PCI Local Bus
  6.2.5.1 all-ones-probe with MEM-decode off; the size inversion is WIDTH-correct
  (a 32-bit BAR's mask inverts in 32-bit width — a 64-bit `~combined+1` would set
  the upper 32 bits to a bogus exabyte size). 64-bit BARs consume the high slot.
- **Cap-walk** (`pci_walk_caps`): the VIRTIO 1.2 4.1.4 vendor-cap list into
  `regions[]`, bounded (48-hop loop guard, the `[0x40,0xFC]` window) and validated
  (BAR index `< PCI_BAR_COUNT`, BAR present, `(u64)offset + (u64)length <= size`).
  First cap of a kind wins.
- **Quiesce-before-free** (`pci_release_bars_and_claim`): clears
  MEM_SPACE + BUS_MASTER in the command register BEFORE dropping the BAR PA claims
  (which may be re-handed-out), on both the last-unref path and the claim-failure
  rollback.

### Lock discipline

`g_pci_lock` (IRQ-safe leaf) guards `g_pci_claims` + the bump arena. NEVER held
across `kobj_mmio_create` (which takes `g_mmio_lock`) — `pci_bar_alloc` releases
`g_pci_lock` before the caller calls `kobj_mmio_create`, so the order is
`g_pci_lock` then (separately) `g_mmio_lock`, never nested. No cycle.

### Handle integration (`kernel/handle.c`)

`KOBJ_PCI = 11` joins `KOBJ_KIND_HW_MASK` -> the 9P transfer path has no
`KOBJ_PCI` case + `handle_dup` rejects it (NoHwDup), both for free (I-5). The
`handle_get` borrow (MAP_BAR / INFO) bumps the ref via `kobj_pci_ref` and pairs
with `handle_put`'s `kobj_pci_unref` — like `KOBJ_LOOM` (both name ref-counted
objects, so the acquire MUST bump or the get/put pairing underflows).

## Data structures

`struct KObj_PCI` (`pci_handle.h`): magic + ref + `(bus,dev,fn,virtio_device_id)`
+ `vpd` (stable pointer into the enumerated `g_virtio_pci_devs[]`) +
`bars[6]` (pa / decoded size / KObj_MMIO claim / present / is_64) +
`regions[4]` (cfg_type-1 indexed) + `notify_off_multiplier` + `intid` /
`intid_valid`.

`struct t_pci_info` (`<thylacine/syscall.h>`, 208 B): the SYS_PCI_INFO copy-out
record. Every field offset + the total size is pinned by `_Static_assert` so a
future field add cannot land without bumping the ABI asserts -> no implicit
padding -> the zero-init + field-fill + byte copy-out leaks nothing.

## State machine

`kobj_pci_claim`: install slot -> bump g_pci_live -> assign+size BARs (decode
off) -> walk caps (validate vs assigned BAR sizes) -> enable MEM-decode +
bus-master -> resolve INTx. Any of assign/walk failing routes through
`kobj_pci_unref` (ref 1 -> free_internal -> release slot + every taken BAR claim +
decrement g_pci_live). Pre-slot-install failures (double-claim / table-full) free
the kmalloc'd object WITHOUT touching g_pci_live (not yet bumped).

## Spec cross-reference

No new ARCHITECTURE.md section-28 invariant. Composes:
- **I-5** — `KObj_PCI` non-transferable (static_assert + dup/transfer rejection).
- **HwResourceExclusive** — one owner per `(bus,dev,fn)`; per-BAR PA ranges inherit
  `KObj_MMIO` overlap exclusivity.
- **I-12** (W^X) — `SYS_PCI_MAP_BAR` maps RW only.
- **I-2 / I-6** — `CAP_HW_CREATE`-gated; the minted handle carries no transferable
  rights; no rights expansion.

## Tests

- `pci.bar_decode_size` — the width-correct inversion vectors (32-bit + 64-bit).
- `pci.claim_*` — claim / double-claim reject / table-full / cap-resolve.
- `pci.syscall_reject` / `pci.syscall_claim_info` — the cap-gate + the INFO copy-out.
- Boot probe: `netdev-pci-driver` (warden-bound, MENAGERIE 6b-3) drives a full ARP
  round-trip over `VirtioNetPci` (claim + map BARs + send + poll_rx + recycle +
  quiesce), narrowed to its `(bus,dev,fn)` PCI allowance axis; runs every boot
  alongside the warden-bound MMIO `netdev-driver`. (Retires the broad-cap
  standalone `netdev-pci-test`.)

## Error paths

| Return | Trigger | Caller action |
|---|---|---|
| `SYS_PCI_CLAIM -1` | no `CAP_HW_CREATE` / device-not-found / already-claimed / BAR-assign fail / malformed-cap-list / OOM / table-full | the driver SKIPs (no net-PCI device) or aborts |
| `SYS_PCI_MAP_BAR -1` | no cap / wrong kind / no `RIGHT_MAP` / bad prot (EXEC, W-without-R, 0) / bar_index out of range or absent / map fault | abort; the claim is intact |
| `SYS_PCI_INFO -1` | bad user buffer / wrong kind / no `RIGHT_READ` / copy-out fault | abort |

## Performance characteristics

Claim is one-shot at driver startup (BAR probe + cap-walk over config space, a
handful of ECAM accesses). The hot path (send / poll_rx) is the BAR-mapped
register transport — single ISV-safe `ldr`/`str` per access, identical cost to
the virtio-mmio bank.

## Status

- **pci-1b** (`89a6c69`): `KObj_PCI` kernel object — claim + BAR-assign +
  cap-resolve + INTID + handle integration + in-kernel tests.
- **pci-1c** (`11eb559`): the three syscalls + the 208 B `t_pci_info` ABI +
  libt / libthyla-rs wrappers + syscall tests.
- **pci-2** (`2908e0e`): userspace `PciDev` + `netdev::VirtioNetPci` (reuses the
  audited `crate::ring`) + the `netdev-pci-test` boot probe (retired at 6b-3 for
  the warden-bound `netdev-pci-driver`).
- **pci-3**: the focused soundness audit (this round) + SMP gate + these docs.

## Known caveats / footguns

The pci-3 focused audit closed CLEAN (0 P0 / 0 P1 / 0 P2 / 4 P3). The four P3
dispositions:

- **BAR mappings outlive the handle** (#847 dual lifetime): `PciDev::Drop` (and a
  `SYS_PCI_CLAIM` handle close) release the claim + drop the per-BAR `KObj_MMIO`
  ref, but a live user mapping's independent Burrow ref keeps that BAR's pages
  mapped until the mapping tears down (proc exit). Mirrors `Mmio`.
- **Partial-map on a `claim` error is proc-exit-bounded (pci-3 F1, tracked)**: if
  `PciDev::claim` maps BAR 0 then fails on a later BAR, the already-mapped windows
  are not explicitly unwound. This is NOT fixable cheaply: `SYS_BURROW_DETACH` is
  confined to the burrow-attach window (`EXEC_USER_BURROW_BASE` = 4 GiB+, the
  security bound protecting ELF/stack/guard VMAs), and the driver-VA windows (BAR +
  DMA, mirroring the byte-identical mmio driver) live below it by design, so every
  virtio driver mapping (MMIO / DMA / BAR) shares the proc-exit-bounded posture.
  `claim` is one-shot at startup and a single-BAR virtio-net-pci never maps a
  second BAR, so the path is unreachable in practice.
- **Hostile-device notify/region bounds (pci-3 F2, hardened)**: the userspace
  driver trusts QEMU at v1.0; a hostile device reporting an out-of-bounds
  notify-offset / region length can only fault a write within the driver's OWN
  address space (terminating the driver Proc), never escalate — the kernel
  validates every config-space field it copies out and keeps ECAM kernel-only.
  `VirtioNetPci` additionally bounds the device-supplied
  `queue_notify_off * notify_off_multiplier` doorbell within the notify region
  length (`NotifyRegionTooSmall`), alongside the `CCFG_MIN_LEN` / `DEVICE_CFG_MIN_LEN`
  region guards.
- **ABI drift-defense (pci-3 F3)**: the libthyla-rs `TPciInfo` mirror pins every
  field offset with `offset_of!` const-asserts against the kernel header, so a
  future kernel field reorder fails the Rust compile too.

## References

- `docs/VIRTIO-PCI-DESIGN.md` — the design charter (the #140 DFS fork).
- `docs/reference/37-virtio_pci.md` — the P4-H ECAM enumerator this builds on.
- `docs/reference/114-netdev.md` — the `VirtioNetPci` driver.
- `docs/reference/89-hardware.md` — `PciDev` in the libthyla-rs hardware substrate.
