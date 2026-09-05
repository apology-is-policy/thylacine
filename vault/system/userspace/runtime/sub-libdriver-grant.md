---
id: sub-libdriver-grant
type: sub
title: "libdriver's grant core — one BoundResources, two consumers, and the kernel re-derives neither"
parent: moc-userspace-runtime
code:
  - usr/lib/libdriver/src/lib.rs
  - usr/lib/libdriver/src/manifest.rs
  - usr/lib/libdriver/src/resource.rs
  - usr/lib/libdriver/src/driver.rs
audit: hard
guarded-by: [inv-i34]
validated-by: [prose, gate-smp]
locks: []
hazards: []
abis: []
design: ["docs/MENAGERIE.md section 6"]
created: 2026-08-03
updated: 2026-08-03
area: userspace
---
## Purpose

The four files that decide what a driver may touch. `manifest` is the schema of
what a driver asks for; `resource` computes what it gets, and encodes it;
`driver` is the runtime a driver is written against, plus the one function that
turns a grant into authority the kernel will enforce.

**This is the only place in the tree where a device's physical resources become
one Proc's hardware reach.** It carries an `audit` level the rest of its area
does not, and the reason is in Invariants enforced.

## Contract

A grant is computed **once** and consumed **twice**.

```
resolve(manifest, node, instance) -> BoundResources
                                           |
                     +---------------------+---------------------+
                     |                                           |
              to_descriptor()                             to_allowance()
              (argv[1] -> the driver)            (Command::allowance -> kernel)
              "which resources to                "which resources you are
               create handles for"                permitted to create"
```

The descriptor **informs**; the allowance **authorizes**. A driver that
fabricated a physical address outside its allowance is refused by the kernel
gate, not by the codec — so the codec's strictness is defence in depth rather
than the boundary. The crate says this in its own header, and it is the sentence
the whole design rests on.

Externally: `Manifest::parse` is total (any input yields a `Manifest` or
`Error::Parse`, never a panic); `resolve` never returns resources the bound node
does not have; the codec round-trips exactly; and `run::<D>()` never returns.

## Mechanism

### The intersection: the manifest selects, the node supplies

Each axis of `needs` names a *selection*, and the concrete values come from the
device node:

| axis | the manifest says | the grant carries |
|---|---|---|
| mmio | `node:reg`, or nothing | every `reg` window the node exposes |
| irq | `node:interrupts`, `msi:N`, or nothing | the node's wired INTIDs (`msi` yields none against a DTB node) |
| dma | `pool: N`, or nothing | N — a budget, not a node resource |
| pci | `node`, or nothing | the node's own `(bus, dev, fn)` |

So a manifest cannot widen a driver's reach; it can only decline an axis. That
is the auditable property, and it is asserted **as a property** rather than as
behaviour: one host test iterates the resulting grant and checks membership of
every window and every INTID in the node it came from.

The ordering that decides the match is the **node's**, not the manifest's:
`resolve` walks the node's ids most-specific-first and takes the first one that
any `binds` entry claims. The manifest field's documented "most-specific first
by convention" is therefore inert here — a note for the human, since a DTB
`compatible` list is canonically ordered already. Which *manifest* wins for a
given node is a separate decision, made by the discovery layer.

### Every default is the least-authority one

An absent `needs` block yields `Needs::NONE` — no MMIO, no IRQ, no DMA, no PCI.
An absent `pci` key yields nothing even on a node that has a bdf. An absent
`lifecycle` yields `Transient`, so the warden tears the driver down rather than
leaving it resident. An absent `restart` yields `OnCrash` rather than `Always`.
Nothing in the schema defaults *toward* authority or persistence, which is what
makes a partially-written manifest safe rather than merely invalid.

### Gathering folds several nodes into one grant, and re-checks each

A `gather = all` manifest — the compositor, which owns both a GPU function and
an input function — resolves its first matched node normally, then folds each
further node's INTIDs, windows and bdf into the same grant under the same caps.
Every extra node is **re-matched against the manifest before anything of its is
folded in**: the caller gathered them, and the fold does not trust that. So the
per-axis property survives aggregation — every conferred value is still some
matched node's own.

### The two consumers deliberately disagree by exactly one page

`to_allowance` does not copy the grant's windows verbatim. It runs each through
`page_round`, because MMIO maps page-granular: the kernel checks the driver's
*page-sized* create against the allowance, and a sub-page device register — a
virtio-mmio slot is 0x200 bytes — is only reachable by mapping its whole page.
The descriptor keeps the exact sub-page window, so the driver still learns its
precise slot address; only the allowance is rounded out.

