# 118 - libdriver: the Menagerie driver framework crate

**Status**: as-built through Menagerie build-sequence step 6b-2 (2026-06-17). Pure
userspace; no kernel ABI. The first consumer is the warden (step 5c); step 5d-1
added the **discovery-source layer** (`source`): the typed `DeviceId` / `DeviceNode`,
the `DiscoverySource` trait, the `DtbSource`, the source -> warden node-record
codec, and bind-by-id (`best_match` over typed identities). The first real
`impl Driver` is the netdev retrofit (step 5d-3). Step 5d-4 (the focused audit)
added the trust-boundary enforcement `reconcile_reported_node` (the warden
constrains a source's reported resources to its own trusted view) + the `MAX_IDS`
record bound. Step **6b-2** added the **PCI axis**: a `DeviceId::VirtioPci(u16)`
identity (distinct from the MMIO `Virtio`), a `PciNeed` manifest axis + a
`NodeResources`/`BoundResources` `pci: Option<(u8,u8,u8)>` bdf carried through
`resolve` + the descriptor codec + `to_allowance` (`push_pci`, the 6a I-34 PCI
allowance axis), the `parse_pci_ctl` devpci ctl-line parser, and the in-process
`PciSource` over `/hw/pci` ([devpci](120-devpci.md), the DtbSource sibling).

Crate: `usr/lib/libdriver` (native `libthyla-rs`, `no_std` + `alloc`). Design:
`docs/MENAGERIE.md` §6 (anatomy of a driver + the manifest), §4 (the hardware
allowance / I-34), §5 (the warden).

---

## Purpose

A Thylacine driver is a userspace Proc holding only its device's narrowed,
non-transferable hardware allowance (I-34). `libdriver` is the scaffold that
makes one *droppable* and its grant *auditable*: it removes the spawn/argv/handle
boilerplate so a new driver is `probe()` + the device logic + a served file, and
it puts the auditable grant computation (the node-INTERSECT-needs intersection)
in one host-tested place.

It sits between two roles:

- **The warden** (the device-manager TCB Proc, step 5c) enumerates device nodes
  from its discovery sources (`DtbSource` over `/hw`; the virtio-mmio bus source
  added in step 5d-2), matches each node's typed identity (`DeviceId`) against the
  manifests in its bind database (`best_match`), calls `resolve` to compute the
  narrowed grant, derives both the kernel allowance (`to_allowance` ->
  `Command::allowance`) and the spawn descriptor (`to_descriptor` ->
  `Command::arg`) from that one grant, and spawns the driver.
- **The driver** (built on the framework) decodes the descriptor in `bind`,
  mints its `KObj_MMIO`/`IRQ`/`DMA` handles from the grant (the helpers), and
  serves a file.

`BoundResources` is the shared currency: one value the warden computes + encodes
and the driver decodes, so the authority the kernel enforces and the resources
the driver maps never diverge.

---

## Layering

Mirrors the kaua/netdev split (pure host-testable core + a thin device layer):

| Module | Feature | Builds on | Role |
|---|---|---|---|
| `manifest` | always | `alloc` only | the §6 brace-block manifest schema + parser + `to_text` |
| `resource` | always | `alloc` only | `NodeResources`, `BoundResources`, `resolve`, the descriptor codec |
| `dtb` | always | `alloc` only | decode raw devhw `/hw` property bytes (compatible/reg/interrupts) into a `NodeResources` |
| `source` | always (+ `DtbSource` under `driver`) | `alloc` (+ `libthyla-rs` for `DtbSource`) | the discovery-source abstraction: typed `DeviceId` / `DeviceNode`, `DiscoverySource`, `best_match`, the source -> warden node-record codec; `DtbSource` (the `/hw` source) |
| `supervise` | always | nothing (Copy enums + `Restart`) | the warden's restart-loop *decision*: `RunOutcome` / `Disposition` / `SuperviseStep`, `next_step` (restart-vs-settle per `Restart` policy), `backoff_ms` -- the impure loop (spawn/reap/sleep) lives in the warden |
| `readyline` | always | `alloc` only | the warden's driver-readiness line assembly: `feed_ready_line` (accumulate chunks, scan for '\n', cap at `READY_LINE_MAX`) + `ReadyLine`. The PURE half of the readiness read; the warden does the impure single bounded read and feeds chunks here, so a partial line never blocks the reader mid-line (5e-4 audit F1) |
| `driver` | `driver` (default) | `libthyla-rs` | the `Driver` trait, `run`, the handle-mint helpers, `to_allowance` |

`manifest` + `resource` + `dtb` + `supervise` + the `source` *types* (`DeviceId` /
`DeviceNode` / the codec / `best_match`) carry no `libthyla-rs` dependency, so the
grant logic + the codecs + the endianness/cell-width DTB decode + the bind matching
+ the supervision state machine run under `cargo test` on the host. The `driver` surface + the concrete `DtbSource` (gated
inside `source`) are the only `libthyla-rs` code; they are compiled for
`aarch64-unknown-none` only.

```
Build (device): cargo build -p libdriver               # default features -> driver
Host tests:     cargo test -p libdriver --no-default-features --target <host>
```

---

## Public API

### `manifest`

```rust
pub const MANIFEST_ABI: u32 = 1;

pub struct Manifest {
    pub name: String,
    pub abi: u32,
    pub binds: Vec<String>,   // compatible strings, most-specific first
    pub needs: Needs,
    pub serves: String,        // path template; "%instance" expands per bind
    pub restart: Restart,
    pub lifecycle: Lifecycle,  // Persistent (left running on READY) | Transient (default)
    pub sig: Option<String>,   // section-9 authorization input; carried verbatim
}
pub struct Needs { pub mmio: MmioNeed, pub irq: IrqNeed, pub dma: DmaNeed, pub pci: PciNeed }
pub enum MmioNeed  { None, Node }                 // "node:reg"
pub enum IrqNeed   { None, Node, Msi(u32) }       // "node:interrupts" | "msi:N"
pub enum DmaNeed   { None, Pool(u64) }            // "pool: N" bytes
pub enum PciNeed   { None, Node }                 // "node" -- the bound function's (bus,dev,fn)
pub enum Restart   { Never, OnCrash, Always }
pub enum Lifecycle { Transient, Persistent }      // warden owns teardown | leave running on READY

impl Manifest {
    pub fn parse(text: &str) -> Result<Manifest, Error>;  // the section-6 brace block
    pub fn to_text(&self) -> String;                      // the inverse (round-trips)
}
```

The on-disk form is the §6 brace block:

```
driver "rp1-eth" {
    abi      = 1
    binds    = ["raspberrypi,rp1-eth", "brcm,genet-v5"]
    needs {
        mmio = "node:reg"        # the bound node's own reg window(s)
        irq  = "msi:1"           # one MSI vector  (or "node:interrupts" for wired)
        dma  = "pool: 2 MiB"
    }
    serves    = "/dev/net/%instance"
    restart   = on-crash
    lifecycle = persistent                    # a standing NIC service (default: transient)
    sig       = "<ed25519 over the package>"  # optional
}
```

Required keys: `abi`, `binds` (non-empty), `serves`. Optional: `needs` (defaults
to all-`None`), `restart` (defaults to `OnCrash`), `lifecycle` (defaults to
`Transient`), `sig`. `#` starts a
line-comment. Sizes accept binary units (`2 MiB`, `512 KiB`, `1 GiB`, `64 B`, or
a bare byte count).

### `resource`

```rust
pub const DESCRIPTOR_VERSION: u32 = 1;
pub const MAX_MMIO: usize = 8;   // mirrors T_ALLOWANCE_MMIO_MAX
pub const MAX_IRQ:  usize = 8;   // mirrors T_ALLOWANCE_IRQ_MAX

pub struct NodeResources {       // what the warden reads from /hw (or /hw/pci)
    pub compatible: Vec<String>, // most-specific first
    pub reg: Vec<(u64, u64)>,    // (base, size) MMIO windows
    pub interrupts: Vec<u32>,    // wired GIC INTIDs
    pub pci: Option<(u8,u8,u8)>, // a virtio-PCI node's (bus,dev,fn); None for a DTB node
}
pub struct BoundResources {      // the narrowed grant (the shared currency)
    pub instance: u32,
    pub compatible: String,      // the matched compatible
    pub serves: String,          // "%instance" expanded
    pub mmio: Vec<(u64, u64)>,   // <= MAX_MMIO, a subset of the node's reg
    pub irq: Vec<u32>,           // <= MAX_IRQ, a subset of the node's interrupts
    pub dma_max: u64,
    pub pci: Option<(u8,u8,u8)>, // the granted function bdf (== node's), or None
}

pub fn resolve(manifest: &Manifest, node: &DeviceNode, instance: u32)
    -> Result<BoundResources, Error>;
pub fn expand_instance(template: &str, instance: u32) -> String;

impl BoundResources {
    pub fn to_descriptor(&self) -> Result<String, Error>;          // -> Command::arg
    pub fn parse_descriptor(desc: &str) -> Result<BoundResources, Error>;
}
```

### `dtb`

The warden's DTB discovery-source decode -- it reads each `/hw/<node>/<prop>`
file (the devhw tree publishes every FDT property verbatim, big-endian on-wire)
and turns the raw bytes into the typed `NodeResources` that `resolve` intersects.
Pure (no `libthyla-rs`), so the endianness + cell-width + SPI-mapping logic is
host-tested against the real QEMU-virt bytes.

```rust
pub const ARM64_ADDR_CELLS: u32 = 2;     // FDT root #address-cells (ARM64)
pub const ARM64_SIZE_CELLS: u32 = 2;     // FDT root #size-cells
pub const GIC_INTERRUPT_CELLS: u32 = 3;  // GIC #interrupt-cells: <type number flags>

pub fn parse_compatible(bytes: &[u8]) -> Vec<String>;  // NUL-separated, most-specific first
pub fn parse_reg(bytes: &[u8], addr_cells: u32, size_cells: u32) -> Vec<(u64, u64)>;
pub fn parse_interrupts(bytes: &[u8], interrupt_cells: u32) -> Vec<u32>;  // -> absolute INTIDs

impl NodeResources {
    pub fn from_dtb(compatible: &[u8], reg: &[u8], interrupts: &[u8],
                    addr_cells: u32, size_cells: u32, interrupt_cells: u32) -> NodeResources;
}
```

`parse_interrupts` maps the GIC 3-cell form to absolute INTIDs (SPI = number+32,
PPI = number+16 -- matching the kernel `lib/dtb.c` convention); unknown interrupt
types (e.g. the PMU/timer affinity rows) are skipped. The cell counts are passed
in (v1.0 uses the universal ARM64 values; reading them from the DTB root + the
interrupt-parent is a v1.x refinement). Decode is best-effort: a malformed
property yields a short/empty axis rather than a panic.

### `source`

The discovery-source abstraction (MENAGERIE §3 + §7) -- the layer the warden binds
*through*. A `DiscoverySource` turns a discovery domain into a list of typed
`DeviceNode`s; the warden matches each node's identity (`DeviceId`) against the
bind DB (`best_match`) and never reads a device register itself. Pure except the
concrete `DtbSource` (gated under `driver`).

```rust
pub enum DeviceId {
    Compatible(String),  // a DTB compatible string, e.g. "arm,pl061"
    Virtio(u16),         // a virtio-MMIO device-id;  string form "virtio:<n>"
    VirtioPci(u16),      // a virtio-PCI  device-id;  string form "virtio-pci:<n>"
}
impl DeviceId {
    pub fn parse(s: &str) -> DeviceId;     // "virtio-pci:N"->VirtioPci; "virtio:N"->Virtio; else Compatible
    pub fn as_string(&self) -> String;     // the canonical (binds / record) form
    pub fn matches_bind(&self, bind: &str) -> bool;
}

pub const MAX_IDS: usize = 16;   // a node record's id-list bound (trust boundary)

pub struct DeviceNode { pub label: String, pub ids: Vec<DeviceId>, pub resources: NodeResources }
impl DeviceNode {
    pub fn from_compatible(label: &str, resources: NodeResources) -> DeviceNode;
    pub fn to_record(&self) -> Result<String, Error>;             // source -> warden line
    pub fn parse_record(line: &str) -> Result<DeviceNode, Error>; // strict + bounded
}

pub trait DiscoverySource { fn enumerate(&mut self) -> Vec<DeviceNode>; }
pub fn best_match(db: &[Manifest], node: &DeviceNode) -> Option<usize>; // most-specific id wins

/// Constrain a source-reported node to the warden's trusted device view: match it
/// to a trusted slot by reg base + rebuild its resources from that slot. The
/// discovery-source trust boundary ENFORCED -- a non-TCB source supplies identity,
/// the warden supplies resources, so a source can mis-identify (caught downstream)
/// but never fabricate a reg/INTID. None if the reported node names no trusted slot.
pub fn reconcile_reported_node(reported: &DeviceNode, trusted: &[DeviceNode]) -> Option<DeviceNode>;

/// Parse one devpci `ctl` line (`v1 bus=.. dev=.. fn=.. vendor=.. device=.. virtio=.. intid=..`)
/// into a typed `VirtioPci` node. Pure (host-tested). None on a malformed line or
/// a virtio=0 function. Lenient (tolerates an unknown appended field) -- devpci is
/// the kernel/TCB, not the non-TCB bus-source boundary `parse_record` guards.
pub fn parse_pci_ctl(label: &str, line: &str) -> Option<DeviceNode>;

#[cfg(feature = "driver")]
pub struct DtbSource { /* enumerate /hw      -> Compatible nodes */ }
#[cfg(feature = "driver")]
pub struct PciSource { /* enumerate /hw/pci  -> VirtioPci nodes  */ }  // 6b-2
```

`DeviceId` is extensible: `VirtioPci(u16)` (step 6b-2) is the first bus id added
after `Virtio`; USB `vid:pid` joins as that source lands (each a new variant + a
`bus:`-prefixed string form). A DTB `compatible` never contains `:`, so the typed
namespace cannot collide with one; an unrecognized `bus:` prefix stays
`Compatible` (fail-closed forward-compat). **`VirtioPci` is deliberately distinct
from `Virtio`**: the two virtio transports have different claim paths (a
virtio-PCI driver claims its function via `SYS_PCI_CLAIM` + maps BARs over the
`KObj_PCI`; a virtio-mmio driver mints an MMIO handle over its allowance window),
so a manifest binds exactly one (`"virtio-pci:1"` vs `"virtio:1"`) and the two
never collide. The prefixes are disjoint -- `"virtio-pci:1"` does not start with
`"virtio:"` -- so `parse` cannot confuse them.

**The PCI axis (step 6b-2).** A virtio-PCI node carries no `reg`/`interrupts`
windows the way a DTB node does -- its identity + resources are its `(bus,dev,fn)`
bdf (the `pci` field) plus the INTx-routed INTID. The manifest selects it with
`pci = "node"` (the `PciNeed::Node` axis), and `resolve` confers the node's bdf
verbatim into `BoundResources.pci` (the I-34 grant property on the PCI axis: the
granted bdf is the node's, never fabricated). The grant carries **no MMIO**: a
virtio-PCI driver's BARs are handle-gated (`SYS_PCI_MAP_BAR` over the `KObj_PCI`
the driver `SYS_PCI_CLAIM`s), not allowance windows, so the PCI axis adds a bdf to
the allowance (`to_allowance` -> `push_pci`, the 6a kernel gate) and nothing to
`mmio`.

The **node record** is the source -> warden wire form
(`v1;label=..;id=virtio:1;reg=base:size,..;intid=hex,..`), used when a source runs
as a separate Proc (the virtio-mmio source, step 5d-2) and reports nodes over a
pipe. `parse_record` is strict + bounds every list -- the resource counts to the
allowance caps (`MAX_MMIO` / `MAX_IRQ`) and the id list to `MAX_IDS` -- the
discovery-source trust boundary, so a hostile or buggy source cannot make the
warden over-allocate. `DtbSource` runs *in-process* in the warden, so its
`Compatible` nodes (which carry commas) never cross the record channel; the encoder
rejects a comma/`;` in an id, fail-closed.

Parsing a record is not the same as trusting it. `reconcile_reported_node` (step
5d-4) is the warden's **enforced** containment: the warden matches each reported
node to a slot in its OWN trusted DTB view (by reg base) and rebuilds the node's
reg/INTID from that trusted slot -- so a source supplies only the device IDENTITY
(its DeviceID -> a typed `Virtio(n)`), never the resources. A compromised source
can at most mis-identify a real slot (the driver's own device re-validation catches
a wrong device) or name a non-existent slot (rejected, `None`); it can never
fabricate a reg/INTID to inflate a peer driver's conferred allowance. This
generalizes to the recursive PCIe/USB sources, where the parent's granted slot
table is always the containment bound.

### `driver` (feature `driver`)

```rust
pub trait Driver: Sized {
    fn probe(res: &BoundResources) -> Result<Self, Error>;        // mint handles, bring up
    fn serve(self, res: &BoundResources) -> Result<(), Error>;    // publish + service
}

pub fn run<D: Driver>() -> !;                  // bind -> probe -> serve -> exit code
pub fn bind() -> Result<BoundResources, Error>; // decode argv[1]
pub fn to_allowance(res: &BoundResources) -> TAllowanceDesc;       // warden-side mirror

pub struct DriverVa(/* private */);            // page bump allocator from DRIVER_VA_BASE
pub fn map_mmio(res, idx, &mut DriverVa) -> Result<Mmio, Error>;
pub fn claim_irq(res, idx) -> Result<Irq, Error>;
pub fn alloc_dma(res, size, &mut DriverVa) -> Result<Dma, Error>;

pub const EXIT_OK: i64 = 0;     // clean serve stop
pub const EXIT_BIND: i64 = 71;  // descriptor missing/unparseable (a warden bug)
pub const EXIT_PROBE: i64 = 72; // device init failed
pub const EXIT_SERVE: i64 = 73; // serve loop errored

pub mod example { pub struct NopDriver; pub fn nop_entry() -> !; } // the compile-proof null driver
```

A complete driver:

```rust
#![no_std] #![no_main]
struct MyNic { mmio: Mmio, irq: Irq, dma: Dma }
impl libdriver::Driver for MyNic {
    fn probe(res: &BoundResources) -> Result<Self, libdriver::Error> {
        let mut va = libdriver::DriverVa::new();
        let mmio = libdriver::map_mmio(res, 0, &mut va)?;
        let irq  = libdriver::claim_irq(res, 0)?;
        let dma  = libdriver::alloc_dma(res, 0x1000, &mut va)?;
        // ...bring the device up...
        Ok(MyNic { mmio, irq, dma })
    }
    fn serve(self, res: &BoundResources) -> Result<(), libdriver::Error> {
        // ...publish res.serves, service requests until removal...
        Ok(())
    }
}
#[no_mangle] fn main() { libdriver::run::<MyNic>() }
```

---

## Implementation

### The grant (`resolve`) -- the auditable I-34 property

`resolve` is the warden's grant computation, isolated in one host-tested
function. For each axis the **manifest selects**, the **node supplies** the
values, so the grant can never exceed the device's physical resources:

- `MmioNeed::Node` -> every `reg` window the node exposes (capped at `MAX_MMIO`);
  `None` -> nothing.
- `IrqNeed::Node` -> every wired INTID (capped at `MAX_IRQ`); `Msi(_)` -> nothing
  *from a DTB node* (MSI vectors come from the PCIe source on real hardware, not a
  DTB node); `None` -> nothing.
- `DmaNeed::Pool(n)` -> a budget of `n` bytes (DMA is allocated memory, not a node
  resource); `None` -> 0.
- `PciNeed::Node` -> the bound node's `(bus,dev,fn)` bdf verbatim (the driver
  `SYS_PCI_CLAIM`s it); `None` -> no bdf. A `PciNeed::Node` manifest against a node
  with no bdf (a DTB node) yields `None` -- fail-closed, the same lenience the
  MMIO/IRQ axes have when the node supplies nothing.

