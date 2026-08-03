---
id: sub-libdriver-discovery
type: sub
title: "libdriver's discovery and lifecycle — identity up, resources down, registers last"
parent: moc-userspace-runtime
code:
  - usr/lib/libdriver/src/source.rs
  - usr/lib/libdriver/src/dtb.rs
  - usr/lib/libdriver/src/supervise.rs
  - usr/lib/libdriver/src/readyline.rs
audit: hard
guarded-by: [inv-i34]
validated-by: [prose, gate-smp]
locks: []
hazards: []
abis: []
design:
  - "docs/MENAGERIE.md section 3 (bind on identity, not transport)"
  - "docs/MENAGERIE.md section 5 (the warden's supervision loop)"
  - "docs/MENAGERIE.md section 7 (the discovery sources)"
created: 2026-08-03
updated: 2026-08-03
---
## Purpose

The other half of the driver framework: where a device node **comes from**, how a
freshly-spawned driver says it is **up**, and what happens when one **dies**.

[[sub-libdriver-grant]] computes what a driver may touch. This half decides
*which device it is talking about* — and that question is the more dangerous one,
because the grant is only as sound as the node it intersects with.

## The organizing fact

**Identity flows up from a source that may be lying; resources flow down from a
view that cannot be; and the driver believes neither until it reads the
register.**

Three layers, each trusting its predecessor less than it could:

| Layer | Supplies | Trusted for |
|---|---|---|
| a bus source | the device's *identity* | identifying a slot — never describing one |
| the warden | the slot's *resources* | everything (it holds the DTB view) |
| the driver | nothing | it re-reads the device registers |

That middle row is the design's whole security posture, and it is stated in the
scripture as a rule rather than left implicit: *the warden binds on the identity,
not the transport, and never reads a device register.* A bus whose device type is
only knowable by poking hardware — a virtio-mmio slot's DeviceID, a PCIe
function's class — is enumerated by **its own source**, which claims the raw
transport nodes and re-emits **typed** children the warden binds by id.

So the warden never touches a device register, and the source that does is
sandboxed and disbelieved.

## Contract

Four modules, all **pure** (no libthyla-rs) except two feature-gated concrete
sources at the bottom of `source`. The purity is what puts the fiddly parts —
big-endian cell decoding, a state machine's cross-product, a trust boundary's
parser — on the host where they can be tested exhaustively.

- **`source`** — the `DeviceId` / `DeviceNode` types, the node-record codec that
  carries a node from an out-of-process source to the warden, the bind matcher,
  and the reconciliation that bounds an untrusted source. Plus `DtbSource` and
  `PciSource`, the two in-process concrete sources.
- **`dtb`** — decoding raw FDT property bytes (`compatible`, `reg`, `interrupts`)
  into typed resources.
- **`readyline`** — accumulating a driver's one-line readiness signal from
  whatever chunks arrive.
- **`supervise`** — the restart decision: given one run's outcome, a policy, and
  the restarts already spent, restart with back-off or settle.

## Mechanism

### A device's identity is a type, not a string

`DeviceId` has three variants: a DTB `compatible` string, a virtio device-id
behind an MMIO transport, and a virtio device-id behind a **PCI function**. The
last two are deliberately distinct despite naming the same device number, because
the two transports have different claim paths — a virtio-PCI driver claims its
function and maps BARs, a virtio-MMIO driver mints a handle over its allowance
window — so a manifest binds exactly one and the two can never collide.

Parsing is **fail-closed forward-compatible**: an unrecognized `bus:`-style
prefix stays a literal `Compatible`, which then simply never matches a typed
node. An old build does not choke on an identity from a bus it has never heard
of; it declines it. The typed namespace cannot collide with a DTB compatible
because a DTB compatible never contains a colon.

### The bind: most-specific id wins over database order

A node carries an **ordered** id list, most-specific first (the FDT's own
convention for `compatible`). `best_match` scans every manifest, finds the
earliest node-id position each one binds, and keeps the manifest whose match sits
earliest. So a node listing `["arm,pl061", "arm,primecell"]` binds the pl061
driver even if the primecell driver is listed first in the database.

