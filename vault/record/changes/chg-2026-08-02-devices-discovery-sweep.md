---
id: chg-2026-08-02-devices-discovery-sweep
type: chg
title: "vault sweep: hardware discovery -- I-34 minted, and a document that describes its own audit in the future tense"
date: 2026-08-02
arc: arc-vault
commits: []
touched:
  - sub-kernel-discovery
  - inv-i34
  - inv-i5
  - inv-i15
  - inv-i32
  - sub-kernel-hwcap
  - sub-kernel-allowance
  - moc-kernel-devices
  - sub-substrate-builders
established:
  - inv-i34
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-02
---
Batch 21, the third of FOUR sweeps of `devices/` -- the area is four, not the
three previously estimated: the remaining 3351 lines are two unrelated stories
(the synthetic filesystems and the entropy source), so cramming them into one
sweep would have been a shape imposed for tidiness. Read from code:
`kernel/virtio.c` (351), `kernel/virtio_pci.c` (305), `kernel/devpci.c` (497),
`kernel/devhw.c` (404) plus both transport headers -- and `kernel/allowance.c`
(252) for the invariant.

L-1 had still not reached main (NINTH check, `addrspace.h` absent), so
address-space stayed deferred again.

THE MERGE, AND THE LEDGER GAP IN ACTION. Main had moved twice. The sync was a
clean auto-merge -- three files, no conflict -- and that is precisely the
condition batch 20 warned about: the stub protection only fires on ABSORBED
documents, and `usr/ports/mesa/README.md` is not a reference document at all.
So #120's builder knowledge (the dirty-tree guard, and the deliberate decision
to leave the keep machine's Mesa fork dirty) landed on an area already swept,
with nothing to catch it. Folded manually into [[sub-substrate-builders]]: the
guard's argument is a failure-MESSAGE argument (git's own refusal names neither
the script nor the series, so the guard says it where the fix is knowable), it
checks only TRACKED changes because those are exactly what the apply refuses,
and it is the same shape as that dossier's artifact assertions -- both replace a
misdirecting failure with one that points at the cause. This is a second
data point for the ledger gap: an unabsorbed document drifted, in an area whose
dossier says `updated: 2026-08-01`, and only a manual check found it.

THE BATCH'S OWN FINDING: **discovery is a projection, and what the projection
hides is decided by whether reading the source is itself an act.** Two Devs
built to the same shape disclose wildly different amounts, and the reason is not
policy. The device tree is inert data -- a buffer relocated at boot and never
written -- so `/hw` hands back property bytes VERBATIM, big-endian, and lets the
consumer parse. PCI config space is a live window where a read is a bus
transaction and a write reconfigures hardware, so `/hw/pci` publishes no window
at all: it republishes CONCLUSIONS. The interrupt number is where that shows
sharpest -- building a ctl line performs a real config-space read of the
function's declared pin, swizzles it through the interrupt map, and passes only
the resulting GIC number. The driver gets the answer and never the mechanism.
That is the entire content of "mediated".

I-34 MINTED -- referenced in prose by TEN notes with no note of its own, exactly
the state I-5 was in before batch 20, and BOTH Devs here define their posture
against it ("visibility, not authority -- the boundary is I-34"). Read
`allowance.c` rather than paraphrasing the batch-13 dossier. [[inv-i34]] states
the two-axis framing (the capability says MAY YOU, the allowance says OVER
WHAT), the two-step create whose second step re-checks under the lock the revoke
takes (the central hazard: a create in flight when a device is removed), the
immutability that makes the lock-free first check sound, the confer gate that
can only narrow, and the leaves rule (a narrowed Proc is refused children, so no
hardware-capable grandchild can outlive a revoke). **The honest part: three of
the four legs are kernel-enforced and the fourth is not** -- that the conferred
set corresponds to the device actually bound is the supervisor's policy, and the
kernel copies whatever it is handed. The code says so in its own header.

AND IT EXPLAINS BATCH 20'S I-32 GAP. The allowance's transfer bound is a single
maximum SIZE, so there is nowhere in its data model for a SUM to live. The
missing cumulative DMA budget is therefore structural in I-34 rather than an
oversight in I-32 -- a cumulative bound would have to extend the model, not add
a check. Both notes now say so.

