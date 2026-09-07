---
id: chg-2026-09-06-exception-doc-absorb
type: chg
title: "absorb docs/reference/08-exception (P1-F/G vector table + fault machinery): zero-fold, 4-surface redirect stub; the dossiers are far ahead of the Phase-1-era doc"
date: 2026-09-06
arc: arc-vault
commits: ["4a49f288"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
The P1-F/P1-G exception-handling reference (425 lines) -- rich, but Phase-1-era
and comprehensively superseded. Verified atom-by-atom against four owning
dossiers before stubbing; the soundness-critical trio (#713/#157/#107) got the
most attention, including reading the CURRENT arch/arm64/userland.S rather than
trusting the doc's #157 narrative.

WHERE EACH ATOM LIVES (all AHEAD of the doc):
- vector table (16 slots, 4 live), struct exception_context (288B + offset
  asserts), KERNEL_ENTRY/EXIT + .Lexception_return, the ESR/FAR sync handlers,
  the IRQ handler, exception_unexpected, exception_init, the PA/VA helpers, the
  #107 EL0-return tails (preempt->die->notes->stop, I-24/I-39), the #713
  eret-window mask rule (I-13 sweep), the descent/recursion guard, the
  hardware-debug classes -> sub-kernel-exception. It even self-documents that the
  doc's vector table is stale (both EL0 slots listed "unexpected").
- R12-uaccess kernel-mode user-VA fault recovery (the .uaccess_fixup table,
  userland_demand_page, retry-vs-fault, alignment-not-recoverable; = 40-uaccess)
  -> sub-kernel-uaccess.
- the uniform-EL1h model (I-21; = 67-el1h-kernel) + context.S
  thread_user_trampoline (the #713 second trampoline) -> sub-kernel-sched-smp.
- the extinction/ELE primitive (= 04-extinction) -> sub-kernel-halls.

VERIFY-BEFORE-FOLD CATCH (the sharp one): the doc's #157 SPSel section describes
the P4-Fix157 `msr SPSel,#0; mov sp` dance and asserts "the kernel's normal-mode
steady state is SPSel=0". Reading the CURRENT arch/arm64/userland.S showed P5-el1h
REPLACED that: the kernel now runs uniformly at SPSel=1 and userland_enter writes
the non-current bank directly (`msr sp_el0, user_sp`), so the dance is gone. The
dossier is correct (uniform EL1h); folding the #157 dance would have imported a
DEAD mechanism. #713's DAIF mask (msr daifset,#0xf across the ELR-set..eret
window) IS still live and IS carried by the dossier's "one rule they all obey" +
Prosecution.

ZERO fold: every atom is carried, more currently, by an existing dossier (the
19-handles/21-elf "stale milestone doc superseded by a more-current dossier"
vein, one tier richer). No I-12 fold: the exception handler only DIAGNOSES a W^X
kernel-image fault; the ENFORCER is the PTE constructors (mmu), so I-12 correctly
is not claimed in the exception dossier.

WHAT THE DOC GOT WRONG (named in the stub): both EL0 vector slots listed
"unexpected" (live since userspace); the #157 SPSel dance (superseded by P5-el1h);
the "Not yet implemented / Phase 2" list is largely built (recoverable faults,
EL0 entry, the #107 tails); #713 is current but the doc is its historical
write-up (the AEGIS-256 ghost-hunt narrative belongs to the audit/chg record).

Render clean; lint 0-fail. view-absorption 72 -> 73.
