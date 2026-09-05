---
id: chg-2026-08-02-authority-sweep
type: chg
title: "vault sweep: the kernel authority substrate"
date: 2026-08-02
arc: arc-vault
commits: []
touched:
  - sub-kernel-handle
  - sub-kernel-caps
  - sub-kernel-allowance
  - sub-kernel-perm
established: []
closed: []
opened:
  - seam-devcap-plain-caps-read
mirrors-checked: []
depth: skeletal
created: 2026-08-02
---
Batch 13, and a course correction. Read from code: `handle.{c,h}`,
`caps.h`, `devcap.c`, `allowance.c`, `perm.c`, plus the capability and
allowance arms of `rfork_internal` and `proc_become_legate`. Four dossiers
under `system/kernel/security/`.

THE COURSE CORRECTION, AND ITS MEASUREMENT. This batch set out to be the
first REGISTRY pass — the invariant family, which the corpus wants most: a
scan of `I-N` mentions across every note found eighteen invariants referenced
in prose but never minted, led by I-5 (seven mentions across five areas),
I-34, I-12, I-39 and I-22. Nine of them form one interlocking family
(I-2/5/22/23/25/26/27/34/39) where I-2 is the monotonic-reduction rule the
rest specialize, so minting them together was the obvious shape.

Then the schema's own field list stopped it. An `inv` note requires
`guards`● — sub ids — and FOUR of the nine (I-5, I-26, I-34, I-39) had no
swept enforcement home at all, because `handle.c`, `devproc.c`,
`allowance.c` and `cons.c` are unswept. The `guards` edge is exactly what
makes an invariant note more than a restatement of ARCH section 28; minting
I-34 with an empty one would have produced the scripture-restatement the
standing bar forbids. The schema agrees in its own ordering — step 3 is the
subsystem sweep, step 4 the registry passes — and `arc-vault`'s sweep exit
criterion is still unchecked. **THE REGISTRY PASS IS NOT INDEPENDENT OF THE
SWEEP.** So the dependency was pulled forward and this became the sweep of
the subsystems the invariant family enforces in.

THE ORGANIZING FACT is that authority is three orthogonal axes — capability
(may this Proc do this KIND of thing), rights (may this REFERENCE be used
this way), identity (is this PRINCIPAL allowed at this object) — with the
hardware allowance a fourth layered under the first. What makes the area
coherent is that the separation is enforced by deliberate REFUSALS to
conflate: `perm_check` special-cases no `principal_id`, not even
`PRINCIPAL_SYSTEM`, so the DAC-override is a capability and never an
identity; `CAP_DAC_OVERRIDE` is deliberately not an axis on the kill or
debug gates, keeping fs-admin orthogonal to process control; and
`rights_for_omode` is written as a matched pair with `perm_want_for_omode`
so a granted envelope can never exceed the checked access.

I-5 IS A COMPILE-TIME PARTITION, NOT A RUNTIME CHECK. Every `kobj_kind`
belongs to exactly one of four masks, pinned by six pairwise-disjointness
`_Static_assert`s plus a COMPLETENESS assert that their union covers every
kind but `KOBJ_INVALID`. That last one is the load-bearing one: it makes
"add a kind and forget to classify it" a build failure rather than a handle
that falls through every partition test. The runtime guard is then a single
expression that is the exact negation of the spec's precondition.

THE ACQUIRE/RELEASE ASYMMETRY IN `handle.c` IS DELIBERATE AND DOCUMENTED.
`handle_acquire_obj`'s `KOBJ_SRV` arm is a no-op balanced against a release
arm that does work — sound ONLY because a `KObj_Srv` handle is now always a
non-refcounted service listener, and the code says outright that if one ever
again named a refcounted `SrvConn` this no-op would underflow the get/put
pairing into a UAF. `KOBJ_LOOM` and `KOBJ_PCI` are the contrasting cases and
explain why they MUST bump.

I-25's PRIVILEGE GUARANTEE RESTS ON THE ROOT ALONE. `rfork` strips
`CAP_ELEVATION_ONLY` unconditionally, so a legate scope MEMBER never holds
the elevated caps — only the root does, and it dies on its own exit or at
`valid_until`. That makes the teardown walk a tidiness sweep rather than the
invariant's enforcement, and a straggler it misses an unelevated Proc with a
stale tag rather than a violation. The clearance window opens at REDEEM, not
at grant, with a saturating add so a large `valid_for` clamps to `~0` rather
than wrapping to a small deadline — or to exactly 0, which is the sentinel
meaning "no time bound", an alias the clamp exists to remove.

