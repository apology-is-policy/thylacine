---
id: sub-libtapestry
type: sub
title: "libtapestry — the client weave, and a cleanup helper one line skips"
parent: moc-userspace-runtime
code:
  - usr/lib/libtapestry/src/lib.rs
  - usr/lib/libtapestry/Cargo.toml
audit: light
guarded-by: []
validated-by: [prose]
locks: []
hazards: []
abis: []
design: ["docs/TAPESTRY.md"]
created: 2026-08-04
updated: 2026-09-05
---
## Purpose

The client half of the graphics protocol: connect to the compositor,
create a surface, get a zero-copy mapping of its framebuffer, draw into
it, present, and receive events. Every graphical program links this;
[[sub-aurora]] is the one that matters, since it is the console.

The naming is the design in miniature — a loom weaves threads into fabric,
a tapestry is the woven picture. The client maps a surface's *weave* and
operates a ring to present into it.

## Contract

`Surface::fullscreen()` or `Surface::open(w, h)` connects a session and
creates a surface. `pixels()` hands out the current draw slot as a mutable
pixel slice. `present(rect)` pushes it and rotates to the next slot.
`wait_event()` blocks for the next event; `poll_event()` does not.
`handle_configure(ev)` absorbs the compositor's redraw and resize
requests, returning whether the geometry changed.

**Each surface owns its own session.** Opening the service mints a fresh
server connection per opener, so one client's surfaces are unresolvable
from any other session. Processes that deliberately share a session by
inheriting the descriptor share its surfaces — that is the Plan 9
shared-mount semantic, not a leak.

`surface_ctl(cmd)` writes one verb on **this surface's own** control
descriptor — the one the surface mint rebound — so it rides the owning
connection **by construction**. That is the point of the shape rather than a
convenience: because no other connection can even resolve this surface, the
verb cannot be aimed at someone else's, and no addressing argument is needed
to prove it. Present and release keep their dedicated paths; this is the
general escape hatch for verbs that do not.

**Authority carried by which descriptor you hold, rather than by a check on
what you named** — the same shape as the inherited control descriptor on the
console, and the reason the per-opener session above is load-bearing rather
than tidy.

`Drop` closes everything: the weave descriptor's clunk drops the client
mapping, the control clunk and connection close retire the surface.

## Mechanism

**The wire is six file operations and no custom protocol.** Open the
service for the session root; open a clone node, which rebinds the
descriptor onto the new surface's control file and reads back its id;
write a create command; open the weave, read its geometry, map it; open
the present and event nodes. The compositor-side protocol is 9P
throughout, which is why this crate is small.

**Presenting is a ring submission whose completion is the recycle gate.**
A present stages a fixed-layout descriptor into a registered buffer,
submits a write, and waits for *that* submission's completion — correlated
by a per-operation tag, because one ring carries both presents and event
reads. Event completions arriving while waiting are routed to the pending
queue rather than dropped.

**Event reads are single-shot, deliberately.** A multi-shot read re-arms
into the same registered slice, so a delivery landing before the client
drains the previous one overwrites it — droppable for a frame tick, a lost
keystroke for the classes that must never drop. Until the ring grows a
provided-buffer pool, the client re-arms after each drain: correct by
construction, one call per delivery batch.

**A resize maps the new generation before unmapping the old.** The ack is
the server's generation fence; then a *fresh* weave descriptor is opened
(the old one's kernel-side mapping is pinned to the old generation, so
fresh state needs a fresh descriptor), the geometry re-read, the new
weave mapped, and only then the old descriptor closed — so the client
stays mapped throughout. The slot cursor restarts, because every slot of
the new generation is untouched.

**Authority writes ride the caller's own connection.** The compositor's
gate checks the connection's kernel-stamped peer, so a mode change sent
over a shared mount would carry the mounter's identity rather than the
caller's. There are two paths for this: one over a live surface's session,
and a throwaway-connection form for the startup push that must happen
*before* any surface exists.

## Data structures

`Surface` holds five descriptors (root, control, weave, present, event),
the geometry, the mapping address, the slot stride and count, the ring,
one registered staging buffer, the current slot, an armed flag, a closed
latch, a pending event queue and a sequence counter.

The staging buffer is partitioned by fixed offset: the present descriptor
and its inline rectangle array at the base, the event landing zone above
it. That partition is what bounds the client-side rectangle count.

`Event` is the decoded 24-byte record. `TapError` has eight variants, one
of which — busy — is load-bearing rather than informational: it means a
resize offer went stale and the caller should keep draining, not that
anything failed.

## Concurrency

None. A surface is owned by one caller, there are no locks and no shared
state. The pending queue is a plain vector used as a queue, with a
front-pop helper whose comment is honest about the cost — event volumes
are small.

## Invariants enforced

None of the enumerated system invariants. This is client code above the
privilege boundary: it reaches the display only through a service its
caller's namespace already grants, and the compositor validates every
request. A defect corrupts this client's own surface.

It is on the *client* side of the surface-share machinery, whose integrity
property is the kernel's and the compositor's, not this crate's. What it
must not do is misuse the mapping it is handed — see the caveats, where
the safety argument for the one unsafe construction rests on a field the
parser does not check.

## Error paths