A manifest cannot widen its reach -- it can only decline an axis. The
host tests `resolve_grant_never_exceeds_node` (MMIO/IRQ) and
`resolve_pci_grant_equals_node_bdf` (PCI) assert every granted resource is a
member of the node's set (the I-34 property in a test).

### The descriptor codec -- one argv slot

The grant crosses the spawn boundary as a single argv element (the warden's
`Command::arg`), because `SYS_SPAWN_ARGV_MAX` is 16 elements and a flag-per-window
encoding would exhaust it. The form:

```
v1;inst=0;compat=virtio,mmio;serve=/dev/net/0;mmio=a003000:1000,a004000:200;irq=2a;dma=200000
```

Fields are `;`-delimited `key=value`; numbers are hex except `inst` (decimal);
`mmio`/`irq` are comma-separated lists, omitted when empty; `dma` is always
present. A `pci=<bus>.<dev>.<fn>` field (bare hex, the devpci form) is appended
when the grant carries a bdf, omitted otherwise -- so a virtio-PCI grant looks
like `v1;inst=0;compat=virtio-pci:1;serve=/dev/net/0;irq=23;dma=10000;pci=0.1.0`
(no `mmio`, the BARs being handle-gated). `compat` is taken verbatim (its internal
commas, e.g. `raspberrypi,rp1-eth`, are not list separators -- only `mmio`/`irq`
values are comma-split). `parse_descriptor` is strict: an unknown/duplicate key, a
bad number, a window missing its `base:size` colon, a bdf with the wrong component
count or an out-of-u8 component, an unknown version, or a count over the caps all
reject. `to_descriptor` rejects a `compat`/`serve` containing the field delimiter
`;` (fail-closed against a malformed node).

