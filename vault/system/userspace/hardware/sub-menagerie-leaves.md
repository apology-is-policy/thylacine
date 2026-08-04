---
id: sub-menagerie-leaves
type: sub
title: "The Menagerie leaves — the two programs the warden hands a device to"
parent: moc-userspace-hardware
code:
  - usr/virtio-mmio-source/src/main.rs
  - usr/virtio-mmio-source/Cargo.toml
  - usr/netdev-driver/src/main.rs
  - usr/netdev-driver/Cargo.toml
audit: light
guarded-by: [inv-i34]
validated-by: [prose, gate-smp]
locks: []
hazards: []
abis: []
design: []
created: 2026-08-04
updated: 2026-08-04
---
## Purpose

The far end of the bind path: two small programs the warden spawns under a
narrowed hardware allowance, each to do one job the broker will not do itself.
`virtio-mmio-source` reads device identity registers so the trusted component
never touches one; `netdev-driver` holds a network device through a full
lifecycle so the lifecycle can be observed. Between them, 414 lines.

They are the only programs in the tree that are *granted* hardware rather than
taking it. Everything in [[sub-virtio-probes]] hardcodes a physical address and
claims it under broad authority; these two are told what they may touch and can
touch nothing else. Reading them against that set is the clearest available
demonstration of what [[inv-i34]] changed, because the code is otherwise doing
the same kind of work.

They are also the reason the framework has consumers at all. [[sub-libdriver-grant]]
computes a grant and [[sub-libdriver-discovery]] decides what a device is; those
two dossiers describe machinery, and these are the two programs the machinery
runs.

## Contract

Both take a single argument — the spawn descriptor the warden encodes, which
carries the resources the grant covers — and both decode it through the same
library entry point rather than parsing it themselves. Neither has stdin or a
terminal.

Where they differ is what "done" means, and that difference is the whole of
their shapes:

| | `virtio-mmio-source` | `netdev-driver` |
|---|---|---|
| Job | enumerate a bank, report what is in it | claim one device, stay up |
| Output channel | newline-delimited records on stdout, a pipe the warden reads | a readiness line, then silence |
| Termination | exits when the bank is scanned | never; the warden terminates it |
| Grant | the whole bank window, no interrupt | one slot, its interrupt, a DMA ceiling |

The source's contract has a second half that is easy to miss: it must **release
the bank before the warden binds anything**, because memory claims are exclusive
and a driver's slot lives inside the window the source held. The warden's
sequencing makes this automatic — it reads the pipe to end-of-file, which only
happens when the source exits, and only then binds — but the source also drops
the mapping explicitly before its final log line.

## Mechanism

**The source** maps its granted bank, walks the transport nodes its own view of
the hardware tree reports, reads two registers per populated slot, and emits one
typed record per slot that answered. The record carries the identity it just
learned plus the slot's address and interrupt as the source understands them.

That last part is what makes it interesting rather than routine. The source is
*not trusted*, and the warden does not believe the resources it reports: it
matches each record to a slot in its own trusted view by address and rebuilds
the resources from there, keeping only the identity. So the source's authority
over the outcome is exactly one field — which device a real, kernel-described
slot is declared to be — and everything else is re-derived by the party that
does the granting. [[sub-libdriver-discovery]] describes the reconciliation;
this is the program it was written against.

**The driver** binds its grant, opens the network device through
[[sub-netdev]]'s MMIO transport, resets the device, announces readiness, and
then blocks on its interrupt handle forever. The blocking is the point: it is
alive so that something can take the device away from it, and the observable
proof is the warden's log recording the bind, the readiness, and then the
teardown with an exit status.

## Data structures

Neither owns one. The source builds library-defined node records and hands them
to the library encoder; the driver holds a transport handle from [[sub-netdev]]
and a grant record from [[sub-libdriver-grant]]. Both are thin by design — the
structures live where they can be shared, and these are the consumers.

## Concurrency

Single-threaded, no shared state, no locks. The only ordering that matters is
across processes, and it belongs to the warden rather than to either program:
the source's claim must be gone before a driver's claim is attempted, and that
holds because the warden reads to end-of-file before it reaps and binds.

The driver's teardown is the other cross-process ordering, and it is the
subject of the second caveat.

## Invariants enforced

Neither enforces anything; both are *bounded by* [[inv-i34]], which is a
different relationship and the reason this dossier names it as a guard rather
than a subject. The kernel checks each hardware request against the allowance
the warden conferred, so a program here that asked for a window outside its
grant would be refused. Neither asks.

