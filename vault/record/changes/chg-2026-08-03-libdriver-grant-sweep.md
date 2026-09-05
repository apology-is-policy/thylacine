---
id: chg-2026-08-03-libdriver-grant-sweep
type: chg
title: "libdriver's grant core — and the area claim it does not fit under"
date: 2026-08-03
arc: arc-vault
commits: []
touched:
  - sub-libdriver-grant
  - moc-userspace-runtime
  - inv-i34
established:
  - sub-libdriver-grant
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-03
---
Batch 46: libdriver's authority core — lib, manifest, resource, driver. 4 files,
2267 lines. The first slice of 57d, and the first userspace batch since the
kernel plane whose subject is a real authority computation.

**THE AREA WAS BIGGER THAN THE TASK SAID, SO THE SLICE WAS CUT FROM THE CRATE'S
OWN STATED SPLIT.** 57d was carried as "~20 files, ~10k lines"; measured, it is
**22 files and 12,281 lines** — and libdriver alone is 8 files / 3897, not the
7 / ~3.6k the handoff repeated. Rather than pick an arbitrary line count, the
cut follows the boundary `lib.rs`'s own header draws: `manifest` + `resource`
are PURE and host-tested, `driver` is the single libthyla-rs layer. That is one
coherent subject — everything about what a driver may touch — and it is
batch-sized.

**THE ORGANIZING CLAIM IS THAT ONE GRANT IS COMPUTED ONCE AND CONSUMED TWICE.**
`resolve` intersects a manifest's declared needs with a device node's actual
resources; the result is then encoded two ways. `to_descriptor` becomes argv[1]
and tells the driver *which resources to create handles for*. `to_allowance`
becomes the kernel allowance and says *which it is permitted to create*. The
descriptor informs; the allowance authorizes. The crate states this itself, and
it is the sentence the design rests on: a driver that fabricates an address is
refused by the kernel gate, not by the codec, so the codec's considerable
strictness is defence in depth rather than the boundary.

**AND THE TWO CONSUMERS DELIBERATELY DISAGREE BY EXACTLY ONE PAGE.**
`to_allowance` runs each window through `page_round`, because MMIO maps
page-granular and a 0x200-byte virtio-mmio slot is only reachable by mapping
its whole page. The descriptor keeps the exact sub-page window; only the
allowance is rounded out. That is the one sanctioned widening in the framework —
and it turned out to be the proof of the finding below.

**THE AREA'S ORGANIZING FACT DOES NOT COVER THIS DOSSIER, AND CORRECTING IT WAS
PART OF THE BATCH.** [[moc-userspace-runtime]] says of its plane: "None of this
is a privilege boundary ... a bug in a library here corrupts its own caller's
state and nothing else." That is right about the runtime, and it is the MOC's
whole reason for filing its notes `audit: light`. It is not right about the
library the same MOC reserved a slot for by name.

Traced to ground rather than asserted:

- The kernel's I-34 machinery checks a conferred allowance **against the
  conferrer's own** — the never-widened property.
- The warden's own allowance is **BROAD**: a null pointer, for which the
  kernel's permit check returns true unconditionally (`if (!al) return true;`).
- So that check is **vacuous for exactly the Proc that computes grants**.
- And the kernel never asks whether an allowance describes the driver's own
  device. It cannot: it does not know which node the bind chose.

So `resolve` is not redundant with a kernel check; it is the only place the
correspondence between a driver's authority and its device exists. A defect
there does not stop at the warden — it moves a hardware boundary. The dossier
therefore files `audit: hard` in an area whose MOC declares itself light, and
the MOC now carries the exception rather than being quietly contradicted by its
own child.

The page-rounding above is what makes this concrete rather than theoretical: the
framework ALREADY grants more than the node exposes, deliberately, and nothing
rejects it. If the kernel re-derived the grant, it could not.

**F1 -- `abi` IS REQUIRED, EXPORTED, AND COMPARED TO NOTHING (task #134).** The
manifest's framework-version field is mandatory (a missing `abi` fails the
parse, with a test) and its value is unconstrained: `abi = 999` binds exactly
like `abi = 1`. `MANIFEST_ABI` has three references in the whole tree — its
definition, its own doc comment, and a re-export — and that doc says the warden
"refuses to bind a manifest whose `abi` it does not implement".

What makes it worth filing rather than shrugging at is the sibling. The same
crate carries a *second* version field, the descriptor's, and that one is
checked — at the receiving end, by the driver, fail-closed, with its own test.
So a framework skew is not undetected; it is detected one layer later by the
codec, with a diagnostic naming the codec rather than the manifest nobody
checked. Unreachable today (the database is five string constants compiled into
the warden) and live at exactly the seam that field exists for.