### `run` -- the lifecycle

`run::<D>()` is bind -> probe -> serve -> a lifecycle exit code:

1. `bind` decodes argv[1]; on failure exit `EXIT_BIND` (a warden bug -- not a
   driver crash the supervisor should blindly restart).
2. `D::probe` brings the device up; on failure exit `EXIT_PROBE`.
3. `D::serve` runs until removal or a fatal error; `Ok` -> `EXIT_OK`, `Err` ->
   `EXIT_SERVE`.

Terminal exits use `t_exit_group` (whole-Proc termination, I-24), correct whether
the driver spawned data-path threads or not. The supervisor (step 5e) reads the
exit code for its restart policy.

### The handle-mint helpers + authority vs information

`map_mmio`/`claim_irq`/`alloc_dma` draw a resource from the grant and call the
`libthyla_rs::hardware` RAII constructor (`Mmio::new` R+W, no EXEC per I-12;
`Irq::new` SIGNAL; `Dma::new` R+W). MMIO/DMA map targets come from `DriverVa`, a
page bump allocator over `DRIVER_VA_BASE` (16 MiB) -- a private region clear of
the fixed VAs the existing transports hardcode (netdev's MMIO/DMA at
`0x50_0000..0x63_0000`; the PCI BAR window at `0x80_0000..0xE0_0000`).

