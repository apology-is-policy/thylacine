---
id: inv-i43
type: inv
title: "I-43 — a phenotype confers ABI shape, never authority"
number: I-43
guards: [sub-diorama, sub-kernel-vivarium, sub-viv]
validated-by: [prose]
strength: prose
created: 2026-08-04
updated: 2026-08-06
---
## Statement

A compatibility layer that presents Thylacine state in a foreign operating
system's shape may confer that shape and nothing else. It supplies the
*format* a foreign binary expects; it must never supply an *answer* the
native surface would have refused the same caller.

The two halves are separable and both are required. A shim that reads a
native file and re-renders it in another system's column layout has
changed only the presentation, and the kernel's own gates still decide
what the caller sees. A shim that reaches state through a path its client
could not have used — a privilege it holds and its client does not, a
value supplied by the client itself, a fabricated plausible number — has
stopped reformatting and become an authority, whatever its output looks
like.

The failure mode is the confused deputy, and it is easy to reach by
accident because each individual step looks like an improvement: a file
that would otherwise be empty gets filled, a value that has no source
gets a reasonable default, a caller that wants to ask about another
process gets to name one.

## Enforcement

Structural rather than checked, which is the point of stating it this way.
**Three surfaces enforce it, one per layer of the phenotype**, and the
statement above was written when only the first existed:

- [[sub-viv]] **declares** it. A phenotype is a manifest annotation and
  never an inference — absent, or anything but `"linux"`, yields native —
  and the declaration lands on the container's entrypoint alone. viv holds
  no capability beyond the invoker's, so declaring a phenotype grants
  nothing.
- [[sub-kernel-vivarium]] **decodes** it. Every row's collision argument
  ends at the same place: what a mis-declared Proc reaches is its own
  memory and its own descriptors, bounds-checked, never authority. The
  `PRINCIPAL_SYSTEM` → 0 uid mapping is the sharpest instance — it changes
  what a guest is *told*, never what it may do, because every gate reads
  the real principal.
- [[sub-diorama]] **renders** it, and is the whole of its own design:

- **Every rendered byte derives from a native read.** The renderers open
  `/proc/<pid>/*` and `/ctl/*` through the ordinary namespace, so the
  kernel's existing gates run underneath unchanged — a read the kernel
  refuses the server is a read the server cannot serve.
- **The caller is never asked who it is.** `self` resolves through the
  kernel-stamped connection peer, and the alternative — letting a client
  name a pid — is deliberately not offered.
- **Read-only at the protocol edge.** Every write is refused before any
  renderer is reached, which removes the surface a mutable compatibility
  layer would carry.
- **The one file whose authority differs is absent, not gated.**
  `/proc/<pid>/environ` is owner-or-capability natively; the server runs
  as SYSTEM, so serving it per-pid would hand a client bytes the client
  would natively have been denied. It is served under `self` only, where
  the target is the caller's own process — and the absence is asserted
  rather than assumed.

The constants are the deliberate exception, and the discriminator is
written down: a value **derived from kernel state** needs a native source
without exception, while a constant **declaring which ABI the caller is
looking at** is the phenotype speaking about itself and carries no
information about the system at all.

## Validation

Prose plus the vivarium audit round, which prosecuted the claim directly
("does any phenotype bit confer authority anywhere?") and concluded it
holds structurally by exhaustive search. The server's own boot selftest
pins the negative half — a walk to `/<pid>/environ` must miss, and the
assertion says why in its failure string.

**blind-to:** whether a *newly added* file obeys it. The invariant is a
rule about provenance, and nothing mechanical checks the provenance of a
render — a new file that quietly reads something its client could not
would pass every test, because the tests assert shapes and the boot gate
asserts liveness. The defence is the rule being stated at the top of the
file and restated at each site that could tempt an exception, which is a
review property rather than an enforced one.

Also blind to the *aggregate* question. Each file is individually
derivable from a native source; whether the assembled set discloses more
than the sum — a timing or correlation channel across several honest
files — is not something the per-file rule can see.

Not enumerated in ARCHITECTURE section 28: the table runs I-40 then I-42,
and its closing paragraph accounts for I-41's absence without mentioning
this one (task #155).