AND IT SHARPENS I-5's RELAXATION. The recorded argument for leaving the virtio
transport slots un-reserved was convenience (reserving would need a delegation
API). The mechanical reason is stronger: reservation works at PAGE granularity
and the slots are packed EIGHT TO A PAGE, so a driver claiming its own slot
necessarily claims seven neighbours and no reservation could take one without
taking all eight. The live configuration depends on it -- the kernel's entropy
source shares a page with a userspace driver's device -- which is why the
death-time quiesce must exclude that device BY IDENTITY rather than by
ownership. A better API alone would not have avoided this.

THE DEFECT (task #29). **The reported interrupt and the claimed interrupt are
computed differently.** devpci's ctl line routes the pin the function DECLARES;
the claim path hardcodes the first pin. The interrupt map keys on (dev, pin) --
that is what the swizzle is for -- so for any function declaring a different pin
the two disagree, and for a function declaring NO interrupt the claim can still
mint a binding the device never asked for, on a number that by the swizzle may
belong to another slot. Verified: the divergence, and that the route depends on
the pin. NOT verified: which pins the current devices declare (believed uniform,
which is why this is dormant). Found by pulling on an include whose own comment
says the constant is unused -- it is; the include is vestigial, and following
that thread led to the constant's real user one file over. Third instance in two
batches of the same shape: two independently maintained answers to one question,
agreeing today by accident of configuration (cf. the duplicated page-order
helper, and #106 which BIT).

MEASURED. Thirty-seven registered tests across the four files (11 devhw, 6
devpci, 8 virtio_pci, 12 virtio) out of ~1250. **No locks anywhere in the four
files** -- both device tables are written before secondary CPUs start and are
read-only afterwards, so immutability IS the synchronization. That immutability
is load-bearing in three separate places: a namespace index minted by a walk
stays valid forever; looking a device up by identity and then claiming it reach
the same function (which is what makes I-34's two-step sound); and a qid can be
a raw offset rather than a handle. A ctl line is 63 bytes in a 96-byte buffer.
Only the first PCI bus is mapped -- **the kernel's view of the machine is
bounded by its own mapping window, not by the machine.**

SMALLER FINDINGS. Each namespace borrows its identity from its source's own
addressing: a `/hw` qid IS a byte offset into the flattened tree, a `/hw/pci`
qid is an index with a parity bit. Both carry a SENTINEL inside that space,
safe only because the code decodes it first -- the second instance of the area's
sentinel habit after batch 20's reserved-owner value, and safe for the same
reason: not that the value is unreachable, but that the decoder is controlled.
PCI's config accessors fail toward ALL-ONES, which is the bus's own idiom for
nothing-here, so a refused read is indistinguishable from an absent device --
correct by construction rather than by luck. Non-seekability is load-bearing,
not incidental: a directory cursor here is a raw structure-block offset, and a
seek could aim it into the middle of a token. A truncated PCI enumeration is NOT
surfaced despite comments promising it is logged and bannered -- the sibling MMIO
enumeration does report what it skipped, so the one path that can silently
under-report the machine is the one whose comment says it cannot.

DOC ROT, SHARPENED INTO A RULE. Batch 20 found a document maintained exactly
where debugging touched it (a meticulous caveat list over a body frozen several
phases back) and gave the area its rule: documentation decays where being wrong
has no observer. The two older documents here sharpen it. `35-virtio.md` froze
at P4-F and describes its own closing audit **in the future tense** -- "the
closing audit will prosecute the transport against [list]" -- while that audit
has since run and produced the death-time reset, the power-of-two rejection and
the status readback, all now load-bearing and none present. One listed item is
even stated as settled mechanism where the audit's finding was that the
mechanism is insufficient. Its test table says seven; eleven are registered, and
**the four it omits are exactly the four the audit added** (verified by diffing
the table against the registry).

So: **forward-looking prose cannot become wrong.** "The audit will prosecute X"
stays grammatically true forever, whatever the audit found -- rot-proof, and
therefore worthless as a record, while reading like diligence. That is the same
shape as the area's organizing fact one level up: a claim decays where being
wrong has no observer, and a claim about the future is permanently unobservable.
Recorded in [[moc-kernel-devices]] as a rule for reading this tree's documents.

Two of the four absorbed documents (`116-devhw.md`, `120-devpci.md`) are CURRENT
-- both Menagerie-era, both knowing the synthetic mount point and the allowance
boundary. The currency tracks the document's AGE, not its subject. Neither is in
the reference index at all (they are among the 48 unindexed from 99 up), which
is the ledger gap again from the other side.