The conferred allowance (5a `Command::allowance`) is the **authority**; the
descriptor only **informs**. A driver that fabricated a PA outside its allowance
is rejected by the kernel I-34 gate at `SYS_MMIO_CREATE` (`allowance_permits` /
`allowance_handle_alloc`), not by the codec -- the helpers' bounds checks are
clarity + early failure, not the security boundary. This is why libdriver is not
a kernel privilege surface: the kernel validates everything.

### `to_allowance` -- the warden's mirror

`to_allowance(res)` turns a `BoundResources` into the `TAllowanceDesc` the warden
passes to `Command::allowance` -- `push_mmio` each window, `push_irq` each INTID,
`set_dma_max`, and (6b-2) `push_pci` the bdf when `res.pci` is `Some` (the 6a I-34
PCI allowance axis the kernel checks at `SYS_PCI_CLAIM`). Because both the
allowance and the descriptor come from the same `BoundResources`, the authority
the kernel enforces and the resources the driver maps cannot drift.

Each MMIO window is **page-rounded** (`resource::page_round`, a pure `pub` helper)
before it goes into the allowance: MMIO is mapped page-granular, so the kernel
I-34 gate checks the driver's *page*-sized `SYS_MMIO_CREATE` against the allowance,
and a sub-page device register (a virtio-mmio slot is 0x200) is only reachable by
mapping its whole 4 KiB page. The descriptor keeps the exact (sub-page) window, so
the driver still learns its precise slot address; the allowance covers the page so
the map is admitted. For a virtio-mmio net slot this grants the page shared with an
adjacent blk slot -- the documented #140 / net-2 co-residency over-grant. (5d-3:
the bug this fixes is that a sub-page slot grant is unmappable -- the driver's
page-map exceeds the slot window and the gate, correctly, rejects it.)

