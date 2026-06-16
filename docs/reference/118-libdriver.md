# 118 - libdriver: the Menagerie driver framework crate

**Status**: as-built at Menagerie build-sequence step 5b (2026-06-16). Pure
userspace; no kernel ABI. The first consumer is the warden (step 5c); the first
real `impl Driver` is the netdev retrofit (step 5d).

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

- **The warden** (the device-manager TCB Proc, step 5c) reads a device node's
  resources from `/hw`, matches the node's `compatible` against the manifests in
  its bind database, calls `resolve` to compute the narrowed grant, derives both
  the kernel allowance (`to_allowance` -> `Command::allowance`) and the spawn
  descriptor (`to_descriptor` -> `Command::arg`) from that one grant, and spawns
  the driver.
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
| `driver` | `driver` (default) | `libthyla-rs` | the `Driver` trait, `run`, the handle-mint helpers, `to_allowance` |

`manifest` + `resource` carry no `libthyla-rs` dependency, so the grant logic +
codec run under `cargo test` on the host. `driver` is the only `libthyla-rs`
surface; it is compiled for `aarch64-unknown-none` only.

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
    pub serves: String,       // path template; "%instance" expands per bind
    pub restart: Restart,
    pub sig: Option<String>,  // section-9 authorization input; carried verbatim
}
pub struct Needs { pub mmio: MmioNeed, pub irq: IrqNeed, pub dma: DmaNeed }
pub enum MmioNeed { None, Node }                  // "node:reg"
pub enum IrqNeed  { None, Node, Msi(u32) }        // "node:interrupts" | "msi:N"
pub enum DmaNeed  { None, Pool(u64) }             // "pool: N" bytes
pub enum Restart  { Never, OnCrash, Always }

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
    serves   = "/dev/net/%instance"
    restart  = on-crash
    sig      = "<ed25519 over the package>"   # optional
}
```

Required keys: `abi`, `binds` (non-empty), `serves`. Optional: `needs` (defaults
to all-`None`), `restart` (defaults to `OnCrash`), `sig`. `#` starts a
line-comment. Sizes accept binary units (`2 MiB`, `512 KiB`, `1 GiB`, `64 B`, or
a bare byte count).

### `resource`

```rust
pub const DESCRIPTOR_VERSION: u32 = 1;
pub const MAX_MMIO: usize = 8;   // mirrors T_ALLOWANCE_MMIO_MAX
pub const MAX_IRQ:  usize = 8;   // mirrors T_ALLOWANCE_IRQ_MAX

pub struct NodeResources {       // what the warden reads from /hw
    pub compatible: Vec<String>, // most-specific first
    pub reg: Vec<(u64, u64)>,    // (base, size) MMIO windows
    pub interrupts: Vec<u32>,    // wired GIC INTIDs
}
pub struct BoundResources {      // the narrowed grant (the shared currency)
    pub instance: u32,
    pub compatible: String,      // the matched compatible
    pub serves: String,          // "%instance" expanded
    pub mmio: Vec<(u64, u64)>,   // <= MAX_MMIO, a subset of the node's reg
    pub irq: Vec<u32>,           // <= MAX_IRQ, a subset of the node's interrupts
    pub dma_max: u64,
}

pub fn resolve(manifest: &Manifest, node: &NodeResources, instance: u32)
    -> Result<BoundResources, Error>;
pub fn expand_instance(template: &str, instance: u32) -> String;

impl BoundResources {
    pub fn to_descriptor(&self) -> Result<String, Error>;          // -> Command::arg
    pub fn parse_descriptor(desc: &str) -> Result<BoundResources, Error>;
}
```

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

A manifest cannot widen its reach -- it can only decline an axis. The
host test `resolve_grant_never_exceeds_node` asserts every granted window/INTID is
a member of the node's set (the I-34 property in a test).

### The descriptor codec -- one argv slot

The grant crosses the spawn boundary as a single argv element (the warden's
`Command::arg`), because `SYS_SPAWN_ARGV_MAX` is 16 elements and a flag-per-window
encoding would exhaust it. The form:

```
v1;inst=0;compat=virtio,mmio;serve=/dev/net/0;mmio=a003000:1000,a004000:200;irq=2a;dma=200000
```

Fields are `;`-delimited `key=value`; numbers are hex except `inst` (decimal);
`mmio`/`irq` are comma-separated lists, omitted when empty; `dma` is always
present. `compat` is taken verbatim (its internal commas, e.g.
`raspberrypi,rp1-eth`, are not list separators -- only `mmio`/`irq` values are
comma-split). `parse_descriptor` is strict: an unknown/duplicate key, a bad
number, a window missing its `base:size` colon, an unknown version, or a count
over the caps all reject. `to_descriptor` rejects a `compat`/`serve` containing
the field delimiter `;` (fail-closed against a malformed node).

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
`set_dma_max`. Because both the allowance and the descriptor come from the same
`BoundResources`, the authority the kernel enforces and the resources the driver
maps cannot drift.

---

## Tests

19 host tests (`cargo test -p libdriver --no-default-features --target <host>`):

- **manifest** (7): parse the §6 example; `to_text` round-trip; wired-IRQ +
  no-DMA; defaults when optional fields absent; the size-unit parser
  (`MiB`/`KiB`/`GiB`/`B` + overflow + bad-unit rejection); malformed-manifest
  rejection (unterminated block, unquoted name, missing/duplicate/unknown key,
  empty binds, bad need value, bad restart, trailing garbage); comment +
  whitespace tolerance.
- **resource** (12): `resolve` for the MSI manifest (node MMIO granted, no DTB
  IRQ) and the wired manifest (node interrupts granted); the
  grant-never-exceeds-node property; `NoMatch`; `TooManyWindows`; the descriptor
  round-trip (full + empty axes); the exact descriptor shape; strict rejection
  (bad version, missing required field, duplicate/unknown key, bad number,
  missing window colon); the over-cap rejection; the delimiter-injection guard;
  the end-to-end resolve -> encode -> decode equality.

The device layer (`driver`) is proven to compile by the whole-workspace device
build + `example::nop_entry`, which instantiates `run::<NopDriver>` so the entire
scaffold monomorphizes. A live `impl Driver` over a real device lands at step 5d
(the netdev retrofit); the warden's use of `resolve`/`to_allowance`/`to_descriptor`
lands at step 5c.

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

- **MSI is carried, not resolved.** `IrqNeed::Msi(n)` is parsed + round-tripped
  but yields no INTIDs from a DTB node; MSI vectors are the PCIe source's job on
  real hardware (MENAGERIE §12 -- the brcmstb host-bridge owns MSI). v1.0 on QEMU
  virt exercises only the wired (`node:interrupts`) path.
- **Whole-node MMIO selection.** `MmioNeed::Node` grants every `reg` window;
  per-window selection (e.g. "only reg[1]") is a v1.x manifest refinement. The
  auditable property (grant <= node) holds either way.
- **`sig` is carried, not verified.** The section-9 authorization ladder
  (try-bind / ask-once / remember) is the warden's concern (step 5c+); libdriver
  round-trips the field only.
- **No native consumer yet.** libdriver ships as a library with no binary
  depending on it at 5b; the warden (5c) is its first consumer and the netdev
  retrofit (5d) its first `impl Driver`. It is compiled (whole-workspace device
  build + `nop_entry`) but not yet exercised on-device.

---

## Cross-references

- `docs/MENAGERIE.md` §4 (the hardware allowance / I-34), §5 (the warden), §6
  (the driver + manifest), §16 (the build sequence).
- `docs/reference/117-allowance.md` -- the kernel I-34 gate + the 5a
  confer-at-spawn ABI (`Command::allowance` / `TAllowanceDesc`) libdriver builds on.
- `docs/reference/116-devhw.md` -- the `/hw` DTB tree the warden reads
  `NodeResources` from (step 5c).
- `docs/reference/89-hardware.md` -- the `libthyla_rs::hardware` `Mmio`/`Irq`/`Dma`
  wrappers the helpers mint.
- ARCH §28 I-34 -- the driver-authority-bound invariant the grant model enforces.
