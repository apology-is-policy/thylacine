---
id: spec-pty
type: spec
title: "pty.tla"
models: [sub-ptyfs, sub-kernel-proc]
pins: [inv-i20, inv-i9]
cfgs:
  - "pty.cfg -- clean: the four atomicity legs of I-20 composed"
  - "pty_liveness.cfg -- EventuallyDrained: a cooked byte is eventually readable"
  - "pty_buggy_signal_also_byte.cfg -- SignalXorByte violated: the ldisc failed to swallow a control char"
  - "pty_buggy_lost_teardown_byte.cfg -- RingConserved violated: master close discards the ring instead of draining it"
  - "pty_buggy_signal_wrong_pgrp.cfg -- SignalToFgOnly violated: the server escapes its pts's foreground group"
  - "pty_buggy_double_stop.cfg -- StopCompatI39 violated: a job resume clears a debug stop"
gate: "any change to the ldisc's byte accounting, the drain-then-EOF order, or the signal-routing seam"
created: 2026-08-02
updated: 2026-08-02
---
## Abstraction

Two counters and two flags. `m2s` is the master's cooked input toward
the slave; `s2m` is the slave's output plus the echoes toward the
master. Each side reads the ring the other fills, and sees EOF when the
other closes.

Everything interesting is in what a single master input char *becomes*.
A normal char becomes exactly one ring byte (plus an echo if ECHO); an
ISIG control char becomes exactly one signal and **no** ring byte. The
model's central clause is that identity:

```
SignalXorByte == sigCount + dataProduced = Consumed
```

Every consumed char is accounted for as precisely one of the two, never
both and never neither. `BUGGY_SIGNAL_ALSO_BYTE` is the ldisc that
raises the signal and *also* enqueues the `^C` — the natural bug, since
"swallow the byte" is a `continue` someone can forget.

The signal-routing half is modelled as a seam rather than a mechanism:
`SignalToFgOnly` says a `wrongPgrp` flag is never set, and the flag can
only be set by a buggy cook. That is honest about where the property
actually lives — the server names only `(pts_id, class)` and the kernel
resolves the foreground group, so the server has no way to name a wrong
one. The model pins the consequence of a seam it does not contain.

`RingConserved == dataProduced = m2s + slaveRead` is the teardown
clause: a cooked byte is either still in the ring or was read, so a close
must *drain* rather than discard. `HupAtMostOnce` is the carrier-loss
counter.

## Action-site map

| Action | Site |
|---|---|
| `CookData` / `CookSignal` | `ptyfs server.rs::Ptys::master_write` — the per-byte input cook |
| `SlaveWrite` | `Ptys::slave_write` — the ONLCR output cook, pair-atomic |
| `SlaveDrain` / `MasterDrain` | `Ptys::slave_read` / `master_read` → `ring_drain` (Data while non-empty **regardless of the peer's closure**) |
| `SlaveSleep` / `MasterSleep` | `Conn::h_read`'s `WouldBlock` park; `Conn::poll_reads` at the serve-loop top |
| `CloseMaster` / `CloseSlave` | `Conn::close_endpoint` at every opened-endpoint drop |
| `StopJob` / `StopDebug` and their resumes | the kernel — and modelled properly next door in [[spec-pty-stop]] |

## Where the model stops — and it is not where it sounds like

The module header calls leg (1) *"no byte lost/torn/duplicated across
the cook"*, and `RingConserved` reads like a whole-cook conservation
claim. It is not one.

`CookData` is guarded `m2s < CAP`: in the model, a cook onto a full ring
simply **does not happen**. That is back-pressure, and it is exactly what
`master_write`'s *raw* arm does — `ring_push` returning 0 breaks the loop
without consuming, so the short `Rwrite` makes the writer retry.

The *cooked* arm does the opposite, deliberately. A byte past `LINE_MAX`
is consumed and dropped un-echoed; a line flush into a full `m2s`
discards the tail. `echo()` drops on a full `s2m` unconditionally, where
the model guards `ECHO => s2m < CAP`. Those are the classic tty-overrun
semantics, the kernel console does the same, and the code says so
plainly in the docstring of the very function the map points at.

So the model proves conservation for one of the two arms and is silent
about the other — and the map's `CookData` row asserts the stronger
thing, that *"every consumed non-signal byte is ring data (assembled-
then-flushed or raw)"*. The parenthetical even names the assembly whose
overflow breaks it.

Nothing here is wrong in the code. What is wrong is the inference
available from the model's greenness: one spec action points at one
function with two branches of opposite overflow behavior, and a
one-row-per-action map has no way to say which branch it modelled.
Tracked as task #48.

## Beneath the model

The stop-ownership algebra ([[spec-pty-stop]] models it properly);
ICANON assembly, erase, and ICRNL/ONLCR as *transforms* (the model
counts bytes, it does not transform them); the ctl grammar's
tcsetattr-atomicity; the fid and refcount discipline that keeps a pts
alive; and `HupAtMostOnce`'s actual argument, which is not a counter at
all but a four-link structural chain in [[sub-ptyfs]] — masters are
mint-only, no walk resolves one, 9P forbids walking from an opened fid,
and a walk to an in-use newfid is rejected.

That last link is a two-line check in `h_walk` that looks like protocol
hygiene and is load-bearing for a safety property here — which is why
its absence in [[sub-tapestryd]] was worth finding.