### `PciSource` -- the in-process /hw/pci source (6b-2)

`PciSource` is the `DtbSource` sibling for the PCI bus: it `read_dir`s `/hw/pci`
(the kernel-mediated [devpci](120-devpci.md) topology), opens each `<bdf>/ctl`,
and feeds the line to `parse_pci_ctl` to build a `VirtioPci` node. On QEMU-virt the
kernel owns ECAM and re-publishes the enumerated functions at `/hw/pci/<bdf>/ctl`,
so the source is itself the kernel-TRUSTED view -- it reads the mediated result
**in-process** (no poking Proc, never touching config space) and (unlike the
non-TCB virtio-mmio bus source) needs **no** `reconcile_reported_node` step. The
node record codec (`to_record`/`parse_record`, the source -> warden *pipe* form) is
**not** extended with a PCI bdf: the only PCI source is in-process and never
crosses that pipe, so a bdf on the wire is a forward seam for a future
out-of-process PCIe source. Best-effort like every source: a malformed `ctl` line
is skipped; a missing `/hw/pci` yields no nodes.

The split mirrors the DtbSource: `parse_pci_ctl` is the **pure** line parser
(host-tested -- the devpci -> source channel is the input surface), and `PciSource`
is the thin `driver`-gated fs-reading shell over it. `parse_pci_ctl` is lenient
(it tolerates an unknown appended field, honoring the devpci `v1` forward-compat
promise) because devpci is the kernel/TCB -- distinct from `parse_record`'s
hostile-source-hardened strictness, since that codec IS the non-TCB bus-source
boundary.