**This is the one sanctioned widening in the framework**, and it is also the
clearest proof of the point made under Invariants enforced: if the kernel
re-derived a driver's grant from the device tree, a page-rounded allowance would
be rejected as exceeding the node. It is not, so the kernel does not. The cost
is stated at the site — on the virt board two virtio-mmio slots share a page, so
a net driver's allowance also spans the adjacent block slot.

### The runtime is a three-step scaffold with distinct exit codes

`run::<D>()` does bind, then `probe`, then `serve`, and exits the whole Proc with
a code naming which step failed: 71 bind, 72 probe, 73 serve, 0 clean. The
supervisor reads that to decide whether a restart is sensible — a bind failure is
a warden bug, not a device that might come back.

MMIO and DMA maps draw their virtual addresses from `DriverVa`, a page-granular
bump allocator starting above the fixed addresses the older hand-written
transports hardcode. Its arithmetic saturates rather than wrapping, so a
pathological window size from a malformed descriptor produces an out-of-range
address that fails cleanly at the kernel, rather than a wrapped one that lands
somewhere real.

## Data structures

- **`Manifest`** — name, `abi`, `binds`, `Needs`, `serves`, `restart`,
  `lifecycle`, `gather`, and an optional `sig` carried verbatim.
- **`Needs`** — four `Copy` enums, one per axis. `Needs::NONE` is the default.
- **`NodeResources`** — what a node physically exposes: compatibles, `reg`
  windows, wired INTIDs, and an optional bdf.
- **`BoundResources`** — the grant. Instance, matched compatible, expanded
  service path, granted windows, granted INTIDs, DMA ceiling, primary bdf, and
  the gathered extra bdfs.
- **`DriverVa`** — a single `u64` bump pointer.
- **`Error`** — twelve flat `Copy` variants, so the pure layers need no
  libthyla-rs error type and the scaffold can print one under `{:?}`.

Caps: `MAX_MMIO`, `MAX_IRQ` and `MAX_PCI`, all 8, documented as mirroring the
kernel allowance's three arrays. See Caveats — nothing pins them.

## Concurrency

None. `manifest` and `resource` are pure functions over owned values: no shared
state, no interior mutability, no statics. `driver` adds a scaffold that runs
once, before any thread could exist — `run` is called from `main`, and the Proc
has no peer threads at bind time. The warden that calls the grant path is itself
single-threaded.

There is nothing here for two threads to reach, which is why this dossier has no
`locks` and no wait/wake section despite sitting under an invariant whose kernel
half is full of both.

## Invariants enforced

**[[inv-i34]] — but precisely half of it, and the half the kernel cannot do.**

The invariant carries two obligations, discharged in different places:

- *A driver's allowance is never wider than what it was granted.* **Kernel.**
  A conferred set is checked against the conferrer's own, and every hardware
  create is gated against the Proc's allowance.
- *A driver's allowance corresponds to the device it was bound to.* **Here.**
  `resolve` is the only computation that establishes it.

The second is not redundant with the first, and the reason is worth stating
plainly: **the warden's own allowance is BROAD** — a null allowance, for which
the kernel's permit check returns true unconditionally — so the parent-scope
check is *vacuous for exactly the Proc that computes grants*. The kernel
faithfully enforces the allowance it is handed and never asks whether that
allowance describes the driver's own device. It cannot: it does not know which
node the bind chose.

What still bounds the warden is the I-5 reservation floor — a conferred window
over kernel-owned MMIO is refused — but at the driver's create, one hop later,
by a different check, against a different question.

So a defect in `resolve` does not stop at its caller. That is the qualification
this area's organizing fact needs, and the reason for the `audit: hard`.

## Error paths

Every failure is a value; nothing panics and nothing aborts.

`Manifest::parse` rejects an unexpected byte, a string unterminated at a newline
or at EOF, non-UTF-8 inside a string or identifier, a missing or duplicate or
unknown key, an empty `binds` list, a bad enum word, a bad need value, a size
with no digits or an unknown unit or an overflowing product, and trailing
garbage after the closing brace. All of it is one `Error::Parse`, and the
warden's disposition for it is "this driver is not bindable" — never a crash,
never a partial manifest.

`resolve` yields `NoMatch`, `TooManyWindows` or `TooManyIrqs`.

The codec is strict in both directions. Encoding refuses a `compatible` or
`serves` containing the field delimiter, so a malformed node cannot corrupt the
descriptor's framing. Decoding refuses an unknown version, a field without `=`,
an unknown key, a duplicate key, a window missing its `base:size` colon, a
non-hex number, a bdf that is not exactly three components or whose component
overflows a byte, and any axis over its cap. An *absent* optional axis is not an
error — absent and malformed are distinguished, which is what lets the encoder
omit empty axes without the decoder having to guess.

