---
id: spec-loom-multishot
type: spec
title: "loom_multishot.tla"
models: [sub-kernel-loom]
pins: [inv-i29, inv-i30]
cfgs:
  - "loom_multishot.cfg -- clean: the thirteen-conjunct safety set"
  - "loom_multishot_liveness.cfg -- EventuallyTerminal: an admitted stream always reaches its terminal"
  - "loom_multishot_buggy_double_terminal.cfg -- BUGGY_DOUBLE_TERMINAL: two terminals for one stream (ExactlyOneTerminal)"
  - "loom_multishot_buggy_more_after_terminal.cfg -- BUGGY_MORE_AFTER_TERMINAL: a shot after the stream ended (TerminalEndsStream)"
  - "loom_multishot_buggy_resolve_at_shot.cfg -- BUGGY_RESOLVE_AT_SHOT: re-resolve the object per shot (ObjPinnedAcrossShots)"
  - "loom_multishot_buggy_shot_lost_on_full.cfg -- BUGGY_SHOT_LOST_ON_FULL: drop a shot the queue could not admit (CqAccounted)"
  - "loom_multishot_buggy_stale_after_teardown.cfg -- BUGGY_STALE_AFTER_TEARDOWN: a shot into a torn-down ring"
gate: "any change to re-arm, the shot bound, the terminal decision, or a stream's queue reservation"
created: 2026-08-02
updated: 2026-08-02
---
## Abstraction

One submission, many completions. The completion queue becomes a *multiset*,
which is exactly why this could not be an extension of [[spec-loom]] — that
module's queue is a set with one entry per operation, and the shape is load-
bearing in its invariants.

A stream's life is: admitted, armed, a shot posts and re-arms, and eventually a
terminal that ends it. The re-arm is a distinct step from the shot, because in
the implementation they happen in different contexts — the shot is posted under
the engine's lock, the re-issue is deferred outside it — and the gap between them
is where a stream can be lost.

## What it pins

- **The pin survives the whole stream.** `ObjPinnedAcrossShots` — a re-arm
  re-issues against the *same* pinned object, never a fresh resolve. Same
  property as the single-shot module's, extended over an unbounded number of
  completions, and the one the corresponding buggy configuration attacks.
- **Exactly one terminal, and it ends the stream.** `ExactlyOneTerminal`,
  `TerminalEndsStream`, `ArmedImpliesNotTerminal`. The terminal is what a
  consumer waits on to know the object is recyclable, so a shot arriving after it
  is not a cosmetic ordering error — it is a use-after-recycle.
- **A shot is held, never dropped.** `CqAccounted` — each re-arm reserves its
  next completion slot before re-issuing, so a full queue defers the stream
  rather than losing a shot. This is the single-shot module's admission rule
  applied per-shot, and it is what makes the deferred-re-arm flag safe to leave
  set indefinitely.

## What it cannot see

The synthetic terminal bound. A real multishot operation ends when its event
source does; the only vehicle available when this landed was a durability
barrier, which replies once, so the implementation carries a shot *count* as a
stand-in terminal. The model takes a bound as given and says nothing about
whether a real source terminates.

Which context performs the re-arm is invisible here — the model has one
re-arm step, the implementation has two drive loops that call it and one entry
path that does not. That asymmetry is
[[seam-loom-rearm-needs-blocking-enter]], and it is a liveness gap the model's
own liveness configuration cannot express because the model has no notion of a
caller who declines to drive.

Multishot combined with ordering is rejected by the implementation rather than
modelled; the two modules assume each other's absence.

## Binding

`specs/SPEC-TO-CODE.md::loom_multishot.tla`. The shot-versus-terminal decision ↔
the completion callback's branch on error, shot bound, and post success; the
re-arm ↔ the deferred re-issue in the drive loops; the per-shot reservation ↔ the
in-flight bump taken under the ring lock as the flag is cleared.