Two properties are worth recording as things a reader might expect to find
enforced here and will not:

- **The correspondence between a grant and a device** is not checked by anyone
  at runtime. The warden computes it; the kernel enforces whatever it is handed.
  [[sub-libdriver-grant]] records why.
- **The source's honesty** is not checked either, and is not meant to be. The
  containment is structural: it can misname a device, and it cannot fabricate a
  resource, because the resources are rebuilt from a trusted view.

The source does carry one bound of its own, and it is the model of the shape:
before reading a slot it checks that the slot's **entire read extent** lies
inside the granted window, not merely its base address. The comment says the
over-run case is unreachable given page-rounded banks and the slot stride, and
gates it anyway. That is the discipline stated correctly — a bound on the
access rather than on the address — and it is worth not regressing.

## Error paths

Both fail closed and both are non-fatal to the boot.

The source distinguishes a bind failure from a probe failure by exit code, logs
which of the two happened, and on a mapping refusal names the window it was
denied so the diagnosis points at the allowance. If it produces no records the
warden proceeds with whatever else it discovered — a source failure costs the
typed devices, not the boot.

The driver's failure modes are the library's: a grant it cannot decode, a device
that does not match what it expected, a transport that will not initialize. All
of them exit before the readiness line, which is exactly the signal the warden's
supervision keys on.

## Performance

Irrelevant to both. The source reads sixty-four registers once at boot; the
driver blocks.

## Prosecution

- **Can the source's claim outlive its usefulness?** No, but for a reason that
  is not in the source: the warden cannot bind until it has read to end-of-file,
  which requires the exit. The explicit release inside the source is good
  practice with an overstated justification.
- **Can the source reach outside its grant?** Not through the code as written —
  the per-slot extent check gates every read, and the allowance gates the
  mapping. But see the first caveat for what the grant itself permits.
- **Does the driver's teardown leave the device running?** This is the sharp
  question for the pair, and the answer is a chain rather than a check: see
  [[sub-netdev]]'s teardown caveats, and the second caveat below.
- **Does either program's failure widen anything?** No. Both fail before
  acquiring, or exit; the kernel releases claims at process teardown regardless.

## Seams

- **The source is the only non-trusted reporter in the system**, and the whole
  reconciliation machinery in [[sub-libdriver-discovery]] exists for it. A second
  bus source — the design anticipates one for a different bus — would inherit
  that machinery, and the note there records that a stricter path is wanted for
  a bus whose slots are not addressable the same way.
- **The driver is a lifecycle proof that a real driver will replace.** Its own
  header names becoming persistent as the next step, which is the subject of the
  second caveat.

## Caveats

- **The source maps the whole bank writable and performs only reads** (task
  #145). It asks for read, write and map rights and maps read-write, then issues
  two loads per slot and no stores; its own comment notes that the accessors only
  read. Read-only mapping is supported at both the library and the kernel, which
  validates the requested protection against the handle's rights explicitly. The
  gap matters more here than it would elsewhere because this program exists *to
  be* the least-privileged component — the broker spawns it so the trusted side
  never touches a device register — and the bank it holds spans every virtio
  slot on the machine, including the block transports the filesystem server
  claims later. Not an exploit: it consumes almost nothing hostile. But the
  distance between what it holds and what it uses is the ability to write any
  virtio register on the machine, for a program that writes none.
- **The driver's transient lifecycle is load-bearing and undocumented as such**
  (task #142). Memory windows map page-granular, so its grant is page-rounded and
  it claims the whole page containing its slot — which on the reference machine
  also contains two block-device slots that the filesystem server probes for its
  disk. Claims are exclusive. It works only because the driver's manifest omits
  the persistence marker, so the broker tears it down before the pivot. Making it
  persistent, which its header names as the next step, would hold that page past
  the pivot. Two documents mention halves of this and neither connects them.
- **The obligation to stop a device before teardown is stated on the wrong
  transport, and this driver satisfies it uselessly.** [[sub-netdev]] records the
  full shape; what belongs here is that this driver quiesces the device and
  *then* signals readiness and blocks — correct for a lifecycle proof whose
  expected end is a forced teardown, and uncopyable by a driver carrying traffic,
  which must keep the device live and receives no removal notice. The one
  compliant caller complies at the one moment compliance costs nothing.
- **Neither program is exercised by a test.** Both are proven only by the boot
  log — the source by the typed-node count the warden reports, the driver by the
  bind-ready-teardown sequence. That is real evidence and it runs every boot, but
  it means neither has a failure-path regression: nothing checks that a bad grant
  is refused, only that a good one is accepted.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
