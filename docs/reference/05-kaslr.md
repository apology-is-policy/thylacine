# 05 — KASLR [ABSORBED INTO THE VAULT]

This document was absorbed at the boot sweep (`chg-2026-08-02-boot-sweep`).
Its content now lives, code-verified and current, in the dossier:

    vault/system/kernel/boot/sub-kernel-kaslr.md

(the three entropy sources in priority order and why the source is reported,
the avalanche mix, the slide choice and its never-zero clamp, the relocation
walker, the canary seeding and the stack-protector exemption it forces, and
the `volatile` on the cached load-address bounds — the fix for a specific
observed miscompilation, not defensive typing).

**What this file got WRONG by the time it was absorbed** (the reason the
dossiers are written from the code): **every number in its parameters section
is one generation stale.** It quotes a 4 MiB alignment, an alignment-bits
value of 22, a mask of `0x3FFC00000`, and "12 bits of entropy = 4096 distinct
offsets". The tree has 8 MiB, 23, `0x3FF800000`, and 11 bits / 2048 offsets.
The alignment was widened again when a sanitizer build's image outgrew the
page-grain kernel mapping — the same reason it had been widened the time
before, which this document describes.

It also cites a compile-time assertion in `mmu.c` pinning the alignment at
`>= 22`; the assertion says `>= 23`. And it does not mention the *second*
assertion beside it, which pins the alignment against the mapped span
directly rather than against a number — the self-maintaining form, which stays
correct the next time the span widens. The tree learned the better form; this
document records the older one.

**The entropy figure is a small case study in how a number drifts.** Across
the record it currently appears as 13 bits (pre-widening), 12 (here and in one
`REFERENCE.md` entry), a "future bump to 17", a separate row promising 18 —
and 11 in the code. Five values, four documents, one quantity. The dossier and
`vault/invariants/inv-i16.md` state the durable fact instead: the figure is
the accumulated cost of image growth rather than a security calculation, and
each future widening spends another bit.

The invariant lives at `vault/invariants/inv-i16.md`. The open debt is
`seam-kaslr-link-va-unchecked` (task #24) — the link-time base is duplicated
between the C header and the linker script, and both documents claim a
build-time cross-check that neither performs. Design scripture is unchanged:
`ARCHITECTURE.md section 5.3`, `section 6.2`, `section 24`.
