# 08 — Exception handling [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-exception-doc-absorb`).
The ARM64 exception vector table and the fault/interrupt/EL0-entry-and-return
machinery behind it. This is a P1-F/P1-G-era document, partially updated (a
P5-el1h Status note) but Phase-1-shaped throughout, and comprehensively
superseded by the current dossiers — which the owning dossier itself flags (it
carries a "the reference document's vector table is stale" caveat). Its content
is carried, more currently and more completely, by:

- the **exception entry/exit machinery** — the 16-slot vector table (four live:
  kernel sync + IRQ, EL0 sync + IRQ), the saved-frame `struct exception_context`
  and its size/offset `_Static_assert`s, `KERNEL_ENTRY`/`KERNEL_EXIT` + the shared
  `.Lexception_return` trampoline, the ESR/FAR-decoding synchronous handlers (an
  EL0 fault terminates the Proc; a kernel fault extincts), the IRQ handler, the
  EL0 **return tails** (#107: preempt -> die-check -> notes -> stop, and why that
  order is I-24/I-39 load-bearing), the **eret-window mask rule** (#713: any
  hand-rolled `eret` to EL0 must mask across the whole link-register-set-to-`eret`
  window), the register-sweep-vs-restore rule (I-13), and the recursion/descent
  guard:

      vault/system/kernel/entry/sub-kernel-exception.md

- the **kernel-mode user-VA fault recovery** (the doc's R12-uaccess arm, i.e.
  docs/reference/40-uaccess) — the `.uaccess_fixup` table, `userland_demand_page`,
  the retry-vs-fault-out argument, and why alignment faults are *not* recoverable:

      vault/system/kernel/entry/sub-kernel-uaccess.md

- the **uniform-EL1h model** (the doc's P5-el1h Status arm, i.e. docs/reference/
  67-el1h-kernel + invariant I-21) **and the `thread_user_trampoline` EL0-entry
  path** (`arch/arm64/context.S` — the second of the two `eret` trampolines the
  #713 mask rule governs):

      vault/system/kernel/scheduling/sub-kernel-sched-smp.md

- the **extinction (ELE) primitive** the handlers terminate with (the doc's
  see-also to 04-extinction):

      vault/system/kernel/entry/sub-kernel-halls.md

**What this file got WRONG or MISSED by the time it was absorbed** — its Status
note admits it "is otherwise Phase-1-era and stale on axes unrelated to this
update", and the drift is exactly the "oldest summary lags the current per-slot
source comments" pattern:

- **The vector table lists both EL0 slots as "unexpected".** That was true before
  userspace existed and has been wrong since the EL0 sync (`0x400`) and EL0 IRQ
  (`0x480`) paths went live — they are two of the four LIVE slots now. The
  dossier's own Caveats flag this exact staleness.
- **The #157 `SPSel` section describes a mechanism P5-el1h REPLACED.** It says
  "the kernel's normal-mode steady state is `SPSel=0`" and the fix is
  "`msr SPSel, #0; mov sp`" — but P5-el1h made the kernel run **uniformly at
  `SPSel=1`**, and `userland_enter` now writes the non-current bank directly
  (`msr sp_el0, user_sp`), so the whole `SPSel` dance is gone (`arch/arm64/
  userland.S` says so in-source). The doc's Status note flags the P5-el1h
  reversion, but the #157 *body* still walks the superseded dance.
- **The "Not yet implemented / Phase 2" list is largely built.** Recoverable sync
  faults (the page-fault + demand-paging path via uaccess), userspace exception
  entry, and the EL0-return tails (#107) all landed; the doc predates them.
- **#713 is current, but this file is its historical write-up.** The eret-window
  mask *rule* and mechanism live in the dossier; the ghost-hunt narrative — the
  "AEGIS-256 corruption", "why it hid for a year", the stratumd worker thread —
  is history and belongs to the audit/chg record, not the as-built reference.