That ordering is also **the same one `resolve` uses**, which is what keeps the
two halves consistent: `resolve` re-finds the first node id its manifest binds
and records it as the grant's identity. Both walk the node's ids with the same
predicate, so the identity named in the grant is exactly the identity that won
the bind. There is no path where the warden binds on one id and grants under
another.

### The two source flavours, and why only one needs vetting

- **In-process and trusted.** `DtbSource` reads the DTB tree the kernel
  publishes; `PciSource` reads the kernel's mediated PCI topology. Both are the
  *kernel's own* view, read directly, so there is no non-TCB reporter to vet —
  which is why the PCI path has no reconciliation step and says so.
- **Out-of-process and untrusted.** A bus source that must poke hardware to
  identify a slot runs as a **separate sandboxed Proc** and reports back over a
  pipe. This is live, not hypothetical: the virtio-mmio bus source claims the
  bank, reads each slot's DeviceID, and pipes typed records to the warden.

### The reconciliation — what an untrusted source can and cannot do

`reconcile_reported_node` matches a reported node to a trusted slot **by its
first register base**, then rebuilds the node's resources — windows, interrupts,
bus function — from that trusted slot, keeping only the reported *identity*.

| A hostile source tries to | Outcome |
|---|---|
| name an address it was never granted | rejected outright (no trusted slot matches) |
| inflate a real slot's window | discarded; the trusted size is used |
| claim a different interrupt | discarded; the trusted INTID is used |
| smuggle a bus function | the record cannot even represent one (below) |
| **mis-identify a real slot** | **permitted** |

Only the last one gets through, and it is deliberate — identifying is the one
job the source has. The residual is that the *wrong driver binds to a real
device*. That is contained one layer further down: the driver re-reads the
device's magic value, device-id and version register before driving it, with the
principle stated exactly right in its own comment — **the grant is information;
the device registers are ground truth**. A mis-identified bind therefore fails
closed at the driver, which converts an authority question into an availability
one: the wrong driver refuses, and the right one was never bound.

### The record codec splits its checks on a principle

The node record is a single line of `key=value` fields. The encoder and the
decoder check *different* things, and the split is principled rather than
arbitrary:

- **The encoder rejects what would produce a valid-but-wrong record.** A
  delimiter inside a label or an id would silently re-frame the line into a
  different, well-formed record — so those are caught at encode. So is a node
  carrying a **bus function**, which the format cannot represent at all: rather
  than silently drop it (degrading downstream to a grant with no PCI axis), the
  encoder **fails loudly**, and the comment says a future out-of-process PCIe
  source must extend the format first.
- **The decoder rejects what can be cleanly refused.** Unknown version, unknown
  key, duplicate key, empty id list, malformed number, and every count over its
  allowance cap. A hostile source cannot make the warden allocate without bound
  or mis-grant; it gets a rejected record and a logged skip.

The decoder is strict in exactly the way the spawn descriptor's is — same crate,
same trust-boundary reasoning, and the encoder says so explicitly. The one
codec that is deliberately **lenient** is the PCI topology parser, which
tolerates unknown appended fields because its reporter is the kernel; it carries
an explicit contract naming the in-process caller it is valid for, and a note
that a future untrusted PCIe source must use the strict path instead.

### Readiness: a bound on the wrong loop

A driver signals it is up by writing one newline-terminated line to its stdout
pipe. The accumulator takes whatever chunk arrives, scans for the newline, and
returns the line, a request for more, or **garbled** — the last on either
overflowing the line cap or failing UTF-8 validation.

The history is the interesting part, and it is a bound-composition failure worth
carrying. The original read the pipe **one byte at a time with blocking reads**,
looping until the newline. The give-up budget lived in the warden's *outer poll
loop* — so a driver that wrote a partial line and then simply held, alive with
its write end open and nothing more to say, stalled the warden **forever** on the
next byte's blocking read, escaping the budget entirely. A hang on the warden is
a boot denial-of-service by a misbehaving driver, which is precisely the threat
the framework exists to contain.

