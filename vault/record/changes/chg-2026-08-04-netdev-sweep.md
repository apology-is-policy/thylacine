---
id: chg-2026-08-04-netdev-sweep
type: chg
title: "The NIC transport — a teardown obligation stated on the transport that does not need it"
date: 2026-08-04
arc: arc-vault
commits: []
touched:
  - sub-netdev
  - moc-userspace-runtime
  - sub-netd-nic
established:
  - sub-netdev
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-04
---
Batch 49: the NIC frame transport — two virtio transports and the ring
arithmetic they share, 1587 lines across four files. The fourth slice of 57d,
and the layer both driver-framework halves exist to hand a device to.

**THE AREA GAINS A THIRD EXCEPTION, FOR A THIRD REASON — AND THAT IS THE
BATCH'S ONE STRUCTURAL FACT.** [[moc-userspace-runtime]]'s organizing claim is
that its libraries are not privilege boundaries, because the kernel validates
everything handed to it, so a bug corrupts its own caller and nothing else. Two
libraries already broke it by *computing an authority*. This one breaks it at a
different joint: it computes nothing and decides nothing, but the containment
argument names exactly one party across the boundary, and a NIC driver has a
second. Once the queues are armed the kernel does not mediate a single byte the
device writes. **The premise is not weakened there; it is absent** — so the
descriptor bounds and length clamps in this crate are not defense in depth but
the only check that will ever run, the same no-second-opinion shape as
[[sub-warden]]'s grant arithmetic with a device in place of a manifest.

The corollary that makes the whole batch legible: **a thread stops when its
process dies; an armed virtqueue does not.** It is a standing instruction to
write named physical addresses, and killing the driver does not retract it.
Everything below follows from that one asymmetry.

**F1 — THE TEARDOWN OBLIGATION IS STATED ON BOTH TRANSPORTS AND IS TRUE OF
ONE.** Both files tell a long-lived driver to stop the device itself, because
the supervisor's teardown is a forced group-terminate that skips the Rust
destructor. Traced to ground on each side, and they came out opposite:

- **PCI**: the kernel already does it. Releasing the device handle clears the
  function's bus-master bit before releasing anything else, so the device can
  initiate nothing. The production network daemon — the only PCI consumer, the
  NIC owner the whole isolation story rests on — never calls `quiesce()`, and
  is safe for a reason neither document mentions.
- **MMIO**: no equivalent exists. Releasing a memory-window handle frees a
  claim slot and touches no device state. The driver's own reset is the entire
  fence.

The emphasis is inverted: the file that names the supervisor explicitly and
insists a warden-bound driver must reset the device itself is the PCI one,
where the kernel covers it; the vaguer statement sits on the transport where it
is load-bearing. **I nearly filed this as a P1 on the network daemon.** The
check that dissolved it was reading what `KObj_PCI` release actually does
rather than what the library says the driver must do — batch 47's lesson in the
same direction it was learned, for the third batch running.

**F1b — AND THE FENCE THAT SAVES IT IS AIMED SOMEWHERE ELSE.** Its stated
purpose is stopping memory decode before the physical BAR ranges are
re-handed-out; stopping transfers into the driver's own ring pages is a
*consequence*. It lands before those pages are freed only because handles are
released in ascending slot order and bring-up claims the function before
allocating its three memory regions. A driver that allocated its pools first —
a natural choice, failing fast on memory before touching hardware — or one that
closed a lower descriptor and then allocated, inverts it. This is one turn past
batch 48's shape: not a structure holding a property nobody named, but a
structure **aimed at a different resource entirely**, holding this one by
coincidence of ordering. Recorded as a caveat at both ends rather than filed:
nothing is wrong today, and the anchor is one line in each place.

