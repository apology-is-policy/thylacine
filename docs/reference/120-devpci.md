# 120 — devpci: the kernel-mediated PCI topology Dev (Menagerie 6b)

## Purpose

`devpci` (`dc='P'`, mounted at `/hw/pci`) re-publishes the kernel's boot-enumerated
PCI functions as a read-only, walkable namespace. It is the **mediated PCIe
discovery source**: on a platform where the kernel owns PCIe config space
(QEMU-virt's ECAM is `mmu_map`-enumerated at boot into `g_virtio_pci_devs[]` and
**I-5-reserved**), a sandboxed userspace source cannot scan config space, so the
kernel — which already poked the bus once, at boot — exposes the *result* here. The
[warden](119-warden.md)'s in-process `PciSource` reads `/hw/pci` exactly as
`DtbSource` reads [`/hw`](#) (devhw), binds each function by its derived virtio
identity, and confers the narrowed I-34 PCI allowance — never touching a device
register itself (MENAGERIE §3).

The privilege posture is the pci-3 invariant: **userspace never gets raw ECAM, and
there is no config-space write surface.** devpci serves *only* bounded topology
(bdf + vendor/device → derived virtio id + the INTx-routed INTID), read kernel-side
from the already-mapped config space; the interrupt pin is the only config-space
read it does, and only the derived GIC INTID crosses to userspace.

Design: `docs/MENAGERIE.md` §7 (the two PCIe-source realizations — mediated devpci
vs the self-scanning brcmstb host bridge) + §16-6b. Scripture commit `f0878ba`.

## The namespace

```
/hw/pci                       the PCI root              (QTDIR)
/hw/pci/<bus.dev.fn>          one function's directory  (QTDIR)
/hw/pci/<bus.dev.fn>/ctl      that function's info      (QTFILE; one text line)
```

`<bus.dev.fn>` is the function's bdf in bare lowercase hex (e.g. `0.1.0`). The
PciSource enumerates the root with `read_dir`, then opens each `<bdf>/ctl` by name.

### The `ctl` line (the source ↔ kernel wire contract)

One line, newline-terminated:

```
v1 bus=<hex> dev=<hex> fn=<hex> vendor=<hex> device=<hex> virtio=<dec> intid=<hex|none>
```

- `bus`/`dev`/`fn`/`vendor`/`device`/`intid` — bare lowercase hex (no `0x`).
- `virtio` — the derived virtio device-id in **decimal** (matching the libdriver
  `virtio:N` / `virtio-pci:N` id convention; net=1, blk=2, gpu=16).
- `intid` — the INTx-routed GIC INTID, or the literal `none` when the function
  declares no INTx pin (`PCI_CFG_INT_PIN == 0`) or the DTB has no route for it.

The `v1` prefix is the version token; a future incompatible field change bumps it
and the PciSource rejects an unknown version (fail-closed on a build skew). Field
order is fixed but the parser is key-based, so an appended field is forward-compatible.

## Implementation (`kernel/devpci.c`)

### Qid encoding

```
path == 0          the PCI root directory (the conventional Dev-root path)
path even, != 0    a function directory; index = (path >> 1) - 1
path odd           a function's ctl file;  index = (path >> 1) - 1
```

Function `i` (0-based, an index into `g_virtio_pci_devs[]`) owns dir `(i+1)*2` and
ctl `(i+1)*2 + 1`. The device array is built once at boot (`virtio_pci_init`) and is
**immutable** thereafter, so an index minted by a successful walk stays valid for
the kernel's lifetime — no refcount, no lifetime hazard, and (read-only over
immutable data) no lock is required, which is why devpci is SMP-safe by construction.

### Walk / read / readdir

