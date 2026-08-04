---
id: sub-warden
type: sub
title: "The warden — the hardware broker, and the one grant nothing re-derives"
parent: moc-userspace-boot-chain
code:
  - usr/warden/src/main.rs
  - usr/warden/Cargo.toml
audit: hard
guarded-by: [inv-i34]
validated-by: [prose, gate-smp]
locks: []
hazards: []
abis: []
design:
  - docs/MENAGERIE.md sections 3-6
created: 2026-08-04
updated: 2026-08-04
---
## Purpose

One program, spawned by init before the root pivot, that turns a machine's
device list into a set of capability-sandboxed driver processes. It
enumerates what is present, matches each device against a compiled-in
database of driver manifests, works out which slice of the machine each
matched driver may touch, spawns it holding exactly that slice, waits to see
whether it comes up, and restarts it a bounded number of times if it does
not. Then it exits, and the drivers it left running outlive it.

It is the only place the two halves of the driver framework meet: one half
says what is out there ([[sub-libdriver-discovery]]), the other says what a
manifest is allowed to have ([[sub-libdriver-grant]]), and this program
intersects them.

## Contract

Its exit code is the boot's verdict on hardware, and it distinguishes two
kinds of failure that most systems conflate. **A device that came up wrong is
not the same as a broker that could not do its job.** A driver that crashes
and exhausts its restarts leaves *its own device* unavailable while the rest
of the system is fine, so it does not fail the boot. Only a structural
failure — a missing binary, an unspawnable child, a driver that cannot be
observed at all — is fatal, and init treats a non-zero exit as a boot
failure.

To the drivers it spawns, its contract is: **you are given a device and told
what you were given, and those two are the same thing.** The authority (the
kernel-enforced allowance) and the information (an argument string describing
the grant) are computed from one value, so they cannot disagree.

To the machine, its contract is a prohibition: **it never reads a device
register.** A bus whose device types are only knowable by asking the hardware
is enumerated by a separate sandboxed process, not by the broker itself.

## Mechanism

Four phases, in order, and the order is load-bearing in one place.

**Discover.** The static fabric comes from the kernel's device-tree view.
Raw slots of the dynamically-typed bus are pulled *out* of the bind set and
handed to a helper: the broker computes the address window spanning all of
them, spawns a sandboxed process granted only that window, and reads back
typed device records over a pipe. Those records supply identity only — every
resource is rebuilt from the broker's own trusted view, so a compromised
helper can mis-identify a slot but cannot invent an address. The bus fabric
comes from the kernel's mediated topology view and needs no such helper,
because there is no untrusted reporter in that path: the kernel's own
enumeration is the trusted view. Finally one synthetic device is appended,
backed by no hardware, existing purely to exercise the restart ladder.

**Bind.** Each device gets at most one manifest, the most specific match.
Manifests marked *gather* do not bind per device; their matches are collected
and folded into a single grant at the end, so the compositor gets one process
holding all of its several devices rather than several processes each holding
one.

**Grant.** The manifest's declared needs are intersected with the device's
actual resources. This is the step nothing else re-derives — see *Invariants
enforced*.

**Supervise.** Spawn, wait for the driver to declare itself, and decide.

## Data structures

There are almost none, and that is worth stating: the broker holds a parsed
manifest list, a flat list of discovered devices, a per-manifest instance
counter, a per-manifest gather bucket, and four integer tallies. Everything
else is a local. The grant itself is the library's value type, constructed
per bind and consumed twice.

The manifest database is compiled in as source text and parsed at startup. A
malformed built-in is treated as a build error rather than a runtime
condition — the broker fails loudly and immediately, because a manifest it
cannot parse is a driver it would silently never bind.

## Concurrency

Single-threaded, and the only concurrency is with the children it spawns.
Two places matter.

**Reading a helper's output before reaping it.** The bounded read drains the
pipe while the helper still runs, so a helper producing more than a pipe's
worth cannot deadlock against a broker waiting to reap it. The read is capped
so that a runaway or hostile helper cannot exhaust the broker's memory — the
broker is trusted, the helper is not.