**F1c — THE ONE CALLER THAT HONOURS THE OBLIGATION HONOURS IT BY QUIESCING
BEFORE IT IS USEFUL.** The Menagerie driver resets the device, *then* signals
readiness, then blocks forever on an interrupt a reset device cannot raise.
Correct for what it is — a lifecycle proof whose expected end is that forced
teardown — but uncopyable by a driver carrying traffic, which must keep the
device live and therefore has no correct moment to quiesce, because no removal
notice reaches a driver ([[sub-warden]]'s recorded seam). So the MMIO
obligation is not merely unmet; **as written it is unmeetable by any driver
that does its job.**

**F2 — A LIFECYCLE CHOICE NOBODY DOCUMENTED IS WHAT KEEPS THE DISK REACHABLE
(task #142).** Register windows map page-granular, so the framework page-rounds
every memory allowance and the MMIO driver claims the whole page holding its
slot. Correct, and the containment check then passes exactly. The consequence
nobody states: on the reference machine that page also holds two block-device
slots, memory claims are page-exclusive, and the filesystem server probes the
bank page by page for its disk. It works only because the Menagerie driver's
manifest omits a persistence marker, so the supervisor tears it down before the
pivot — confirmed in a boot log. That driver's own header names making it
persistent as its next step, which would hold the page past the pivot and lock
the filesystem server out of its disk. Two documents mention pieces of this and
neither connects them.

**F3 — AND THE ONE PLACE THE PAGE-SHARING IS WRITTEN DOWN DESCRIBES A FUNCTION
THAT DOES NOT EXIST (task #143).** The MMIO module header explains a
constructor that probes each page of the bank, keeps the one reporting a
network device, and releases the rest. There is no such constructor; the file
has only the grant-driven one, which is told its slot and probes nothing — and
the retirement is recorded forty lines above, in the same file. Worse, the
paragraph's live half names the wrong pair of programs: it warns that the
network daemon and the filesystem server cannot both hold the page, but the
network daemon is on PCI and never touches that bank. **The tree's only written
statement of a live constraint points at a collision that cannot happen while
describing the one that can as already solved.**

**TWO CANDIDATES DISSOLVED ON MEASUREMENT AND ARE CAVEATS, NOT TASKS.** The
transmit-reclaim clamp looked like it might under-protect a device that reports
progress *behind* the cursor — it does over-reclaim, but only up to the
in-flight count, which is the same exposure a device gets by claiming
everything is complete, an outcome the clamp's own test asserts as desired. The
bound is on wedging, not on buffer reuse; a lying device can always make the
driver overwrite a buffer it is still reading, and the damage stays inside the
driver's own outbound bytes. Separately, the page-rounded allowance looked like
an escape from the granted window until the framework's rounding helper turned
up, tested against this exact case.

**A DELIBERATE NON-CLAIM.** The Menagerie driver (216 lines) was read in full
and used throughout — it is the only consumer of the MMIO half and the whole
evidence for F1c — but it is **not owned by this batch**. It is a program, not
a library, and its natural siblings are the virtio probes arriving next; filing
it under a library area, or minting a programs area whose organizing fact I
would be deriving from one demo, are both the batch-46 trap. So the ledger
moves by 1587, not 1803, and that is honest rather than conservative.

LEDGER, read off the rendered view rather than predicted. Corpus 853 ->
**855**. Coverage 266 -> **270 owned of 421**, 63% -> **64%**; unswept lines
43480 -> **41893**.

Main did not move between batches — the first time in this run of the arc — so
there was no merge-falsehood pass to run and no ledger contribution from
anywhere but the sweep. Fifth batch running this arithmetic, and the line delta
is again exactly the swept lines with no residue.

**And the rule earned itself again in the same paragraph.** I had written 856
and 267 before rendering, and both were wrong while the line count was right:
the corpus grew by two notes rather than three (no area MOC this time, unlike
last batch), and coverage grew by **four** rather than one, because the ledger
counts FILES and I was counting DOSSIERS — batch 48 happened to own a
single-file program, which made the two look like the same number. A habit
formed on a one-file batch is not a rule; the view is.