---

## Tests

80 host tests (`cargo test -p libdriver --no-default-features --target <host>`):

- **manifest** (9): parse the §6 example; `to_text` round-trip; wired-IRQ +
  no-DMA; defaults when optional fields absent; the size-unit parser
  (`MiB`/`KiB`/`GiB`/`B` + overflow + bad-unit rejection); malformed-manifest
  rejection (unterminated block, unquoted name, missing/duplicate/unknown key,
  empty binds, bad need value, bad restart, trailing garbage); comment +
  whitespace tolerance; **the `pci = "node"` axis** parses + round-trips (a
  virtio-PCI manifest with `pci`/`irq`/`dma` and no `mmio`) + rejects a bad value.
- **resource** (20): `resolve` for the MSI manifest (node MMIO granted, no DTB
  IRQ) and the wired manifest (node interrupts granted); the
  grant-never-exceeds-node property; `NoMatch`; `TooManyWindows`; the descriptor
  round-trip (full + empty axes); the exact descriptor shape; strict rejection
  (bad version, missing required field, duplicate/unknown key, bad number,
  missing window colon); the over-cap rejection; the delimiter-injection guard;
  the end-to-end resolve -> encode -> decode equality; `page_round` covering a
  sub-page slot, a page-straddling window, an already-aligned window, and zero
  size (the 5d-3 allowance page-rounding); **the PCI axis** (6b-2): a PCI node
  grants its bdf + INTID and no MMIO; the grant bdf equals the node's; a node with
  no bdf yields None (fail-closed); a no-PCI-need manifest ignores a node bdf; the
  descriptor pci round-trip + exact shape (`;pci=0.a.1`) + rejection (too few/many
  components, non-hex, out-of-u8, duplicate); the end-to-end PCI resolve -> codec.