`walk_one` dispatches: root → a `<bdf>` directory (matched by formatting each
function's bdf and comparing); a `<bdf>` directory → its sole `ctl` leaf; `..`
climbs (ctl → its dir, dir/root → root). A ctl leaf is terminal.

`read` of a ctl builds the line into a 96-byte stack buffer (`build_ctl`) and copies
out the `[off, off+n)` slice with EOF semantics; a directory read returns -1 (readdir
is the path, matching devhw/devctl). `readdir` emits one 9P2000.L dirent per child
with a strictly-increasing, never-0 cookie (a 1-based ordinal — the function index+1
at the root, 1 for the single ctl) — the `SYS_READDIR` pagination contract.

Number formatting is self-contained (`pf_hex`/`pf_dec`/`pf_str` — the devctl/devproc
idiom; each returns 0 on overflow so the line-builder degrades to a short-but-valid
line rather than overrunning).

### The `/hw/pci` mount (the devhw synthetic child)

devpci nests under [devhw](#) (`/hw`). `kernel/devhw.c` grows a **synthetic `pci`
mount-point child**: `walk_one` resolves root + `"pci"` → a sentinel qid
(`HW_SYNTH_PCI`, bit 62 set — distinct from every FDT node/prop offset, those being
< 2³¹; bit 63 stays 0 so it is never decoded as a property), and `stat_native` /
`readdir` special-case it (a directory, empty until devpci mounts over it). No real
FDT node is named `pci` (QEMU-virt / RPi expose `pcie@...`), so the synthetic child
shadows nothing.

`kernel/joey.c` mounts devpci at `/hw/pci` **after** `/hw` (so the path resolves by
crossing the `/hw` mount into devhw, then `STALK_MOUNT`-resolving the `pci` child):
`joey_mount_static_dev(kt, &devpci, "hw/pci", 6)`.

## Data structures

devpci holds no state of its own; it is a stateless view over `struct
virtio_pci_dev g_virtio_pci_devs[]` (`kernel/include/thylacine/virtio_pci.h`),
reached through the public `virtio_pci_dev_count()` / `virtio_pci_dev_get(idx)`.
The only fields read: `bus`/`dev`/`fn`/`vendor_id`/`device_id`/`virtio_device_id`,
plus a config-space read of `PCI_CFG_INT_PIN` (0x3d) fed to
`dtb_pci_intx_route(dev, pin, &intid)`.

## Tests (`kernel/test/test_devpci.c` + `kernel/test/test_devhw.c`)

| Test | Covers |
|---|---|
| `devpci.bestiary_smoke` | registration (`dc='P'`, `dev_lookup_by_*`), `perm_enforced == false` |
| `devpci.attach_root` | root attach → qid 0 / QTDIR; `stat_native` SYSTEM-owned IFDIR |
| `devpci.walk_read_ctl` | root → `<bdf>` → `ctl` → read; the ctl line is versioned + carries every field + is newline-terminated; EOF + stat-size; a ctl leaf is terminal *(guarded on count > 0)* |
| `devpci.readonly` | write / create / wstat / bwrite reject on a dir AND a ctl (the I-5 no-config-write property) |
| `devpci.readdir` | root lists one QTDIR per function with strictly-increasing non-zero cookies; each function dir lists exactly its `ctl` QTFILE |
| `devpci.walk_reuse_nc` | the #57a reuse-`nc` shape the `/hw/pci` mount-cross depends on (0-element returns nc unchanged; 1-element advances nc) |
| `devhw.synth_pci_child` | the devhw surgery: root + `"pci"` → the synthetic QTDIR child; `..` → FDT root; stat IFDIR; empty pre-mount readdir |

The live mount + cross is proven by the boot (joey mounts devpci → the warden's
`/hw/pci` reads), not a unit test. The boot run is `930/930 PASS` + `/hw/pci
mounted` + 0 EXTINCTION.

## Error paths

- A directory `read` → -1 (readdir is the byte path).
- A `read` / `stat_native` of a stale ctl qid naming no current function → -1.
- Every mutation (`write`/`create`/`wstat`/`bwrite`/`remove`) → -1 / NULL.
- A `readdir` whose buffer cannot hold the first entry → -1 (never 0 — 0 means EOD).

## Status

Landed at Menagerie build-arc **6b-1**. The PciSource consumer + the
`netdev-pci-driver` bind (the live I-34-on-PCI proof) land at 6b-2 / 6b-3; the
focused audit + SMP gate at 6b-4.

## Known caveats / seams

- **`/hw` does not list `pci` in its readdir.** The synthetic child is walkable +
  mountable but not enumerated in `ls /hw` (the readdir-cookie injection was
  deliberately skipped — the warden walks `/hw/pci` directly; the PciSource never
  readdir's `/hw` to find it). Cosmetic; a v1.x nicety.
- **Post-pivot re-graft.** `/hw/pci` is mounted in the kproc boot namespace and read
  by the pre-pivot warden; the nested mount is dropped (uncollected — #80) at pivot
  and **not** re-grafted post-pivot. A post-pivot `lspci`-style consumer is a v1.x
  item (it would re-graft `/hw/pci` alongside `/hw`). Each such boot mount consumes a
  `PGRP_MAX_MOUNTS` slot until the pivot-time GC (#80) lands; the cap was bumped 12→16
  to accommodate `/hw/pci` plus headroom.
- **claim-by-bdf.** A multi-function device exposing two functions of one virtio id
  needs a claim-by-bdf ABI to disambiguate (`SYS_PCI_CLAIM` resolves the first
  match); with one NET function on QEMU-virt the resolution is unambiguous. See
  [115-pci-claim.md](115-pci-claim.md) / [117-allowance.md](117-allowance.md).