## Performance

Not a measured surface, and it runs once per driver spawn. `resolve` is
node-ids × manifest-binds with both bounded in single digits; the codec is one
pass over a string bounded by the caps at roughly half a kilobyte, against a
64 KiB argv budget — so the descriptor is nowhere near the spawn bound and the
question of truncation does not arise.

## Prosecution

- **Every axis must stay node-supplied.** A new axis whose concrete values come
  from the manifest rather than the bound node breaks the auditable property at
  its root, and nothing downstream would catch it — the kernel enforces the
  allowance it is given.
- **The gathered fold must keep re-matching each extra node.** It is the only
  thing between a mis-gathered node list and a grant carrying an unrelated
  device's resources.
- **`page_round` is the only sanctioned widening, and it must stay a page.**
  Widening it further silently enlarges every driver's reach, and there is no
  check anywhere that would notice.
- **The three caps must stay equal to the kernel's**, and today nothing makes
  them (Caveats).
- **A new `Driver` implementor must mint handles only from its grant.** The
  helpers take an index into `BoundResources` for exactly this reason; a driver
  passing a literal address would be refused, but at the kernel, with a
  diagnostic naming the request rather than the grant.
- **`to_allowance` must stay the warden's alone.** A driver calling it is
  harmless today because it has no broad allowance to narrow — a property worth
  keeping true rather than rediscovering.
- **If the manifest database ever moves off the binary, the parser's robustness
  stops being insurance and becomes the boundary.** Today the database is five
  string constants compiled into the warden, so no foreign manifest exists and
  the fail-closed parsing has never been load-bearing. The signed-package seam
  is exactly the change that makes `name`, `serves` and `binds` unbounded
  external input — at which point their absence of length limits, harmless
  against compiled-in text, becomes a real question against a 64 KiB argv blob.

## Seams

- **Package authorization is unbuilt.** `sig` is parsed, round-tripped, and
  verified by nothing; the crate says so, and the warden never reads the field.
  The manifest database's trust rests entirely on being compiled in.
- **MSI vectors resolve nowhere yet.** `IrqNeed::Msi(n)` grants no INTIDs
  against a DTB node; it is carried for the real-hardware path.
- **MMIO selection is all-or-nothing.** `node:reg` grants every window the node
  has; per-window selection is a refinement, and the auditable property holds
  either way.
- **Sub-page MMIO separation.** The page-rounding over-grant above; separating
  two devices that share a page needs a mechanism that does not exist.

## Caveats

- **`abi` is required, exported, and compared to nothing.** The constant has
  three references in the tree — its definition, its own doc comment, and a
  re-export — and that doc says the warden "refuses to bind a manifest whose
  `abi` it does not implement". It does not: `abi = 999` binds exactly like
  `abi = 1`. The contrast is what makes it sharp — the same crate's *other*
  version field, the descriptor's, **is** checked, at the receiving end, fail
  closed, with its own test. So a framework skew is not undetected; it is
  detected one layer later by the codec, with a diagnostic naming the codec
  rather than the manifest nobody checked. Unreachable while the database is
  compiled in, live at the signed-package seam. Task #134.

- **The three allowance caps mirror the kernel's by prose alone, and the code
  depending on that discards its own errors.** The kernel constants appear in
  this crate only inside doc comments, and there is no compile-time assert
  anywhere in it. Meanwhile `to_allowance` drops the result of every push, on
  the stated grounds that the capacities match. They do, today, by numeric
  coincidence. If they diverged the failure would be quiet *and* misattributed:
  the grant truncates on the way out, and the driver's later map fails with an
  error whose own documentation blames the driver's request. Fail-closed in
  direction — a dropped entry is less authority, never more — so this is a drift
  and diagnostic hazard rather than a privilege one. Task #135.

- **The page-rounded allowance is wider than the node's own windows**, by
  design, and on the virt board that means a driver's allowance can span an
  adjacent device's registers. Documented at the site and reasoned; recorded
  here because it is the one place the "never exceeds the node" property is
  deliberately relaxed, and a reader who takes that property literally will
  otherwise be surprised by it.

- **The runtime layer has no tests, and that follows the crate's stated split.**
  36 host tests cover these files — 11 on the manifest parser, 25 on the grant
  and codec — and all 36 sit in the two pure modules. `driver.rs` has none,
  because it is the libthyla-rs layer and cannot run on the host. The
  consequence is worth naming: the function that converts a grant into
  kernel-enforced authority, and the three helpers that mint hardware handles,
  are covered by a booted VM or not at all.

## Provenance

[[chg-2026-08-03-libdriver-grant-sweep]].