- **dtb** (8): `compatible` NUL-split (most-specific first) + empty/unterminated/
  double-NUL tolerance; `reg` decode in 2/2 cells (the real pl061 + virtio bytes)
  and 1/1 cells + multi-entry + trailing-partial ignore; `interrupts` SPI -> +32
  and PPI -> +16 with unknown-type skip; `from_dtb` building the full pl061 node.
- **source** (22): `DeviceId` parse + `as_string` (typed `virtio:N` + the
  out-of-range / literal-compatible fallback); `matches_bind` (typed vs literal,
  and a raw `virtio,mmio` transport node never matching a `virtio:1` bind); the
  node-record round-trip (virtio + empty axes) + strict rejection (bad version,
  no id, duplicate key, bad number, unknown key) + the resource-count bound + the
  id-count bound (`MAX_IDS`) + the delimiter-in-label guard; `best_match`
  (most-specific id wins, typed binds, unbound id -> None); `reconcile_reported_node`
  (5d-4 trust boundary -- an honest node keeps its identity + takes the trusted
  reg/INTID; a fabricated address -> `None`; the security-proving case: a reported
  REAL slot with an inflated window + lying INTID is reconciled to the TRUSTED
  reg/INTID, so the source cannot inflate the conferred allowance); **the PCI
  identity + ctl-line parser** (6b-2): `VirtioPci` parse + `as_string` + the
  out-of-range fallback; `VirtioPci(1)` distinct from `Virtio(1)` and the two
  binds never collide; `parse_pci_ctl` happy path (bdf + INTID + no reg),
  `intid=none` -> no IRQ, unknown-appended-field tolerated, malformed rejected
  (wrong/absent version, missing component, garbled hex, no `=`, hex virtio,
  empty), `virtio=0` skipped; `best_match` binds a `VirtioPci` node to its
  `virtio-pci:1` manifest not the `virtio:1` one; `to_record` rejects a PCI node
  (the pipe codec cannot carry a bdf -- fail-closed, the forward seam).

