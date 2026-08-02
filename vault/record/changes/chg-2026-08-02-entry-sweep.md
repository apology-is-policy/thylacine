---
id: chg-2026-08-02-entry-sweep
type: chg
title: "vault sweep: the EL0 entry boundary and its return tails"
date: 2026-08-02
arc: arc-vault
commits: []
touched:
  - sub-kernel-exception
  - sub-kernel-uaccess
established:
  - inv-i13
closed: []
opened:
  - seam-el0-irq-tail-no-notes
mirrors-checked: []
depth: skeletal
created: 2026-08-02
---
Batch 16. Read from code: `arch/arm64/vectors.S` (340), `exception.c` (544),
`userland.S` (132), `uaccess.S` (360), `uaccess.c`, `uaccess.h`, plus the second
`eret`-to-EL0 path in `context.S`, the note-delivery decision in `notes.c`, and
the terminate-wake in `proc.c`. Two dossiers under `system/kernel/entry/`, which
had been declared-and-empty since commit 0.

WHY THIS BATCH. The audit-trigger table's FIRST row, and the surface every
syscall, interrupt and fault in the system crosses. It is also the structural
counterpart to batch 15: the console defers work it cannot do in interrupt
context to a manager kthread; entry defers work it cannot do mid-handler to the
return tail. Same shape, different carrier.

THE ORGANIZING FACT is that **a privilege boundary is a set of MOMENTS, not an
instruction** -- entry (where the frame must land on the thread's own kernel
stack), the return tail (the only place with a clean frame, no locks held, and a
thread about to become interruptible again), and the deliberate crossing (a
designated instruction whose fault is recoverable). Almost everything odd about
this area is one of those three moments defending itself.

THE HEADLINE: NOTES ARE DELIVERED ON ONE OF THE TWO EL0 RETURN TAILS.

`notes_deliver_at_el0_return` has exactly ONE call site -- `vectors.S:334`, in
`.Lel0_sync_return`. The 0x480 EL0-IRQ slot runs `preempt_check_irq`,
`el0_return_die_check` and `el0_return_stop_check`, and does NOT call it. So a
Proc's note disposition is evaluated only on return from a syscall or a fault; a
thread that makes neither is never asked.

Three of the four EL0-return actions are on both tails. The fourth is on one.

The consequence is sharp because of WHICH three are on both. Against a
syscall-free compute loop: an outright kill works (group-exit state ->
die-check), Ctrl-Z works (job stop is applied post-side -> stop-check), a
debugger stop works (same). **Ctrl-C does not**, and neither does a registered
handler -- both live inside the delivery function, and the default-terminate
decision (`notes_terminate_note_name_locked`, one production call site at
`notes.c:905`) is INSIDE it.

The tree states the asymmetry itself, in `proc.c:3206`, explaining why the
stop's catchability gate is evaluated at post time: *"the stop -- unlike the
terminate -- is applied post-side, not at the tail."* True, and correct. What
was not followed through is that the tail it refers to is only one of two.

Nor is the terminate-wake a backstop: `proc_interrupt_terminate_wake` walks the
Proc's threads and wakes the BLOCKED ones so they unwind to their tail and die
there (`proc.c:1790`). A RUNNING thread needs no waking, so it is never touched.
The mechanism is complete for blocked threads and empty for running ones.

AND THE SCRIPTURE CLAIMS THE PROPERTY. ARCH section 8.8.2 and LIFE-SUPPORT.md
both state the composed result as **"Ctrl-C terminates any foreground command --
CPU-bound, output-bound, or blocked in sleep / read -- catchably."** Output-bound
and blocked are sleeps, so the terminate-wake covers them. CPU-bound has no
mechanism. The claim is composed of three legs and the third one is the gap.

MEASURED, NOT COUNTED BY EYE. The fix is not a one-liner, and the reason is a
budget: each vector slot is capped at 0x80 bytes = 32 instructions. Assembling
`vectors.S` standalone and disassembling gives the live occupancy --

    0x200 kernel sync : 27 / 32   (5 free)
    0x280 kernel IRQ  : 28 / 32   (4 free)
    0x400 EL0 sync    : 27 / 32   (5 free)
    0x480 EL0 IRQ     : 31 / 32   (1 free)

