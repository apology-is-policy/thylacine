---
id: chg-2026-08-04-virtio-programs-sweep
type: chg
title: "The virtio programs — a guard defeated by a cast, and a bound outside the call it bounds"
date: 2026-08-04
arc: arc-vault
commits: []
touched:
  - sub-virtio-probes
  - sub-menagerie-leaves
  - moc-userspace-hardware
  - moc-userspace
  - sub-netdev
  - sub-stratum-bdev
established:
  - sub-virtio-probes
  - sub-menagerie-leaves
  - moc-userspace-hardware
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-04
---
Batch 50: the seven virtio programs — five standalone reference drivers and
the two the broker hands a device to. 4663 lines across seven crates, and the
last slice of the driver-framework arc.

**A NEW AREA, AND ITS FACT HAD TO BE DERIVED FROM THE SET RATHER THAN FROM ITS
LARGEST MEMBER.** These are programs, so `runtime` (explicitly libraries) was
wrong; they serve no protocol and nobody connects to them, so `services` was
wrong; and `boot-chain`'s fact — every member is spawned holding an authority it
exists to give away — describes *delegators*, while all seven of these are
recipients. The honest fact covering all of them: **each touches hardware
directly, none is a service, none has a user, and each is spawned by something
above it to do one bounded job and report through a channel that party chose.**

The area's internal split is the useful part and is stated rather than smoothed
away: five predate the driver framework, use the raw capability syscalls, and
run under *broad* hardware authority (which is why each hardcodes its bank
base); two are granted a *narrowed* allowance and can touch nothing else. The
same work done twice, once with the authority assumed and once with it
conferred — the tree's clearest before-and-after on [[inv-i34]].

**WHAT THESE PROGRAMS ARE, WHICH NO HEADER SAYS.** Four dossiers already cited
them before this batch, and one of them settles it: [[sub-stratum-bdev]] records
that Stratum's block backend is a *port of* `usr/virtio-blk-rw`. [[sub-netdev]]'s
MMIO transport is the network pair generalized into a library. So these are not
only proofs — they are the reference implementations two production drivers were
read from, and they keep running as kernel-suite gates afterward. Not one of the
seven headers says so, and the two findings below are both consequences of that
silence: the derived copies were audited as production code, the originals
reviewed as probes.

