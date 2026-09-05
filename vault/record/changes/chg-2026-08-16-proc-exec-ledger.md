---
id: chg-2026-08-16-proc-exec-ledger
type: chg
title: "Enumerate the exec reset by who re-arms, not by what looks like it belongs"
date: 2026-08-16
arc: arc-vault
commits: ["b964652a"]
touched: [sub-kernel-proc, sub-kernel-jobctl, sub-kernel-death, sub-kernel-caps]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-08-16
---
The exec path in the process file had no dossier coverage at all — its substance
was recorded on the phenotype surface (the disposition table) and nowhere as a
property of image replacement itself. Filled in, plus the co-tenancy dispositions
for the three dossiers the same churn flagged.

## The exec ledger, and the axis to enumerate it on

Image replacement swaps a live process's address space in place, and everything
it clears follows one sentence: **the image is gone, so anything holding an
address into it, or a disposition installed by it, is now a pointer into someone
else's program.**

Three things are reset. Two of them share a failure mode the third does not, and
the difference is the useful part.

The registered handler entry points go because their addresses were the old
image's — ordinary staleness, and the file-descriptor-shaped delivery path
survives precisely because it names no address.

**The hardware debug slots are the instructive case.** Nothing else disarms them
— the debug state lives until the process is freed — and the context-switch path
**re-arms them unconditionally** from their stored counts at every switch. So a
slot left set is not merely stale: it is *actively re-installed into the new
image, forever*, and it fires on whatever now occupies that address, delivering a
stop in a program the debugger never set a breakpoint in.

**The state that bites at exec is the state some other mechanism restores without
asking.** A field nobody touches again decays quietly. A field a periodic path
rebuilds from a count is re-armed on a schedule. So the right enumeration axis
for an exec path is **who re-arms this**, not *what looks like it belongs to the
image* — the second reading finds the handler addresses and misses the slots,
because the slots do not look like image state at the point where they are
restored.

The attachment relationship deliberately survives, matching the reference
system's behaviour of clearing the slots and keeping the tracer. So the reset is
per-image, never per-relationship — a distinction that is easy to collapse in the
direction of doing too much.

## The gate that bounds threads and was read as bounding processes

`proc_exec_alone` establishes that the caller is the only live thread of its
process, and the exec path re-checks it under the table lock at the swap.

It says nothing whatever about **other processes** — and a comment claiming a
single reader for the disposition table stood on it for the life of the feature.
The full account is on the phenotype surface; what belongs here is the boundary,
stated as a prosecution rule, because the gate is genuinely correct for the
same-process readers and will keep being reached for.

Two further precisions the audit round added, both about not overclaiming:

- The reset is **not a snapshot**. A lock-free reader on another processor can
  see a mix of pre- and post-reset entries. That is sound — every entry is one
  genuinely installed or the default — but it is a per-field guarantee, and the
  first version of the comment described reader-set growth as simply "safe by
  default", which is more than was bought.
- **The helper's precondition is unenforced, and the reason is a test.** The
  production caller extincts on a non-self target; the split-out helper does
  not, because the kernel test drives it on a process it built and never
  scheduled. Stated in the header rather than hidden — the right disposition —
  but the practical effect is that the check protecting the production path
  does not protect a second caller, and a second caller is exactly what a
  split-out, header-declared helper invites.

## Three dossiers flagged for someone else's work

The same churn flagged the job-control, death and capability surfaces, all
co-tenants of one file. Checked by hunk context against each dossier's own
function set: sessions and process groups untouched, the termination cascade
untouched, no capability token in any hunk.

For the capability surface this is the **second consecutive interval** in which
its staleness was borrowed rather than earned, and the previous one is already
recorded there.

That repetition is the thing worth naming. **A churn signal keyed to files cannot
distinguish a surface that changed from a surface that shares an address**, and a
dossier flagged twice running for a neighbour's work is one a reader starts
skipping — which costs exactly the attention the list exists to direct. Recording
the disposition each time, with the check that produced it, is cheaper than
either re-deriving it or letting the entry go permanently red.
