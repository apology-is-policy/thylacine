---
id: fnd-pouchb0-r4-f3
type: fnd
title: "an IRQ-edge ABBA between the console hook lists and g_cons.lock: the list lock was plain, nested under an interrupt-taken lock, and held with IRQs on by console_mgr"
round: adt-pouchb0-r4
severity: P1
status: fixed
surface: [sub-kernel-poll, sub-kernel-cons]
threatens: [inv-i9]
fixed-by: chg-2026-09-21-srvconn-two-endpoint-poll
regression: "none deterministic (the interleaving needs an RX IRQ inside a list hold on one CPU while another holds g_cons.lock); the guard is the comment at the list ops, the poll audit row, lock-poll-list, and the SMP gate"
created: 2026-09-21
---
## Prosecution

**File**: `kernel/poll.c` (`poll_waiter_list_*` took `spin_lock`), `kernel/cons.c` (`cons_poll` registers under `g_cons.lock`, irqsave; console_mgr walks the list as a kthread with IRQs on)
**Invariant**: I-9 (liveness of the whole guest)
**Prosecution**:
1. CPU0: console_mgr holds `g_cons.poll_list.lock` (plain, IRQs on) mid-walk.
2. CPU1: `cons_poll` holds `g_cons.lock` (irqsave) and spins on that list lock.
3. A UART RX IRQ lands on CPU0 (which takes every SPI): `cons_rx_input` spins on `g_cons.lock`. CPU0 is dead in IRQ context, CPU1 IRQ-masked: a wedge. Same for `g_cons_drain.lock`.
**Suggested fix**: irqsave the list lock (LS-8's "acyclic" order ignored the IRQ edge).

## Disposition

Fixed: every list op irqsave. Pre-existing since LS-8. The vault lock note had said "never widen this lock to irqsave"; the half that stands is that no IRQ handler WALKS a list (O(pollers) work) -- the lock is masked for the nesting, not to license an IRQ walk.
