---
id: chg-2026-08-03-notes-sweep
type: chg
title: "note delivery -- a second dormant declaration, and a justification that is right for the wrong reason"
date: 2026-08-03
arc: arc-vault
commits: []
touched:
  - sub-kernel-notes
  - inv-i19
  - moc-kernel-ipc-wake
established:
  - sub-kernel-notes
  - inv-i19
closed: []
opened: []
depth: skeletal
created: 2026-08-03
---
Batch 30, the second sweep off the census: note delivery --
`kernel/notes.{c,h}` + `kernel/devnotes.c`, ~1,900 lines, holding **I-19**.
Main unmoved at `c0c76977`; L-1 absent on the EIGHTEENTH check. One dossier and
[[inv-i19]], which did not exist for the reason I-12, I-20 and I-40 did not:
the enforcement was unread.

**F1 -- THE SECOND DORMANT DECLARATION IN TWO BATCHES.**
`NOTE_MASK_SUPPORTED` (`notes.h:139`) names the set of meaningful mask bits and
carries a comment describing mask-validation policy. It has **zero consumers**.
`sys_note_mask_handler` stores its argument verbatim -- no filter, no
validation.

The behaviour the comment describes -- *"bits outside this mask succeed (we
tolerate unknown bits -- they just have no effect)"* -- is real, but it is
produced by **nothing ever consulting the mask for an unknown bit**:
`notes_peek_locked` only tests bits that the name lookup returned, and that
function only ever returns 0..5. The constant describes an outcome it does not
cause.

It is also unpinned against the `NOTE_BIT_*` definitions it summarizes (no
assert ties `0x3f` to the six bits), and it **already over-claims by one**: bit
4 is "supported" while having no entry in the live name table -- documented as
reserved, but the constant does not know that. Task #61.

Which is [[chg-2026-08-03-mapping-core-sweep]]'s F1 again one file over: a
declaration that reads as enforcement, enforces nothing, and is not pinned to
the thing it claims to summarize. Two consecutive sweeps, two dormant
constants, found by the same question -- *who calls this?*

**F2 -- A JUSTIFICATION THAT IS RIGHT FOR THE WRONG REASON.** The dispatcher
pushes the note name onto the **user** stack while holding the queue lock. That
write can fault, and a fault can demand-page. The source justifies it:
*"uaccess_store_u8 faults route through `p->vma_lock` (acyclic with q->lock);
buddy is non-blocking at v1.0, so holding q->lock through uaccess is safe."*

The lock-order half is right. The non-blocking half is right about the wrong
arm. Since REVENANT, one demand-page arm **does** block -- the file-backed one,
on a 9P round trip. What actually excludes it here is that file-backed regions
are read-only, so a *write* can never reach that arm; the stack is anonymous.
The stated reason (allocator non-blocking) covers the lazy-anonymous arm only.

Sound today. The concern is durability: [[sub-kernel-fault]] records
anonymous-copy-on-write data as a v1.x seam, and the day a writable
file-backed mapping exists, **the comment still reads as valid while the
property it names has quietly become false**. The tree has already met this
exact class once -- the REVENANT audit found a futex `uaccess` under a spinlock
newly sleeping on a file-backed page, and concluded by sweeping for others.
This site passes that sweep for a reason not written down. Recorded as a caveat
rather than a task: nothing to fix, something to know.

**THE COUNTERWEIGHT IS THE FILE ITSELF.** Almost every hard edge here is a
healed audit finding, still visible at the line it fixed, and the pattern of
them is instructive:

- **Peek and pop must share one lock hold** -- split, and the other delivery
  path steals the note.
- **Pop and push must share it too** -- split, and a failed user-stack push
  loses the note.
- **The stack-pointer bound must come first** -- it was moved ahead of the
  queue checks so a bogus pointer cannot cause a missed delivery *decision*.
- **`kill` must be scanned before the mask AND skipped by the fd path** -- two
  asymmetries, two separate rounds, because a handler that was merely *stuck*
  once made a Proc kill-immune.
- **A failed `kill` re-enqueue extincts**, deliberately: losing a kill the
  poster was told succeeded is worse than crashing.

Each is the same lesson in a different position -- **a two-step that must not
be a two-step** -- and the file now reads as a catalogue of the ways one queue
with two consumers can lose an entry.

Two more worth naming. The `tty:` prefix gate is **one string comparison
holding an [[inv-i39]] boundary**: those names are in the deliverable table, so
without the gate `tty:cont` is postable through the ordinary parent-to-child
path and an unprivileged parent could resume a debugger-stopped child. The
source labels it load-bearing and contrasts it with the `snare:` gate, which is
belt-and-braces over a name the table already rejects. **The same mechanism,
one line apart, doing two entirely different amounts of work -- and the code
says which is which.** That is the opposite of this arc's usual finding.

And the two 32-byte structs are pinned **to each other**, not merely each to
32. That third assert is precisely the guard the `t_stat` family (task #43)
does not have.

**PATTERN, SEVEN BATCHES.** b24 assertions pin values not their description;
b25 models pin mechanisms not their own scope; b26 each copy pinned to itself
not to the others; b27 the guard travelled but not its reason; b28 the ledger
pins the areas not the areas to the tree; b29 the enforcement list names a
guard that cannot fire; **b30 the same, plus a justification whose stated
reason and real reason have quietly diverged.**

F2 is a new variant worth separating from the rest. Every earlier finding was
about a guard being narrower than its claim. This one is about a **claim that
is true, with a reason attached that is false** -- which is more durable
camouflage, because verifying the claim confirms the sentence and leaves the
reasoning unexamined.

LEDGER. Corpus 803 -> **805**. Coverage 139 -> **142 owned of 421 (33%)**;
`kernel` 42 unowned -> 39. Invariant notes gain [[inv-i19]] -- and it is the
first invariant swept here whose statement is **five clauses each carrying a
documented exception**, which is the honest shape of it, not a weakness.