DRIVERS ARE LEAVES, AND THE GATE IS ON THE ALLOWANCE, NOT THE IDENTITY.
`rfork_internal` refuses outright for a narrowed parent, because
`proc_group_terminate` is thread-group-scoped: a child Proc would be
reparented to init, leaving a hardware-capable grandchild holding live
MMIO/IRQ/DMA off the warden's chokepoint. Unlike the I-32 resource caps
there is deliberately NO `PRINCIPAL_SYSTEM` exemption — a SYSTEM-identity
driver is still a sandboxed leaf.

NEW SEAM: [[seam-devcap-plain-caps-read]] (task #15). A census of every
`->caps` read in the kernel found exactly two plain loads left — `devcap.c`
lines 180 and 218, the two grant-REGISTER gates. `syscall.c` (sixteen),
`devproc.c` (four), `devctl.c`, `perm.c` (two) and `proc.c` (two) all use an
acquire load, most carrying an explicit comment that a plain load is C11-racy
now that `proc_become_legate` is a cross-thread writer. The sharp part is
that RW-5 F1 hardened the atomic OR at line 333 OF THE SAME FILE, reasoning
carefully about why plain access to `p->caps` is unsound, and left the two
reads above it — the "a fix on site N stops you asking about site N+1"
pattern at its shortest possible range. Low severity (aligned `u64` loads
cannot tear on aarch64; corvus is single-threaded), so what is lost is the
compiler-level edge and the consistency that makes the rest of the sweep
trustworthy.

LAYOUT, READ FROM THE SCHEMA THIS TIME. The schema declares twelve kernel
areas including `security/`, which has existed EMPTY since commit 0 — so the
authority substrate went there rather than into the `authority/` directory
the batch would otherwise have invented. That is batch 12's lesson applied
prospectively instead of retroactively.

The same read found the converse drift: `scheduling/` (four notes) and
`srv/` (three) are POPULATED but appear nowhere in the schema's layout
block. Unlike batch 12's seam split — where an undeclared directory
DUPLICATED a declared home and the fix was to move the files — these are
genuine areas with no declared home at all, created by sweeps that
deliberately split scheduling from execution and `/srv` from devices.
Relocating seven notes into ill-fitting declared areas would be the wrong
correction, so the schema's layout block is AMENDED to declare them. That is
bookkeeping catching up with what the sweep discovered, not a semantic
change — recorded here so the amendment is visible rather than silent.

THE BLIND SPOT, VERIFIED RATHER THAN ASSERTED. Batch 12 concluded that an
id-keyed linter cannot see a filesystem-layout error; this batch probed it.
Moving a dossier into a freshly-created directory the schema does not
declare lints **CLEAN** — 695 notes, 0 fail, 0 warn. So nothing mechanical
will ever catch this class, which is what makes amending the schema the
correct response instead of trusting the gate, and what makes "read the
layout from the schema, never copy it from a neighbour" a rule that has to
be followed by hand every time. The companion probe (deleting a required
dossier section) failed on exactly its target, so the gate is live for what
it does cover; both restored byte-identical.

SCRIPTURE CORRECTED BY ITS OWN HEADER, and worth carrying forward. `caps.h`
records that three documents (`JIT-ON-WX-DESIGN.md`, `LLVM-DESIGN.md`
section 8, ARCH section 28 I-42) describe `CAP_JIT` as "elevation-only,
non-rfork-grantable, the `CAP_HW_CREATE` class" — and that read literally the
trailing phrase is WRONG, since `CAP_HW_CREATE` is fork-grantable, which
contradicts both "non-rfork-grantable" beside it and I-42's own
"non-heritable" clause. The header explicitly warns against "fixing" the bit
toward `CAP_ALL` on the strength of that phrase.

TWO STALE ENUMERATIONS IN THE SAME HEADER, in the direction that matters
least and teaches most: the `CAP_ELEVATION_ONLY` comment says "All five" and
lists six; the `CAP_ALL` comment names four elevation-only caps and omits
`DEBUG` and `JIT`. The MACROS are correct and the `_Static_assert`s pin
them — it is the prose that drifted. A static assert can pin an expression;
nothing pins a sentence.
