---
id: chg-2026-08-02-devices-hwcap-sweep
type: chg
title: "vault sweep: the hardware-capability objects -- I-5 finds its home, and the ledger finds its gap"
date: 2026-08-02
arc: arc-vault
commits: []
touched:
  - sub-kernel-hwcap
  - inv-i5
  - inv-i32
  - moc-kernel-devices
  - sub-substrate-interactive
  - gate-interactive
established:
  - inv-i5
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-02
---
Batch 20, the second of three sweeps of `devices/`. Read from code:
`kernel/mmio_handle.c` (471) + header, `kernel/dma_handle.c` (198) + header,
`kernel/pci_handle.c` (469) + header -- 1138 lines plus headers. Remaining in the
area: the virtio/PCI transports and the synthetic device filesystems.

L-1 had still not reached main (EIGHTH check, `addrspace.h` absent), so
address-space stayed deferred again.

THE MERGE CAME FIRST, AND IT MATTERED. Syncing the branch produced the arc's
first conflict where main had added NEW knowledge to a document the vault had
already absorbed: #125 appended a point 7 to `09-test-harness.md`. Resolving with
`--ours` would have discarded it silently. Folded instead into
[[sub-substrate-interactive]] (the guest-suspension mechanism; why a receive
buffer on the reader is vacuous because capacity belongs to the WRITER's send
buffer; the listener-inheritance fix and its A/B) and [[gate-interactive]] (the
classification consequence: a relay that merely STALLS never dies, leaves no
fingerprint, and the suspended guest then classifies as **guest FAIL** -- the
failure direction is toward blaming the guest).

The property that saved it is worth naming: **a stub replaces the WHOLE file, so
any main-track edit to an absorbed document MUST conflict.** Nothing can
auto-merge past it. Verified for this merge -- main changed six files since
`e4a09831` and exactly one was a stub, the one that conflicted.

...WHICH IS WHEN THE LEDGER GAP APPEARED. That protection covers only what has
been absorbed, and the absorption is far behind the sweep: **37 of 147 reference
documents are stubbed; 110 are still live.** A spot check found 4 of 4 swept
areas had left their document live -- `19-handles.md` (the handle table is swept,
its document is unreferenced by any note), `117-allowance.md`, `107-loom.md`,
`111-cons.md`. So for those 110, a main-track session edits freely, no conflict
fires, and the vault's dossier silently goes stale. The sweep and the absorption
are two different jobs and only one has been kept up. Tracked; it must be
reconciled before view cutover, or stub deletion either loses unabsorbed
knowledge or reopens seventy documents of reading.

THE BATCH'S OWN FINDING: **the same invariant, enforced three completely
different ways, because the three objects differ in where the address comes
from.** A register range is external to the allocator, so exclusivity needs an
explicit overlap-scanned table. A DMA buffer comes FROM the allocator, so the
allocator's partitioning already IS the claim and no table exists -- the
cleanest instance in the tree of an invariant enforced by an ABSENCE, where there
is no code to inspect and the property holds because of what allocation means. A
bus function is claimed by identity and DELEGATES its windows' exclusivity to the
first mechanism. One property, three enforcements, and the middle one is a
deliberate hole in the code.

I-5 MINTED -- ITS HOME AT LAST. Referenced in prose by TEN notes across six
prior batches (six change notes, a hazard, two dossiers), deferred by every one
of them. [[inv-i5]] states it as more than a confinement rule: **these handles
are claims, and a claim you can copy is not a claim.** Exclusivity is not a
property the handle carries, it is the entire content of what the handle means,
so a duplicate would be internally false the moment it existed. Enforcement is
**by partition membership, not by a check at the transfer site**: four disjoint
sets, and the transfer and dup paths ask which set a kind is in rather than
naming kinds. So the bus-function handle inherited both properties in the line
that declared it. Assertions pin the sets pairwise disjoint AND assert their
union is every kind -- so a kind classified nowhere **fails the build** rather
than defaulting to transferable.

I-32 EXTENDED WITH A REAL GAP. **DMA buffers are on none of its axes.** They come
from the same allocator as anon pages and are charged to no counter, so
`page_count` is not the true footprint of any Proc holding hardware. The bound is
differently shaped -- the allowance's PER-BUFFER ceiling, which caps one buffer
rather than their sum -- so the floor here is on *who may ask*, not on how much
they may accumulate. A cumulative per-driver budget is a recorded future item.
This also disarms a hazard that looked live: the DMA path computes its page order
with a LOCAL COPY of the allocator's helper, deliberately unshared -- harmless
only because nothing charges it, and exactly the drift hazard the page accounting
elsewhere was fixed to remove, the moment a cumulative budget lands.

MEASURED. **Thirty registered tests** on the three objects (11 register-range, 9
DMA, 10 bus-function) plus 9 more covering their mapping into a Proc, out of 1238
in the suite. And **every counter in all three files is read only by tests** --
six accessors, zero production consumers anywhere in the kernel or architecture
trees, and three of the six with no caller at all. That is the area's organizing
habit at its limit: where failure has no observer you manufacture one, and here
the only observer manufactured is the test suite.

TWO ABSORBED DOCUMENTS, AND THEY CONTRAST SHARPLY. `115-pci-claim.md` is current
-- audit-fresh, its four findings recorded with dispositions. `39-hw-handles.md`
shows a failure mode the earlier batches had not seen: **its caveat list is
meticulously maintained while its structural body is frozen at P4.** Eleven
caveats, two of them rewritten with strikethrough when the bug was closed, each
cross-referenced to the audit that closed it. Meanwhile the body still says the
capability set is ONE capability (it is twelve), that `struct Proc` is 128 bytes
(400), that the hardware partition covers "all four" kinds (five), and shows the
capability check as the whole creation gate (the allowance's two-step check
landed since). The document is maintained exactly where debugging touches it and
nowhere else.

Which is the same shape as the area itself, one level up: **documentation decays
where being wrong has no observer.** A stale caveat gets someone hurt and gets
fixed; a stale struct size hurts nobody and stays. That is worth holding onto as
a general rule for reading this tree's documents -- the parts that look most
carefully maintained are evidence about where failures happened, not about where
the document is true.

SMALLER FINDINGS. The kernel's own reserved ranges share the claim table with
real claims, distinguished by a SENTINEL OWNER value rather than a second table
-- sound only because the two readers want different things (overlap-checking
ignores ownership; owner-lookup is only called with real pointers), and a future
generic owner scan would match it. Creating an interrupt handle does not require
the right that waiting on it needs, so a caller can mint a handle that can never
be waited on -- documented in the absorbed reference as deliberate, the model
treated as authoritative over the convenience. The window-address arena never
reclaims, on a headroom argument. And the batch-19 seam
([[seam-gic-handler-slot-never-cleared]]) is independently corroborated here:
`39-hw-handles.md` caveat 2 states the same finding and names the missing
`gic_detach` as the proper fix -- one thing the frozen document got right and
kept right.