Every fallible entry returns the crate's error type. The construction path
has a cleanup closure that closes the descriptors opened so far — used on
every failure but one (task #152).

A resize failure *after* a successful ack is unrecoverable for that
surface: the server has moved to the new generation while this client
still holds the old mapping, so presents would show zeroed slots. Callers
are told to treat any non-busy error there as fatal, and aurora does.

The event stream's end-of-file latches a closed flag, so a non-blocking
caller cannot re-arm through it forever.

## Performance

One ring, one staging buffer, no allocation on the present path beyond
the command strings. The present blocks until its own completion — the
recycle gate — so a client's frame rate is the compositor's.

## Prosecution

- **Every failure path in construction must close what it opened.** The
  helper exists for exactly this and one path skips it.
- **The geometry reply must be validated before the mapping is used.**
  Four of five fields are checked; the fifth is the one the safety
  argument names.
- **Event reads stay single-shot** until the ring can hand out distinct
  buffers, or a delivery will overwrite an undrained one.
- **A resize maps new before unmapping old**, or the client is briefly
  unmapped and a concurrent present writes nowhere.
- **Authority writes stay on the caller's own connection.** A shared mount
  carries the mounter's identity.

## Seams

No damage-tracking help — a client computes its own rectangles. No
double-buffer abstraction beyond the slot rotation. No timeout on any
operation.

## Caveats

- **Five descriptors and the weave mapping leak on one failure path**
  (task #152). The construction path defines a cleanup closure and uses it
  on every failure *except* the staging-buffer allocation, which uses the
  question-mark operator and returns early — sandwiched between two
  neighbours that both call the helper. The mapping has already succeeded
  by then, and its lifetime is the weave descriptor's, which is never
  clunked. Aurora's bounded connect retry calls this up to twenty-five
  times, so a sustained allocation shortage leaks the set each time.

- **The one unsafe construction's safety comment names the one geometry
  field the parser skips** (task #154). `pixels()` computes its slice base
  from the slot stride and states "slot stride >= width * height * 4" as
  established. The parser checks the other four fields of the same reply
  and not that one. The property holds — the compositor computes the
  stride as a page-rounded row span — so this is a trust-the-server fact
  written as a checked one, the shape [[sub-netdev]] carries at a
  different joint.

- **The present wait has no bound.** It loops until its own completion
  arrives; a wedged compositor blocks the client indefinitely. Arguably
  correct — the compositor *is* the display, and a renderer with nowhere
  to present has nothing useful to do — but it is worth seeing next to
  [[sub-tls]]'s inverse defect, where the bound sits on the one loop that
  cannot stall.

- **A completion-free return from the submit call spins.** The wait loop
  continues when it reaps nothing, and the submit is a blocking form, so
  this is defensive rather than live — but it is a busy loop rather than a
  back-off if the assumption ever breaks.

- **No tests of any kind.** The crate is unconditionally
  no-standard-library, so the same barrier [[sub-aurora]] hits applies. Its
  whole proof is that the console comes up, which exercises the happy path
  of every function here and none of the failure paths — including the one
  that leaks.

## The restore auto-claim (2026-09-02, H-4b-3a)

`Surface::open` auto-consumes an inherited `TAPESTRY_CLAIM` from `/env`
(`take_env_claim`, guest-gated): the offset the restore tool seeds into a
spawned child's environment, a one-shot 32-hex `u128` placement token. When
present, `open_on_bound` upgrades a Content mint to `Claim(token)`, so the
child's FIRST content surface lands in the leaf the tool placed it in and the
child never learns about placement (13.7's opaque cookie; i3 append_layout
minus the swallow hack). One-shot per process via a `CLAIM_TAKEN` latch, but
correctness does not depend on it -- the server-side consume is already
one-shot, so a spent or inherited-stale token falls back to focus placement,
and the latch only spares the wasted "claim unmatched" round trip. Absent or
malformed -> a normal un-placed open, so a normally-launched program is
unaffected (proved: ls-gfx-panes 33/33, the battery has no such var). The
restore TOOL that mints the tokens + spawns the children LANDED at H-4b-3b
(`halcyon layout restore`, in the halcyon crate). H-4b-3b also hardened the
consume: `take_env_claim` now `remove_file`s the spent `TAPESTRY_CLAIM` from the
consuming child's OWN `/env` (a per-Proc deep copy -- the spawner's is
untouched), so a grandchild spawned from a restored child cannot inherit a token
that names a leaf already taken. Best-effort (a `/env` that cannot unlink only
ever degrades a grandchild to focus fallback, never a foreign placement). The
H-4b arc audit closed 0 P0 / 0 P1 / 0 P2 / 2 P3 (NOT dirty); this surface drew
no finding.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)

## `global_ctl` and `TEV_LAYOUT` (2026-09-05, the KT-1 audit)

`EventRing::global_ctl(cmd)` writes one verb to the compositor's root `ctl` on
the ring's own conn -- the conn every surface minted through `open_on_bound`
shares -- so a `session on` written before the first `fullscreen_on` declares
exactly the conn the surfaces will belong to. Any refusal collapses to
`TapError::Protocol` (E_PERM for a non-session principal, E_BUSY for a seat
held by another principal's live tiles); the caller decides whether to retry
or degrade -- halcyond does both. `TEV_LAYOUT` (kind 10) is the event a
declared session conn receives on one of its surfaces at every structural
layout pass, `value` the layout epoch: re-read `layout`. Other clients never
see it.

