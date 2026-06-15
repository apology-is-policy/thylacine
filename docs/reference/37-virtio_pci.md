# Reference: VirtIO PCI transport — ECAM enumeration (P4-H)

## Purpose

`virtio_pci_init` (`kernel/virtio_pci.c`) enumerates VirtIO devices reachable via the PCIe configuration space of QEMU virt's PCIe root complex. It is the PCI-transport counterpart to `virtio_init` (P4-F MMIO transport): both surface the per-device handles that future userspace drivers (P4-I/J/K/L) bind through the Dev/Spoor/handle plumbing.

PCI Express config space is exposed via the **ECAM** (Enhanced Configuration Access Mechanism, PCI Express Base spec §7) — a flat MMIO region where the config space of bus `b`, device `d`, function `f` is at offset `(b<<20) | (d<<15) | (f<<12)`. Each function gets 4 KiB. QEMU virt advertises ECAM at PA `0x40_1000_0000` size 256 MiB via the DTB node `compatible = "pci-host-ecam-generic"`.

At v1.0 P4-H we enumerate **bus 0 only** (the only bus populated in QEMU virt's no-bridge default config) and **identify** VirtIO PCI devices. We do **not** yet map BARs or walk PCI capabilities; that's per-driver work in the chunk that first uses a PCI device.

ARCH §13: "Block / net / input are MMIO-transport for v1.0. GPU is PCI-only on QEMU virt (the virtio-gpu MMIO transport isn't implemented in QEMU). PCI enumeration is required for GPU support."

## Public API

### `<thylacine/virtio_pci.h>`

```c
// Per-device handle.
struct virtio_pci_dev {
    u8       bus;
    u8       dev;             // device number (0..31)
    u8       fn;              // function number (0..7)
    u8       header_type;     // PCI_CFG_HEADER_TYPE & 0x7F
    u16      vendor_id;       // VIRTIO_PCI_VENDOR_ID = 0x1AF4
    u16      device_id;       // 0x1000..0x103F (legacy) or 0x1040..0x107F (modern)
    u16      virtio_device_id;// VIRTIO_DEVICE_ID_* (block, net, rng, gpu, ...)
    u16      subsys_vendor;
    u16      subsys_device;
    u8       class_code;
    u8       subclass;
    u8       prog_if;
    bool     is_modern;       // device_id in modern range [0x1040, 0x107F]
    void    *cfg;             // KVA of this function's 4 KiB config space
};

void virtio_pci_init(void);

int virtio_pci_dev_count(void);
struct virtio_pci_dev *virtio_pci_dev_get(int idx);
struct virtio_pci_dev *virtio_pci_find_by_device_id(u32 virtio_device_id);

u8  virtio_pci_cfg_read8 (const struct virtio_pci_dev *d, u32 off);
u16 virtio_pci_cfg_read16(const struct virtio_pci_dev *d, u32 off);
u32 virtio_pci_cfg_read32(const struct virtio_pci_dev *d, u32 off);
```

`virtio_pci_init` is called once from `boot_main()` after `virtio_init()`. The init guard extincts on double-call. If no PCIe root complex is in the DTB (e.g. future bare-metal targets without PCI), it silently no-ops and `virtio_pci_dev_count()` returns 0.

The config-space accessors return `0xFF` / `0xFFFF` / `0xFFFFFFFF` on out-of-range or misaligned access (matching the natural PCI absent-device sentinel) — never trap.

## Implementation

### `kernel/virtio_pci.c`

```c
1. dtb_get_compat_reg("pci-host-ecam-generic", &ecam_base, &ecam_size)
2. mmu_map_mmio(ecam_base, VIRTIO_PCI_BUS_CFG_SIZE = 1 MiB)  // bus 0 only
3. for dev in 0..32:
     header_type = probe_function(0, dev, 0)
     if header_type & PCI_HEADER_TYPE_MF:
       for fn in 1..8: probe_function(0, dev, fn)
4. boot banner emits count + functions probed
```

`probe_function(bus, dev, fn)`:
- Compute KVA = `g_ecam_kva + (bus<<20 | dev<<15 | fn<<12)`.
- Read `vendor_id` at offset 0. If `0xFFFF`, function absent — return 0.
- Read `device_id`. If `(vendor_id, device_id)` is in the VirtIO range, populate `struct virtio_pci_dev` from the rest of the header. Otherwise skip.
- For modern devices (device_id 0x1040..0x107F), `virtio_device_id = device_id - 0x1040`.
- For legacy devices (device_id 0x1000..0x103F), `virtio_device_id = subsys_device` (per pre-1.0 spec).

The ECAM mapping uses `mmu_map_mmio` (P3-Bb) — page-grain MMIO in the vmalloc range at `VMALLOC_BASE = 0xFFFF_8000_0000_0000`. 1 MiB = 256 pages, leaving ~80 pages of vmalloc headroom after the boot consumers (UART + GIC dist+redist + 32 VirtIO MMIO slots).

### Data structures

`struct virtio_pci_dev` — 32 bytes. Not on disk, not on wire, not ABI-exposed; no `_Static_assert` size pin (would constrain future fields without payoff).

`g_virtio_pci_devs[VIRTIO_PCI_MAX_DEVS = 16]` — BSS-allocated array of handles. 16 was chosen because QEMU virt's default config produces 3-5 PCI VirtIO devices; doubling that for headroom (e.g. virtio-blk + virtio-net + virtio-gpu + virtio-input × 2 + spare) gives the practical upper bound.

`g_ecam_kva` — the KVA of bus 0's config space, returned by `mmu_map_mmio`.

### State machine

```
g_init_called = false (BSS default)
  → virtio_pci_init runs
  → g_init_called = true
  → second call extincts via the guard
```

No reset / re-enumerate path at v1.0. PCI hotplug (a future feature) would require additional surface.

### Spec cross-reference

No new TLA+ at P4-H. Enumeration is read-only on the PCI side: every observable post-init invariant follows from the QEMU virt PCIe config-space layout (immutable for the VM's lifetime) + `mmu_map_mmio`'s VA→PA stability (covered by P3-Bb invariants in `03-mmu.md` + `23-direct-map.md`).

Future PCI capability walking + BAR mapping (deferred chunk) will introduce per-driver state that may benefit from a spec extension if it composes with the BURROW/handle lifecycle in a non-trivial way.

### Tests

`kernel/test/test_virtio_pci.c` — 7 tests:

| Test | What it verifies |
|---|---|
| `virtio_pci.init_called` | `dev_count()` returns a non-negative value within `VIRTIO_PCI_MAX_DEVS`. |
| `virtio_pci.count_within_bound` | Bus 0 has at least 1 VirtIO device (the `tools/run-vm.sh`-attached `virtio-rng-pci`). |
| `virtio_pci.devices_have_vendor` | Every entry has `vendor_id == 0x1AF4`, device_id in one of the two VirtIO ranges, `is_modern` matches the range. |
| `virtio_pci.devices_have_cfg` | Every entry has a non-NULL `cfg` KVA; round-trip `cfg_read16(VENDOR_ID) == d->vendor_id`. |
| `virtio_pci.find_rng` | `find_by_device_id(VIRTIO_DEVICE_ID_RNG)` returns the RNG device. |
| `virtio_pci.find_unknown_returns_null` | Unknown virtio_device_id returns NULL. |
| `virtio_pci.cfg_read_bounds` | Out-of-range offset, misaligned offset, and NULL device pointer all return the bus-floating sentinel (0xFF / 0xFFFF / 0xFFFFFFFF). |

`tools/run-vm.sh` adds `-device virtio-rng-pci,id=rng_pci0` so the integration boot always has at least one PCI VirtIO device available for the assertions to bite.

### Error paths

| Condition | Behavior |
|---|---|
| No `pci-host-ecam-generic` in DTB | Silent skip; `dev_count() == 0`. |
| `mmu_map_mmio` returns NULL (vmalloc exhausted) | Extinct with `"virtio_pci_init: mmu_map_mmio returned NULL (vmalloc exhausted?)"`. |
| Double `virtio_pci_init` call | Extinct with `"virtio_pci_init: double call"`. |
| `g_virtio_pci_dev_count` reaches `VIRTIO_PCI_MAX_DEVS` | Subsequent devices skipped silently (not extincted — operator may legitimately want more). |
| Non-VirtIO device on the bus | Ignored. |
| Out-of-range / misaligned `cfg_read*` | Returns the absence sentinel; no extinction. |

### Performance characteristics

`virtio_pci_init` cost (measured at v1.0 P4-H, QEMU virt 4-core):
- `mmu_map_mmio(1 MiB)` — installs 256 L3 PTEs + 1 broadcast TLB flush ≈ 50 µs.
- 32 function probes (each reads 16 bytes of config space) ≈ 5 µs.
- Total: ~55 µs added to the boot path. Boot-time budget impact: < 0.05 ms; well within VISION §4's 500 ms total budget.

Banner emission is one UART write per device + one summary line.

### Status

- **P4-H landed** at this commit.
- VirtIO PCI device count on the canonical `tools/run-vm.sh` config (with `-device virtio-rng-pci,id=rng_pci0` added): **3** (QEMU virt's default exposes additional VirtIO PCI devices on the same bus).
- 7 tests added; 178/178 total in-kernel.

### Known caveats / footguns

- **Bus 0 only.** Multi-bus support requires either an L2 block descriptor for the full 256 MiB ECAM (consuming 1 L2 entry but bypassing vmalloc's L3 page-grain mapping) or extending vmalloc to multiple L3 tables. QEMU virt's default config has no bridges, so bus 0 enumeration captures everything. Lands when a target needs more.
- **BAR mapping + cap-walk: now in `KObj_PCI`** (pci-1b, `docs/reference/115-pci-claim.md`). The enumerator deliberately keeps `struct virtio_pci_dev` BAR-free; `kobj_pci_claim` (built on this enumerator's config-space accessors) assigns the BARs from the host-bridge window, walks the VirtIO 1.2 §4.1.4 capability list into resolved common/notify/ISR/device regions, and hands BAR mappings to userspace via `SYS_PCI_MAP_BAR`.
- **INTx wiring: now in `KObj_PCI`** (pci-1b). `dtb_pci_intx_route` (pci-1a) walks the DTB `interrupt-map` to swizzle `(dev) × INTA` to a GIC SPI; `kobj_pci_claim` resolves it into the claim's `intid`, forwarded to a userspace driver via `KObj_IRQ`. The GIC distributor stays kernel-reserved.
- **No MSI/MSI-X enable.** Modern transport prefers MSI/MSI-X; we don't yet program the MSI capability. Same defer-to-first-driver story.
- **No bridge / Type 1 header handling.** PCI-to-PCI bridges (header_type 1) are skipped — we don't walk their secondary bus. Adding bridge support requires the per-bridge bus-number programming + secondary bus mapping.
- **Vmalloc pressure.** 256 of 512 vmalloc pages consumed by ECAM mapping; future vmalloc-heavy chunks (Phase 5+ Stratum 9P transport buffers, Phase 6+ network buffers) may need vmalloc expansion. Documented as a Phase 4 trip-hazard.

### Naming rationale

We keep "virtio_pci" / "BAR" / "ECAM" / "BDF" verbatim from PCI/PCIe spec because this layer is an industry-spec implementation — the spec is the contract and renaming would obscure cross-implementation comparison. Same precedent as `virtio_mmio` (P4-F) keeping "MMIO" / "vring" / "QUEUE_SEL". Thylacine renames stick where Thylacine invented or rebranded the concept (Chan → Spoor; devtab → bestiary); industry-spec surfaces stay canonical.

### References

- [VIRTIO 1.2 spec §4.1 (PCI transport)](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html)
- PCI Local Bus Specification, Rev 3.0
- PCI Express Base Specification, Rev 6.0 §7.2.2 (ECAM)
- `docs/reference/35-virtio.md` — sibling MMIO transport reference.
- `docs/reference/115-pci-claim.md` — `KObj_PCI`, the claim layer built on this enumerator (BAR-assign + cap-walk + INTx + the 3 PCI syscalls).
- `docs/reference/23-direct-map.md` — `mmu_map_mmio` semantics and vmalloc layout.
- `docs/ARCHITECTURE.md §13` — VirtIO transport mix rationale.
