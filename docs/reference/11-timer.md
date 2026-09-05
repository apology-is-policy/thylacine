# 11 — ARM generic timer [ABSORBED INTO THE VAULT]

This document was absorbed at the interrupt-and-time sweep
(`chg-2026-08-02-devices-interrupt-time-sweep`). Its content now lives,
code-verified and current, in the dossier:

    vault/system/kernel/devices/sub-kernel-timer.md

(why the virtual timer rather than the physical one, per-CPU arming and why a
CPU that misses it fails silently, the boot-CPU-only tick counter, the
single-shot arm that lets an idle CPU stop ticking, the clamp contract and its
two reasons, the overflow-avoiding time conversions, the single-word wall-clock
offset that needs no lock, and the real-time clock chip's two-sided
plausibility window.)

**What this file did that is worth naming as its own failure mode.** It is not
stale in the usual sense — it *knows* it is wrong, says so, and asks the reader
to compensate. A note near the top records the switch from the physical timer
to the virtual one and then adds:

> "**The mechanism described below is identical with the `V`-variant
> registers** (some interior pseudocode still names the `CNTP_*` forms — read
> them as their `CNTV_*` counterparts; the headline facts here are
> authoritative)."

The physical-timer register names appear on **twenty-four lines** below that
note. So the document delegates its own correction to the reader, twenty-four
times, in the section that exists precisely to be copied from. A reader who
follows the instruction gets the right answer; a reader who greps does not, and
neither does one who reads a single section without having read the preamble.

That is worse than ordinary staleness in one specific way: staleness is
discovered, and this is *agreed to*. There is no point at which anyone notices
a discrepancy, because the discrepancy is documented policy.

The same incomplete rename is visible in the code: the timer source file's own
header still describes its reload arithmetic in terms of the physical timer's
registers, in a file where every actual access is virtual. The dossier records
that as a caveat.

**Also absent**, because the scope is P1-G plus the switch: the entire
single-shot path that lets an idle CPU stop taking timer interrupts — which is
the change that took an idle machine under the hypervisor from a third of a CPU
spent on nothing to approximately none — and the wall-clock offset with its
runtime setter, whose one-word design is the reason a clock step is safe on a
multiprocessor without a lock. Both are substantial and both post-date this
file.

**What it got right and the vault kept:** the reason for the virtual timer (the
physical one is hypervisor-reserved and a guest write comes back as an
undefined instruction), and the observation that the physical *counter* remains
readable for entropy while only the physical *timer control* is reserved.

**What was NOT absorbed, and is therefore owed** (found at the ledger
reconciliation, `chg-2026-08-02-absorption-reconciliation`): this document also
carried the only account of **the vDSO clock page** — `kernel/vdso.c`, its
shared-page layout, and the magic-and-version handshake that lets a reader
detect a mismatch and fall back to the syscall. The dossier covers the timer
and the real-time clock; the page that publishes their reading to userspace
without a trap is in neither it nor any other note. Tracked as task #32.

The invariants live at `vault/invariants/inv-i15.md` and `inv-i17.md`. The wall
clock's settability, once a recorded gap, is closed. Design
scripture is unchanged: `ARCHITECTURE.md section 22.6`, `PORTABILITY.md section
5`, ARMv8-A ARM section D11.