**F1 — A GUARD DEFEATED BY A CAST ONE STEP EARLIER (task #147).**
`virtio-net-loop` reads the device-chosen used-ring descriptor id as a 32-bit
value and narrows it to 16 bits *in the same expression*. Everything downstream
then validates the already-folded value — including a guard that is correctly
written, correctly placed, and carries a comment about the out-of-bounds read it
prevents. It cannot prevent it, because by the time it runs, 0x1_0000 is 0.

Both siblings facing the same field bound the full width. One of them,
`virtio-input`, names this exact mistake in a comment — "a `desc_id as u16`
truncation would let 0x1_0000 pass the bound" — **while citing `virtio-net-loop`
as the pattern it mirrors**. The production descendant guards it too and
additionally refuses to recycle a bogus id, citing an audit finding, because
re-publishing one can double-post a descriptor the device still holds.

So the truncation survives in the one member the other two treat as the model.
Confined damage — a frame parsed from the wrong buffer and a possibly-live
descriptor re-published, no out-of-bounds access, nothing crossing a process —
and unreachable against a device that does not lie. The lesson is the ordering:
**a bound is only as good as the width of the value that reaches it**, and the
narrowing happened in a different function from the check.

**F2 — A BOUND OUTSIDE THE CALL IT CLAIMS TO BOUND (task #146).** The IRQ wait
has no timeout. `virtio-net-arp` and `virtio-net-loop` wrap it in a counted loop
and describe the count as a defensive ceiling against a hang — but a counter
outside a blocking call bounds how often the loop *completes*, never how long one
pass may *take*, so it cannot fire in the case its comment names.
`virtio-net-loop` has a concrete route to it: transmit completions keep the loop
fed while replies do not arrive, and once the last completion drains there is
nothing left to fire.

The narrowing is what makes it a finding rather than a sweep. Checking each
member individually collapsed it from four programs to two: `virtio-blk-rw` and
`virtio-gpu` carry counters that increment **only on a wake lacking the
used-buffer bit**, which bounds a spurious-wake storm, and both comments say
exactly that. They are honest. They would still block on a silent device, but no
comment claims otherwise.

And **one member cannot hang at all**: `virtio-input` has its interrupt pre-fired
by the spawning test and follows with a wall-clock-bounded poll carrying an
unconditional iteration backstop. Its comments cite the two bugs that produced
that shape — one where a fixed iteration count was outrun by a fast accelerated
vCPU against emulated async delivery, one where a window sized on the emulated
substrate missed 23 of 40 boots under gate load. **The tree learned this lesson
twice, with bug numbers, and applied it to the sibling that suffered it.** The
other four were written against the same substrate and never got it. Since these
are graded by a kernel test that reaps them, a hang is a boot hang: the full
harness timeout with no diagnosis.

**F3 — THE LEAST-PRIVILEGED PROGRAM TAKES MORE THAN IT USES (task #145).** The
bus source maps its granted bank read-write and performs two loads per slot and
no stores; its own comment notes that the accessors only read. Read-only mapping
is supported at both the library and the kernel, which validates the requested
protection against the handle's rights explicitly. It matters here more than it
would elsewhere because this program exists *to be* the sandbox — the broker
spawns it so the trusted side never touches a device register — and its bank
spans every virtio slot on the machine, including the block transports the
filesystem server claims later. The distance between held and used is the ability
to write any virtio register on the machine, for a program that writes none.

**TWO CANDIDATES DISSOLVED ON MEASUREMENT, AS IN THE THREE PRIOR BATCHES.** The
source appeared to leave its bank claimed past the point a driver needs the slot
— it does not, and the reason is in a different program: the broker reads the
source's pipe to end-of-file, which requires the exit, and only then binds. The
source's own explicit release is good practice with an overstated justification.
Separately, its per-slot read looked unbounded until the check turned up: it
gates the **entire read extent** against the granted window rather than the base
address, and its comment says the over-run case is unreachable and gates it
anyway. That is the discipline stated correctly, and it is the counter-example to
F1 and F2 sitting three files away.

**A NOTE ON THE AREA'S EXPECTED FINDINGS.** Every bound in both halves is either
correct or fails safe, and no defect found here can corrupt anything outside the
program holding it. What goes wrong is **the account** — an obligation stated on
the transport that does not need it, a counter described as protection it cannot
give, a page-sharing constraint whose only written statement describes a deleted
function, a guard defeated by a cast. That follows from what these programs are:
four of seven are proofs whose passing is the only feedback anyone gets, and a
proof that passes says nothing about whether its comments are true.

**THE MERGE-FALSEHOOD DUTY RAN AND FOUND NO FALSEHOOD — BUT FOUND SOMETHING
ELSE.** Main's arriving commit touched the console renderer and its reference
doc, which this branch has not stubbed, so no Present-plane claim became false
and no stub tripwire fired. But the commit's own reasoning rested on two
compositor facts, and [[sub-tapestryd]] — swept, owned — carries neither: that
frame ticks reach visible surfaces only, and the hidden-present contract. The
dossier documents what happens to a tick once it exists and never its emission
condition; the half it carries protects the server, the half it omits is the half
a client needs. Filed as task #144. **A dossier can be accurate everywhere it
speaks and still omit the precondition that is the whole of the contract.**

LEDGER, read off the rendered view rather than predicted. Corpus 855 -> **859**.
Coverage 270 -> **277 owned of 421**, 64% -> **65%**; unswept lines 41893 ->
**37292**.

**AND THE RULE EARNED ITSELF A THIRD TIME, IN TWO NEW WAYS.** Both wrong numbers
were written down before rendering, and each had a distinct cause worth keeping:

- **Files: predicted +14, actual +7.** I listed each crate's manifest alongside
  its source in `code:`, and counted both. The coverage census enumerates source
  files only — a manifest can be owned for provenance without being in the
  denominator. Last batch's correction was "it counts files, I counted dossiers";
  this batch I counted files and still missed, because I did not know which files.
  Two wrong models of the same denominator in two batches.
- **Lines: predicted -4663, actual -4601.** The sweep removed exactly 4663, and
  main's arriving commit **added 62** to an unswept file — aurora grew by 87
  inserted less 25 deleted, to the line. The two batches before this one both
  reported the delta as exactly the swept lines with no residue, and batch 48
  went so far as to call that a check passing. It was, but only because main had
  not touched an unswept file either time.

The generalization is the merge duty's arithmetic twin: I checked whether main's
commit falsified a **claim**, and it did not — but I did not think to check
whether it moved the **ledger**, and it did. A sweep's line delta is a statement
about the whole tree, not about the batch, and it is only equal to the batch's
own work when nothing else changed. Fifth batch running this arithmetic, fifth
distinct behaviour.