-- and note delivery costs two (`mov x0, sp` + `bl`). It does not fit. The
0x400 slot has five free while doing strictly MORE work precisely because it
branches to its own tail trampoline; 0x480 inlined its whole tail instead and
has run out of room. So the fix is: factor the tail out first, then add the
call. Tracked as [[seam-el0-irq-tail-no-notes]] (task #21).

The one-instruction headroom is worth recording on its own: the next addition to
the EL0-IRQ return path fails the BUILD (a backwards `.org`). Loud, not silent
-- but it means the file sits at a structural cliff.

AND THE COMMENT ALREADY CLAIMS IT IS DONE. `exception.c:336` says the hook "is
invoked from the vector tails (vectors.S .Lel0_sync_return for sync-from-EL0,
**0x480 for IRQ-from-EL0**)". The second half is false. This is the batch-15
shape again -- a statement that documents the intended end state of an arc
rather than the one that landed -- and here it is the exact statement a reader
would consult to decide whether the gap exists.

REACHABILITY, HONESTLY. Bounded today: anything on musl or the Go runtime
syscalls constantly and reaches the sync tail in microseconds. It takes a
deliberately syscall-free loop to hold the gap open. But that is precisely the
program Ctrl-C exists for -- and the tree has been here before. #810
(DEBUGGING-PLAYBOOK 6.14) was a hang whose real cause was that secondary CPUs
had no per-CPU timer, so **a CPU-bound EL0 thread on a secondary was never
preempted**. Arming that timer is what made the 0x480 tail reachable for a
spinning thread at all. The note hook was never added to it.

THE THIRD INSTANCE OF THE STALE-SUMMARY CLASS, AND THE CLEAREST. uaccess states
in THREE separate places that it provides one primitive: the header's
"Public surface" lists 2 of 6 and says *"At v1.0 only uaccess_load_u8 is
provided; SYS_PUTS is the sole consumer"* -- directly above six declarations;
the assembly's design note says *"We export a single primitive"*; and
`uaccess_fixup_lookup`'s comment says *"at v1.0 the table has one entry"*.
Counted mechanically: 10 fault points (4 scalar + 3 head/body/tail for each bulk
direction), 6 primitives. Nothing is wrong with the code -- every primitive is
correct, every table entry present -- but the prose describes the first version
of a file that has grown five times, and the linear-scan rationale ("the table
has one entry") is the load-bearing-sounding claim that is furthest from true.

Batch 15 found this in `cons.c`'s header block; this batch found it in
`uaccess`'s header, in `uaccess.S`'s design note, in `uaccess.c`'s lookup
comment, in `exception.c`'s tail comment, and in `docs/reference/08-exception.md`
whose vector table still lists BOTH EL0 slots as "unexpected (idx 8/9)" -- true
before userspace existed. Five in one batch. **The drift is not random: it is
always in the summarizing prose, and never in the comment beside the code.** The
per-slot and per-primitive comments in these files are current and unusually
good.

THE #713 RULE, STATED ONCE. Three paths `eret` to EL0. The shared trampoline is
always reached interrupt-masked (hardware masked on entry, nothing unmasks), so
it installs ELR/SPSR and erets in one masked instant. The other two --
`userland_enter` and `thread_user_trampoline` -- are reached with interrupts
ENABLED from a kthread, and each masks DAIF explicitly across the
ELR-set..`eret` window. Without it an interrupt in that window overwrites
ELR_EL1 with the interrupted KERNEL pc, and the eret lands EL0 at a kernel
address. Both also run their die-check BEFORE the mask, deliberately: the die
path is noreturn and must never be entered from inside the masked window. Stated
as one rule in the dossier's Prosecution, since it currently lives as two nearly
identical comment blocks in two files.

REGISTRY TAIL. Minted this batch's own dependency and stopped: [[inv-i13]]. Its
crossing half (the enumerated fixup table, the three-condition recognition, the
register sweep before each eret) has a swept guard here; its separation half
(the translation-table split, the root swap, the ASID) lives in the MMU, which
is unswept -- recorded as the note's `blind-to` rather than asserted.

**I-19 deliberately NOT minted**, though the headline finding is about its
delivery point: its `guards` home is `notes.c`, which is unswept, and minting it
now would recreate exactly the guardless-restatement problem that stalled the
batch-13 registry pass. It lands with the notes sweep, and the seam is the
pointer.

Added `sub-kernel-exception` to [[inv-i21]]'s guards: the two "current EL with
SP_EL0" vector slots are where the uniform-EL1h clause becomes mechanical --
they are unreachable under the model, so they are wired to the unexpected-vector
diagnostic and the violation is a loud extinction rather than a silent
wrong-bank stack write.

SCOPE. `fault.c` (928 lines) is NOT in this batch. It is dispatched from here
but its substance is demand paging, and the audit-trigger table draws the same
line -- "exception entry" and "page fault + COW + W^X" are separate rows. It
belongs to `memory/`, alongside the unswept burrow/vma/mmu group. `context.S`
was read for `thread_user_trampoline` but stays owned by
[[sub-kernel-sched-smp]], which holds it for `cpu_switch_context`.