**Detecting an exit without reading the pipe.** A driver's pipe does *not*
reach end-of-file when the driver exits: a single-threaded process defers
closing its descriptors to *reap*, not to exit. So blocking on the pipe to
learn that a driver died would deadlock — the broker holds the only read end
and cannot reap while blocked reading it. It therefore polls for the exit
separately and uses the pipe only for the readiness data. This is the same
asymmetry that makes a shell's drain-before-reap work, seen from the side
that must not rely on it.

## Invariants enforced

**This program is [[inv-i34]]'s fourth leg, and it is the one the kernel
cannot check.** Three of the invariant's four guarantees are kernel
properties: a handle stays within its process's allowance, an allowance stays
within what was conferred, and revocation clears everything. The fourth —
that what was conferred corresponds to the device the driver was actually
bound to — is computed *here* and copied faithfully by the kernel.

The kernel's second guarantee looks like it should cover this and does not,
for a specific reason worth stating plainly: it compares a new allowance
against the *conferrer's* own, and this program holds an unnarrowed one, for
which that comparison passes unconditionally. **The check is not merely
silent about the fourth leg; it is vacuous for exactly the process whose
arithmetic the leg depends on.**

What makes the arithmetic trustworthy is not a check but a structure. One
value is computed per bind and both outputs are derived from it — the kernel
allowance and the descriptor handed to the driver — so the authority and the
account of the authority cannot drift apart. Recomputed per restart attempt,
so a restart re-confers cleanly rather than reusing a stale value.

**One further authority is conferred, and only where it is needed.** A driver
that publishes a service into the namespace needs the service-posting bit,
which the broker holds because init granted it — the kernel's per-bit rule
admits a conferrer that is console-attached *or* already holds the bit, which
is what makes the one-hop delegation possible without the broker being
console-attached itself. It is conferred only to manifests declared
persistent; a driver the broker intends to tear down serves no namespace and
gets nothing.

## Error paths

**A helper failure is non-fatal.** If the sandboxed bus enumerator cannot be
spawned, emits malformed records, emits non-text, or reports a device outside
its own domain, the broker logs it and binds whatever else it found. The
out-of-domain rejection is where the discovery containment is actually
applied: a reported device whose address is not one of the broker's own
trusted slots finds no match and is dropped.

**A readiness timeout is treated as a crash, not as a failure.** A driver
that neither signals nor exits within the bound is killed and fed to the
restart policy, because a hang may be transient. A driver whose first output
line is anything other than the readiness token is treated the same way.

**A driver that cannot be observed at all is structural**, killed and
reported as a hard failure that fails the boot — on the reasoning that a
blind wait on a long-lived driver would hang the broker forever.

**Two outcomes fall outside the tally.** If the grant cannot be computed —
the device's resource count exceeds what an allowance can hold — the bind is
logged and skipped, and none of the four counters move. See *Caveats*.

## Performance

Not a factor. The whole program runs once, before the pivot, over a few dozen
devices, and its dominant cost is waiting: a readiness poll at a fixed
cadence up to a ten-second give-up, and an exponential back-off between
restart attempts. On the reference machine the entire bind phase — five
binds, one of them restarted three times — completes well inside the boot.

The one deliberate cost is the restart ladder: a driver that always fails
burns its full back-off sequence before the broker gives up on it. That is
the intended trade, since the alternative is treating a transient failure as
permanent.

## Prosecution

- **The grant arithmetic**, because nothing re-derives it. The intersection
  must not produce a window, interrupt or bus function the bound device does
  not have. This is the whole of the fourth leg.
- **The single-value discipline**, since it is what keeps authority and
  description honest. Any future path that computes one of them separately
  reintroduces the drift the structure exists to prevent.