**F2 -- THE CAPS MIRROR THE KERNEL'S BY PROSE, AND THE CODE THAT RELIES ON IT
DISCARDS ITS OWN ERRORS (task #135).** `MAX_MMIO` / `MAX_IRQ` / `MAX_PCI` are
documented as mirroring three kernel constants. Those constants appear in this
crate **only inside doc comments** — never named in code — and there is no
compile-time assert anywhere in it. Meanwhile `to_allowance` drops the result of
every push (`let _ = d.push_mmio(..)`), each of which returns false on a full
array, justified by a comment saying the capacities match. They do, today, by
numeric coincidence.

The interesting part is the failure shape rather than the drift: it would be
quiet *and* misattributed. The grant truncates on the way out, and the driver's
later map fails with `Error::Hardware`, whose own documentation blames the
driver's request — "typically the request fell outside the conferred allowance".
Fail-closed in direction, so a hazard about diagnosis and drift, not privilege.
The fix has an obvious home: `driver.rs` already imports the kernel descriptor,
so three asserts pin all three caps without costing the pure layer its
host-testability.

**THE COUNTERWEIGHTS ARE ABOUT AN INVARIANT TESTED AS AN INVARIANT, AND ABOUT
DEFAULTS.** `resolve_grant_never_exceeds_node` does not assert an expected
value: it iterates the produced grant and checks that every window and every
INTID is a member of the node it came from — the property itself, so a future
axis that violated it fails this test without anyone updating the test. The
gathered fold **re-matches every extra node** before folding anything of its in,
so aggregation does not weaken the property (the handoff flagged this claim as
one to verify rather than inherit; it holds on this half). A degenerate node
with no bdf yields no PCI grant, with a comment saying the discovery layer never
emits such a node but "resolve must not invent authority" — fail-closed against
a case that cannot happen.

And every default in the schema is the least-authority one: no `needs` block
means no MMIO, no IRQ, no DMA and no PCI; an absent `pci` key grants nothing
even on a node that has a bdf; an absent `lifecycle` means the warden tears the
driver down rather than leaving it resident; an absent `restart` is `OnCrash`
rather than `Always`. A half-written manifest is safe, not merely invalid.

**TWO QUESTIONS DISSOLVED ON MEASUREMENT RATHER THAN BECOMING FINDINGS.** The
descriptor is a single argv element built from up to 8 windows, 8 IRQs and 8
bdfs — worst case around half a kilobyte, against a 64 KiB argv budget, so
truncation does not arise. And the manifest's documented "most-specific first by
convention" for `binds` turns out to be inert at `resolve`, which walks the
*node's* ids most-specific-first: the ordering that decides is the DTB's, which
is canonically ordered already. Neither is a defect; both are recorded because a
reader would otherwise ask.

**AND THE INVARIANT NOTE HAD ALREADY RESERVED THE PLACE THIS DOSSIER FILLS.**
[[inv-i34]] was written from the kernel side at batch 24 and says, unprompted:
"Three of the four legs are kernel-enforced; the fourth is not ... that grant is
computed by the supervisor, and the kernel copies whatever it is handed", with a
matching `blind-to`. So no correction was needed — the sweep arrived at exactly
the gap the earlier note had named, two batches before the code that occupies it
was read. What the note gains here is an *address* for the fourth leg, plus the
sharper reason the kernel's second guarantee does not quietly cover it: the
conferred-within-conferrer check is not merely silent about the fourth leg, it
is vacuous for the one Proc the leg depends on.

Worth recording as method rather than as content: the plane structure predicted
where a note would go before the sweep got there, which is the first time in the
arc that an invariant note has anticipated a dossier instead of being corrected
by one.

LEDGER, read off the rendered view before being written here (batch 44's
inversion has not recurred). Corpus 846 -> **848**. Coverage 257 -> **261 owned
of 421**, 61% either way (261/421 = 61.99, still floors to 61); unswept lines
48060 -> **45793**.

**The unswept delta is exactly the batch's line count — 2267 — and that is the
complement of last batch's discrepancy rather than a contradiction of it.** Last
batch the figure fell 1952 against 2286 swept, and the 334-line gap was traced
to a merge adding unowned lines while the branch worked. No merge intervened
this time (`vault/bootstrap..main` is empty), so there is nothing to reconcile
and the two numbers agree to the line. The check is worth keeping precisely
because it does both: it disagreed when something else had moved, and it agrees
when nothing has.
