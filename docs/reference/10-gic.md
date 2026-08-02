# 10 — GIC driver [ABSORBED INTO THE VAULT]

This document was absorbed at the interrupt-and-time sweep
(`chg-2026-08-02-devices-interrupt-time-sweep`). Its content now lives,
code-verified and current, in the dossier:

    vault/system/kernel/devices/sub-kernel-gic.md

(version detection and why version and addresses must come from one node, the
three-stage bring-up on each generation and the orderings the architecture
forces, the per-CPU contract on enabling and secondary bring-up, dispatch and
its per-CPU counter, the completion echo, inter-processor interrupts and their
bounds check, and the edge configuration a lent interrupt needs.)

**What this file got wrong is a failure mode the other two absorbed documents
do not show: it was updated for a feature's *presence* but not its
*substance*.** The headline is accurate, the identifiers are right, the older
generation is named eleven times, and the summary correctly describes it as
reaching its CPU interface through memory-mapped registers rather than system
registers. Nothing here is false.

But the mechanisms are absent — and specifically the ones that make the older
generation genuinely different to drive rather than merely differently spelled:

- **The completion echo.** Acknowledging on the older generation returns a word
  whose next three bits, for an inter-processor interrupt, identify the
  *sending* CPU, and completion must write that same field back. This is the
  reason a per-CPU slot exists to hold the raw acknowledgement between the two
  calls, and the reason that slot needs no lock. Zero mentions.
- **The active-state clear at bring-up.** Enable and pending are cleared;
  so is *active*, because firmware or a previous kernel may have left an
  interrupt half-delivered. Zero mentions.
- **Byte-wise priority writes**, so that a concurrent update to a neighbouring
  interrupt in the same word is not lost. Zero mentions.
- **The runtime line-count bound**, read from the implementation at bring-up
  and used to reject interrupt numbers past what the hardware actually has —
  writes beyond it are undefined. Zero mentions.
- **The barrier after edge configuration**, without which a strict
  implementation could process the following enable first and deliver the first
  interrupt as level-sensitive, which for a source nothing deasserts is an
  unrecoverable storm rather than a wrong value. Zero mentions.
- **The per-CPU dispatch counter**, added later to feed process statistics, and
  deliberately counted at the point every interrupt passes rather than reusing
  the narrower count of interrupts forwarded to driver processes. Zero mentions.

So a reader who needs to know *whether* the older generation is supported is
well served, and a reader who needs to *change* the driver learns nothing about
what is hard. That is the specific gap: the presence of a feature is a fact
about the project, and its mechanism is a fact about the code, and only the
first was written down.

**What it got right and the vault kept:** the version-detection order and the
compatible strings, the shape of the three-stage bring-up on both generations,
the affinity-routing enable preceding the routing writes, and the bounded wait
for the per-CPU interface to acknowledge its wake.

The invariants live at `vault/invariants/inv-i15.md` (which now records that
this is the one device where the tree selects the *driver*, with no fallback)
and `inv-i18.md` (whose statement this sweep sharpened: the pending state is a
bitmap, so the invariant holds vacuously with one interrupt and breaks on the
second, whether or not it carries a payload). The open debt is
`seam-gic-handler-slot-never-cleared` (no task). Design scripture is unchanged:
`ARCHITECTURE.md section 12.3`, `PORTABILITY.md section 5`, ARM IHI 0069 and
IHI 0048B.