The fix moves the blocking out of the loop the budget does not cover: one bounded
read returning whatever is *available*, fed into an accumulator that persists
across poll iterations. A line split across reads still assembles; nothing ever
blocks mid-line. The consumer honours this exactly — it reads into a buffer
sized to the cap and treats garbled as give-up.

### Supervision: soft failure and hard failure are different failures

The restart decision is a small state machine over one run's outcome:

| Outcome | Disposition |
|---|---|
| served, then deliberately removed | up — never a restart candidate |
| exited cleanly | up |
| exited crashed, restarts remain and the policy allows | restart, after back-off |
| exited crashed, restarts exhausted or policy forbids | **gave up** (soft) |
| the warden could not spawn or track it | **failed** (hard) |

**The soft/hard split is the load-bearing distinction.** A driver that crashes
and exhausts its restarts leaves *its device* unavailable while the system is
fine — that must not fail the boot. Only a structural failure, where the warden
could not even spawn the binary, signals a misconfiguration worth failing on.
The warden's exit code keys on the hard count alone.

Back-off doubles from a small base and clamps at a cap, so a crash-loop converges
quickly to "device unavailable" rather than wedging the boot ladder. It is
overflow-safe three ways over: the shift is capped well below the word width, the
multiply saturates, and the clamp dominates long before either matters.

A driver killed with no exit code — a hung one the warden had to terminate —
counts as a **crash**, so it is restarted under the on-crash policy rather than
being mistaken for a clean one-shot.

## Data structures

`DeviceNode` is a label, an ordered id list, and the node's resources. The label
is **provenance only** and is never matched on — a fact worth keeping, since it
is the one field an untrusted source controls that reaches the warden's logs
verbatim.

The wire record is one line: a version token, then `label`, `id`, `reg` and
`intid` fields, numbers bare lowercase hex. Both list caps — identities, and the
resource counts inherited from the allowance — bound the decoder's allocation.

The three lifecycle enums are all `Copy` and flat: the readiness result, the run
outcome, and the terminal disposition. Nothing here allocates except the
assembled line and the decoded node.

## Concurrency

**None, and that is a property rather than an omission.** Every function here is
a pure transformation over its arguments: no shared state, no interior
mutability, no statics. The two concrete sources hold only a root path.

This is what lets the entire discovery and lifecycle policy be exercised on the
host — 50 tests across the four files — while the parts that genuinely cannot be
(the spawn, the poll, the reap, the sleep) stay in the warden and stay small.
A change that introduces shared state here forfeits that.

## Invariants enforced

**[[inv-i34]]** — this half establishes the *correspondence* the grant then
carries. [[sub-libdriver-grant]] records why the kernel cannot re-derive that
correspondence; this note records where it is actually created. The two
mechanisms that create it are `best_match` (which device this manifest is for)
and `reconcile_reported_node` (which resources that device really has), and the
second is a genuine enforcement against a non-TCB reporter, not a convenience.

Composes **[[inv-i5]]**: the untrusted bus source only ever holds the bank
allowance it was granted, and the reconciliation bounds it to the slots inside
that domain. It composes with, rather than duplicating, the kernel's own gate —
the kernel bounds what a handle may cover, this bounds what the warden will ever
ask for.

## Error paths

Every failure is a typed variant of the framework's flat error enum, or a plain
`None`. Nothing panics on any input, including hostile input: the parsers use
only fallible conversions and there is no `unsafe` in this half at all.

The dispositions are deliberately different by layer. A malformed **record** is
rejected and the node skipped — one bad node never sinks a discovery pass. A
malformed **property** decodes best-effort: a trailing partial entry is ignored,
an unknown interrupt type is skipped, an unparseable compatible segment is
dropped. A source or a directory that cannot be read yields **no nodes** rather
than an error, so a missing tree degrades to "nothing discovered here".

## Performance

Discovery runs once at boot over a handful of nodes; nothing here is hot. The
bind matcher allocates a string per identity comparison, which is invisible at
five manifests and would not be at five hundred.

The back-off is the only place time is deliberately spent, and it is bounded per
attempt and in total.

## Prosecution

