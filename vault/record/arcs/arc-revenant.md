---
id: arc-revenant
type: arc
title: "REVENANT — demand-paging a binary from the filesystem, and what that did to every lock that touches user memory"
status: complete
design: ["docs/EXEC-LOAD-DESIGN.md", "docs/ARCHITECTURE.md 6.5"]
chunks: []
created: 2026-08-03
---
## Goal

Stop copying whole binaries into memory at exec. Before this arc, exec slurped
the entire ELF into a kernel buffer bounded at 1 MiB — so a binary larger than
that could not run at all, and every binary paid its full size in eager
allocation whether or not it ever touched those pages.

After it: the kernel reads 16 KiB of header, maps read-only segments **file-backed**,
and pages them in on demand, sharing one copy across every Proc running the
same binary. The proof it worked was booting an unstripped 1.11 MiB binary that
the old path could not have loaded.

## Shape

- **R-1** — `BURROW_TYPE_FILE`: a memory object backed by a kernel-pinned
  Spoor rather than by anonymous pages.
- **R-2** — the fault arm ([[sub-kernel-fault]]): read one page, install once,
  and — the genuinely new requirement — remain interruptible by death and fail
  closed on I/O error.
- **R-3** — the [[sub-kernel-image]] cache, built with no consumer.
- **R-4** — [[sub-kernel-exec]] rewired onto both, retiring the slurp.
- **R-5** — the focused audit.

It established [[inv-i36]], and it is the arc that made the tree's standing
refusal precise: no userspace writable file mapping, ever; kernel-internal
read-only paging from an immutable snapshot, under seven conditions.

## What made this arc interesting

**Two independent prosecutors caught different P1s, and neither was in code
this arc wrote.**

The sharper one: `torpor`'s futex path loaded a user word while holding a
global lock. That had always been safe, because every arm of demand paging
completed without sleeping. R-2 added an arm that sleeps — on a 9P round trip
to the filesystem — and the day text became file-backed, a futex word on a text
page could stall the entire system's futex machinery behind a disk read. The
code that broke was written years earlier and was not touched.

The other: the page-in issued a single read and trusted it to return a full
page. A short reply corrupts an interior page of text.

So the arc's real lesson is about **blast radius rather than about exec**:
making one memory arm able to sleep changed the safety argument of every
`uaccess`-under-lock site in the kernel at once. The close swept for others and
found the futex one; a site that survives that sweep survives it *for a reason*,
and the reason had better be written down.

It is not always written down. [[sub-kernel-notes]]'s dispatcher pushes a note
name onto the user stack under the queue lock and justifies it as "the allocator
is non-blocking" — true, and not the reason it is safe. The reason is that
file-backed regions are read-only, so a *write* never reaches the sleeping arm.
Anonymous copy-on-write data is a recorded seam of this same arc, and the day it
lands, that comment will still read as valid.

## The tail

**#45** widened the shared path from text to every non-writable segment,
because roughly half a Go binary is read-only data. Its audit added the
executability field to the Image key — two `PT_LOAD`s over an identical file
window with different X bits would otherwise have shared one Burrow at two
protections, defeating an instruction-cache sync that is gated on
executability.

**The read-ahead cluster** followed from the Go toolchain: one 4 KiB fault
against an 8 MiB filesystem extent is a bad trade, so a fault now pulls up to
64 pages in one batched read and degrades to the single-page path on any
shortfall.

## What it left

The design document is `docs/EXEC-LOAD-DESIGN.md`. Five source files cite it as
`docs/REVENANT.md`, six times, with section numbers that resolve — against the
other name. No file has ever existed under the name the code uses (task #64).
The arc's name outlived the document it was going to be written in.