The device layer (`driver`) is proven to compile by the whole-workspace device
build + `example::nop_entry`, which instantiates `run::<NopDriver>` so the entire
scaffold monomorphizes. The warden (5c) is the live consumer of
`dtb`/`resolve`/`to_allowance`/`to_descriptor`; `menagerie-probe` (5c) is the
first live `impl Driver` (the netdev retrofit at 5d is the first *useful* one).

---

## Error paths

`libdriver::Error` (flat, `Copy`, prints under `{:?}`):

| Variant | Source | Trigger |
|---|---|---|
| `Parse` | manifest | any malformed manifest |
| `NoMatch` | `resolve` | no node `compatible` is in `binds` |
| `TooManyWindows` / `TooManyIrqs` | `resolve` | node exceeds `MAX_MMIO` / `MAX_IRQ` |
| `BadVersion` | codec | descriptor version != `DESCRIPTOR_VERSION` |
| `BadField` | codec | unknown/duplicate key, missing `=`, missing required field, `;` in a value |
| `BadNumber` | codec | un-parseable hex/dec, window missing `:size` |
| `TooManyResources` | codec | descriptor names > `MAX_MMIO`/`MAX_IRQ` |
| `NoDescriptor` | `bind` | no argv[1] |
| `NoSuchResource` | helpers | asked for window/IRQ index N with fewer conferred, or DMA over `dma_max` |
| `Hardware` | helpers | a `libthyla_rs::hardware` create/map failed (typically the kernel I-34 gate or a VA collision) |

`run` logs the error via `eprintln!` and exits with the matching lifecycle code.

---

## Status / known caveats

- **The PCI axis is the wired-INTx path (6b-2 / 6b-3).** `PciNeed::Node` +
  `PciSource` resolve a virtio-PCI function to its bdf + its INTx-routed INTID
  (`node:interrupts` over the devpci-reported intid). The warden now enumerates
  `PciSource` in-process and binds `netdev-pci-driver` to `virtio-pci:1` narrowed
  to that bdf (step **6b-3**, the live I-34-on-PCI proof -- see
  [119-warden.md](119-warden.md)); this crate (6b-2) lands the mechanism + the host
  tests. MSI is still carried, not resolved (next bullet).
- **MSI is carried, not resolved.** `IrqNeed::Msi(n)` is parsed + round-tripped
  but yields no INTIDs from a DTB node; MSI vectors are the PCIe source's job on
  real hardware (MENAGERIE §12 -- the brcmstb host-bridge owns MSI). v1.0 on QEMU
  virt exercises the wired (`node:interrupts`) path, for both virtio-mmio (DTB) and
  virtio-PCI (devpci INTx).
- **Whole-node MMIO selection.** `MmioNeed::Node` grants every `reg` window;
  per-window selection (e.g. "only reg[1]") is a v1.x manifest refinement. The
  auditable property (grant <= node) holds either way.
- **`sig` is carried, not verified.** The section-9 authorization ladder
  (try-bind / ask-once / remember) is the warden's concern (step 5c+); libdriver
  round-trips the field only.
- **Live since 5c.** The warden (`usr/warden`, ref `119-warden.md`) consumes
  `dtb` + `resolve` + `to_allowance` + `to_descriptor`; `menagerie-probe`
  (`usr/menagerie-probe`) is the first live `impl Driver`. The whole loop --
  discover `/hw` -> match a manifest -> grant -> spawn narrowed -> the driver
  maps its granted MMIO + an out-of-grant create is rejected -- runs in joey's
  boot-probe ladder. The netdev retrofit (5d) is the first *useful* driver.

---

## Cross-references

- `docs/MENAGERIE.md` §4 (the hardware allowance / I-34), §5 (the warden), §6
  (the driver + manifest), §16 (the build sequence).
- `docs/reference/117-allowance.md` -- the kernel I-34 gate + the 5a
  confer-at-spawn ABI (`Command::allowance` / `TAllowanceDesc`) libdriver builds on.
- `docs/reference/116-devhw.md` -- the `/hw` DTB tree the warden reads
  `NodeResources` from (step 5c).
- `docs/reference/120-devpci.md` -- the kernel-mediated `/hw/pci` topology the
  `PciSource` reads `VirtioPci` nodes from (step 6b-2); the source <-> kernel `ctl`
  wire contract `parse_pci_ctl` consumes.
- `docs/reference/89-hardware.md` -- the `libthyla_rs::hardware` `Mmio`/`Irq`/`Dma`
  wrappers the helpers mint.
- ARCH §28 I-34 -- the driver-authority-bound invariant the grant model enforces.