- **The warden must never read a device register.** The moment it does, the
  sandboxed-source architecture has no purpose. A bus that needs poking gets its
  own source Proc.
- **A source is trusted to identify, never to describe.** Any new out-of-process
  source's resources go through the reconciliation against the warden's own
  view. Widening what a source may assert is the way this design fails.
- **The reconciliation's containment depends on a driver-side re-validation
  that lives in another crate.** It is present and correct today. A new driver
  that trusts its grant's identity without re-reading the device weakens a
  security argument made here, and nothing in this crate can detect that.
- **`best_match` and `resolve` must keep walking the node's ids with the same
  predicate**, or the warden binds on one identity and grants under another.
- **The encoder keeps rejecting what it cannot represent, loudly.** Silently
  dropping an axis degrades to a narrower grant, which looks like success. The
  bus-function rejection is the worked example.
- **The strict decoder stays strict.** The leniency of the topology parser is
  sound *only* because its reporter is the kernel; an untrusted reporter must
  use the counted, bounded path.
- **Readiness reading stays non-blocking.** Any blocking read on a driver's pipe
  re-opens the boot denial-of-service, and a give-up budget on an enclosing loop
  will not cover it.
- **Soft and hard failure stay distinct.** Collapsing "this device's driver
  crashed" into "the boot failed" makes one flaky device fatal.

## Seams

- **Nested buses.** Both concrete sources enumerate the top level only, which
  covers every bindable device on the current targets. Descending bridges is a
  refinement.
- **An out-of-process PCIe source.** The record format cannot carry a bus
  function, and the encoder enforces that rather than pretending otherwise. This
  is the one seam the code actively guards.
- **MSI.** Message-signalled vectors are a PCIe-source concern; the grant
  carries the count from the manifest and nothing intersects it here.
- **Finer restart policy.** The kernel collapses every non-clean exit to a
  single status, so the supervisor can distinguish crashed from clean but not
  *which* failure. The pure state machine already accepts arbitrary codes, so
  when structured exit status arrives only the warden's mapping changes — the
  seam is pre-fitted.

## Caveats

- **`best_match`'s specificity logic is untested.** Its entire reason to be more
  than a linear scan is that the most-specific node id wins over database order —
  and all three of its tests are satisfied by an implementation that returns the
  first manifest matching *any* id. Two use single-id nodes, so the ordering
  never engages; the third, named for the property, arranges the database so that
  order and specificity point the same way. Reachable when two manifests bind
  different compatibles of one node, which DTB nodes routinely carry; not live
  today at five compiled-in manifests. A tie between two manifests at the same
  position resolves to database order, which is sensible and undocumented. Task
  #138.
- **The crate's front door describes an earlier crate.** The header presents a
  two-part split — one pure module pair, one libthyla-rs layer — for a crate that
  has six modules, and its claim that `driver` is "the only libthyla-rs layer" is
  false: `source` carries two concrete sources behind the same feature flag. Every
  *module* header is current and cross-references its siblings correctly; only
  the crate header is frozen. Task #139.
- **The FDT cell widths are hardcoded rather than read.** Address, size and
  interrupt cell counts are passed as constants matching the current targets,
  while the DTB publishes its own values in files the source explicitly steps
  past. This is **not** a divergence: the kernel's own decoder hardcodes the same
  convention in the same words, so it is one documented platform assumption held
  in two places rather than two answers to one question. A target whose root
  disagreed would mis-decode every window — and the safety argument offered
  ("the grant is never wider than the node") would still hold while meaning less,
  since the node itself would be wrong.
- **An unknown interrupt type is skipped silently**, which shifts the positions
  of everything after it in the granted list. Justified today because the nodes
  that carry mixed interrupt forms expose no registers and so bind nothing — a
  property of the current platforms rather than of the decoder.
- **The label is attacker-controlled and reaches the logs verbatim.** It is never
  matched on and never reaches a grant, so this is a diagnostic-integrity
  question, not an authority one — but a source that names itself something
  misleading will be believed by a reader of the boot log.

## Provenance

[[chg-2026-08-03-libdriver-discovery-sweep]].