- **The gather fold**, because it is the one path that grows a grant beyond a
  single device. The library re-checks that every folded device matches the
  manifest, rather than trusting the caller's own matching — a good
  discipline, and the fold is still where an over-grant would be easiest to
  introduce.
- **The helper's window**, because it is the least-trusted recipient of any
  grant the broker makes.
- **The never-reads-a-register rule**, which is what keeps the
  hardware-poking outside the trusted computing base. A future bus that
  tempts the broker to peek would move that boundary.
- **The soft/hard split**, since it decides whether a hardware problem stops
  the machine. Widening *hard* makes the boot brittle; widening *soft* lets a
  real failure through.

## Seams

- **The manifest database is compiled in.** The design calls for reading
  manifest files from the filesystem; that is a v1.x step and the shape is
  already right, since the database is just a parsed list.
- **Only two bus kinds are enumerated.** Others named by the design are
  unbuilt.
- **Device-removal notification does not yet reach a driver's consumer.** The
  broker can revoke and terminate; propagating that outward is a later step.

## Caveats

**The declared framework-ABI field is never compared, here or anywhere.**
Every manifest declares which framework version it targets, the constant it
should be compared against is defined and exported, and both the design
document and the library's own documentation state that the broker refuses a
manifest whose version it does not implement. No code performs that
comparison. The field is parsed, stored, printed back out when a manifest is
re-encoded, and asserted on in one test — and read by nothing. Today every
manifest is compiled in, so the value is whatever this build wrote; the
moment manifests are read from files, a stale third-party driver binds
silently instead of being refused. Tracked with the library-side finding.

**The audit line under-reports a gathered grant, and the audit line is the
only external check there is.** Every bind is logged with its granted
resources, and for three of the four axes the log prints a *count*, which
survives folding correctly. The fourth prints a scalar — the primary bus
function — and drops the rest. On the reference machine the compositor is
granted four bus functions and the line names one; the only hint that
anything was folded is that its interrupt count is four. The grant itself is
right in both directions: the kernel receives all four, and the driver is
told about all four. What is wrong is the record. That matters more here than
it would elsewhere, because this is a computation nothing re-derives, so a
human reading the boot log is the entire audit — and the one axis the log
reports dishonestly is the one that folding actually multiplies.

**Two of the five terminal outcomes for a matched device are absent from the
summary.** The final line reports how many devices were bound and how each
ended, and invites the reading that bound equals the sum of the rest. A
device that matched a manifest but whose grant could not be computed is
counted in none of them — not even as a failure — so a machine where a
driver silently never started prints an unchanged, entirely green summary.
Not reachable on the reference machine, where no device has enough resources
to overflow a grant; reachable by configuration, since attaching enough
matching devices to a gathered manifest overflows the fold and would leave
the compositor unstarted with nothing in the summary to say so.

**The helper's window is a hull, and its safety argument is about capacity
rather than containment.** The broker grants the bus enumerator a single
window spanning every slot, because the slots outnumber the windows an
allowance can hold — that reasoning is sound and stated. What is not stated
is that the span is a convex hull: anything lying *between* two slots would
fall inside the granted window. On the reference machine the slots are exactly
contiguous and the hull is exact, which is why this is recorded here rather
than filed. A board that interleaves them would want the cheap check the
broker does not make — that no other discovered device's window intersects the
hull.

**A readiness contract has a second consequence it does not state.** Drivers
are told their first output line must be the readiness token and everything
else must go to the console, with the stated reason that a stray log line
would be misread as readiness. True, but that reason only covers output
*before* readiness. After it, the broker closes its read end and walks away,
so a persistent driver writing a second line is writing into a pipe with no
reader. Every driver today honours the contract — the network daemon signals
readiness last and says nothing after — so this has never bitten; a future
chatty driver would find the consequence unstated.

**One failure classification has two producers and a comment for one.** The
unobservable-driver case is documented as unreachable because every driver is
spawned with a pipe — true of one way into that state, but the exit poll
failing produces it too, and that path ends in a hard failure that fails the
boot.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
