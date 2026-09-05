---
id: spec-cons-poll
type: spec
title: "cons_poll.tla"
models: [sub-kernel-cons]
pins: [inv-i9]
cfgs:
  - "cons_poll.cfg -- clean: every safety invariant, buggy flags FALSE"
  - "cons_poll_liveness.cfg -- Spec_Live: PollerEventuallyServed, the relay delivers"
  - "cons_poll_buggy_lost_wake.cfg -- BUGGY_MGR_LOST_WAKE: the relay strands a poller asleep on a ready console (NoMissedConsPoll counterexample)"
gate: "any change to the manager kthread's sleep, the deferred-flag protocol, or a new interrupt-context readiness source"
created: 2026-08-02
updated: 2026-08-02
---
## Abstraction

One poller, one console, one relay kthread. Readiness is monotonic within an
episode; the poller waits forever (no timeout), which is the sharpest form of
the obligation — a dropped relay strands it outright rather than merely
delaying it.

What the sibling models do not cover: the wake is **relayed through an
intermediary thread**. [[spec-scheduler]] proves one Rendez, [[spec-poll]]
proves N fds' register-then-observe and the hook lifetime, [[spec-tsleep]]
proves the deadline race. None of them has a producer that *cannot wake the
consumer at all*.

That producer is the console receive interrupt. It may not walk a hook list —
[[lock-poll-list]] is non-irqsave and nests a wake inside itself — so it sets a
pending flag and wakes the manager, which walks the list in process context.
This is Linux's tty shape: the hard interrupt buffers the byte and schedules
work; the cooking and the wakeups happen in that work item.

## What it pins

- **NoMissedConsPoll** — [[inv-i9]] across the deferral: a poller with a
  registered hook on a ready console is never left asleep with the relay
  quiescent. The composition is what is under proof, since each half is already
  proven separately.
- The **second register-then-observe obligation** the relay creates. The
  poller's own is [[spec-poll]]'s; the new one is the *manager's* — its
  go-to-sleep must be register-then-observe against the pending flag, or a flag
  set as it heads back to sleep is lost and the relay never fires again.

`SpuriousWake` is what makes the race reachable: the manager's Rendez has other
wakers (the interrupt-note and attention-key flags), and one benign wake is
what puts it in the *awake, about to re-sleep* state where the window opens. It
is capped to fire once — enough to expose the bug, bounded for the state space.

## What it cannot see

Only the *poll* relay. The console's other two deferred actions ride the same
flag-drain-then-act structure and the same kthread, and neither is modelled:
the interrupt-note post and the attention-key transition are prose plus
[[inv-i27]]'s test family. In particular the **batch supersede** — an attention
key and an interrupt coalescing into one service pass, which loses their
arrival order — is a policy decision inside the act phase, invisible here.

The second instance of the relay, for the renderer's output drain, is
structurally identical and equally unmodelled; it is the same proof by
construction.

Object lifetime is below the model, as in [[spec-poll]] — though the console is
the one case where that blindness does not bite, because its hook lists are
file-scope statics that outlive every poller.

## Binding

`specs/SPEC-TO-CODE.md::cons_poll.tla`: the interrupt-side flag set ↔
`cons_rx_input` under the console lock; the manager's register-then-observe
sleep ↔ `console_mgr_main`'s `sleep(&mgr_rendez, cons_mgr_pending)`; the drain
and walk ↔ `cons_service_deferred`; the poller's own register ↔ `cons_poll`.

The correct sleep is **one atomic step** in the model — the register-then-
observe holds the Rendez lock across enqueue and cond re-check; the buggy flag
splits it into observe-then-commit to open the window.
