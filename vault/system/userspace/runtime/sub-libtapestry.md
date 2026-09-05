---
id: sub-libtapestry
type: sub
title: "libtapestry — the client weave and the shared event ring"
parent: moc-userspace-runtime
code:
  - usr/lib/libtapestry/src/lib.rs
  - usr/lib/libtapestry/src/ring.rs
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
create surfaces, get a zero-copy mapping of each surface's framebuffer,
draw into it, present, and receive events. Every graphical program links
this; [[sub-aurora]] is the one that matters, since it is the console, and
halcyond is the one that stresses it, since it drives a whole
session's worth of surfaces — tiles, tag bars, menus, the status bar — from
one process.

The naming is the design in miniature — a loom weaves threads into fabric,
a tapestry is the woven picture. The client maps a surface's *weave* and
operates a ring to present into it.

**One session, one ring, many surfaces (H-3c-2).** The load-bearing shape
this crate settled into: an `EventRing` is ONE 9P session to `/srv/tapestry`
plus ONE Loom ring, and every surface opened *on* that ring shares both,
taking a slot for its own event queue. The one-surface convenience
constructors (`Surface::fullscreen` / `open`) still make a private ring each
— aurora, the battery, tapestry-demo and warp-prove are single-surface and
unchanged — but a multi-surface client (halcyond) opens ONE ring and every
tile, chrome bar, menu and status bar rides it. The reason is not tidiness;
it is a kernel constraint that a two-ring client silently starves (see
Mechanism).

## Contract

`EventRing::connect()` opens a session to the compositor and sets up its
ring; `EventRing::adopt(root)` wraps a session root fd the caller already
holds (the ring owns it thereafter). `Clone` is another handle to the same
ring — the session closes with the last handle. Surfaces are then minted on
the ring:

- `Surface::fullscreen_on(ring)` / `open_on(ring, w, h)` — a content surface.
- `Surface::chrome_on(ring, pane_id, w, h)` — a `Role::Chrome` tag bar bound
  to a pane (H-3b); renderer-gated server-side.
- `Surface::menu_on(ring, w, h)` — the one ephemeral `Role::Menu` surface
  (H-3c); invisible until the compositor places it, and torn down by the
  compositor itself.
- `Surface::status_on(ring, w, h)` — the `Role::Status` bottom bar (H-3d);
  `w` must be the display width and `h` one status unit or the compositor
  refuses.
- `Surface::open_claim_on(ring, w, h, token)` — a content surface steered
  into the specific empty leaf a restore token names (H-4b).

`Surface::fullscreen()` / `open(w, h)` are the private-ring shorthands: they
make an `EventRing` and open one surface on it.

Per surface: `pixels()` hands out the current draw slot as a mutable pixel
slice. `present(rect)` (or `present_rects` / `present_hold`) pushes it,
waits for that submission's completion, and rotates to the next slot.
`handle_configure(ev)` absorbs the compositor's redraw and resize requests,
returning whether the geometry changed.

Per ring: `wait()` blocks until ANY surface on the ring has an event, then
routes every reaped completion to its surface's queue; `poll()` reaps
without blocking. A surface's own `wait_event()` / `poll_event()` then take
from *its* queue. `global_ctl(cmd)` writes one verb on the ring's session
root ctl — the conn every surface shares — so a session declaration made
before the first surface names exactly the conn those surfaces will belong
to.

**Authority is carried by which descriptor you hold, not by a check on what
you named.** A surface's control verbs ride the fid the `surface/new` mint
rebound onto that surface — the owning connection, by construction, because
no other connection can even resolve the surface. `global_ctl` rides the
ring's own conn for the same reason. This is the shape that makes the
per-opener session load-bearing rather than tidy: the compositor's gate
checks the connection's kernel-stamped peer, so a verb sent over a shared
mount would carry the mounter's identity, and the shape forecloses that
without an addressing argument.

`Surface::drop` retires the surface (a `destroy` verb, then it leaves the
ring slot, then closes its fds); `EventRing`'s last handle closes the
session.

## Mechanism

**The wire is a handful of file operations and no custom protocol.** Open
the service for the session root; open `surface/new`, which rebinds the fid
onto the new surface's control file and reads back its id; write a `create`
command (with the role/claim suffix for a chrome/menu/status/claimed mint);
open the weave, read its geometry, map it; open the present and event nodes;
join the ring. The compositor-side protocol is 9P throughout, which is why
this crate is small.

**One session per ring is load-bearing, not a convenience.**
`loom_wait_for_completions` (kernel/loom.c) pumps the session of the ring's
FIRST in-flight op only, so a ring spanning two sessions would silently
starve the second: its replies would land only inside a blocking wait or one
of that thread's own RPCs on that session. The H-3c lever measured exactly
this across halcyond's earlier two-rings-on-two-sessions arrangement — a
tile's CONFIGURE arrived only at the next pane-tree RPC, and a menu's
keystroke never. Collapsing to one ring over one session is the fix, and it
is why the shared-ring constructors exist.

**A present is a synchronous write whose completion is the recycle gate.**
`present` stages a fixed-layout descriptor into the surface's region of the
ring's registered staging buffer and does a `t_write` of the present on the
present fid; the compositor composes inside that write's dispatch, so the
Rwrite IS the recycle gate, and the calling thread is the session's reader
for the duration (in-flight event replies demux to CQEs meanwhile). This
replaced an earlier Loom WRITE that bought nothing and cost a second
registered handle per surface.

**Event delivery is a per-slot single-shot read, demultiplexed by tag.**
Each surface's event read is armed into that surface's 128-byte region of the
one registered staging buffer; the completion's `user_data` packs the slot
in bits 40.., a per-join generation in bits 8..40, and the op class in the
low byte. `EventRing::wait` arms every idle slot's read (`arm_all`), blocks
in ONE `enter(GETEVENTS)`, and routes each reaped completion by its tag into
that slot's queue. `poll` is the submit-only form — a non-blocking enter on a
non-SQPOLL ring demuxes nothing, so replies land only inside a blocking wait
or this thread's own RPCs. Reads stay single-shot because a multi-shot read
would re-arm into the same region and overwrite an undrained delivery —
droppable for a frame tick, a lost keystroke for the classes that must never
drop.

**The registered-handle table is index-stable, with a placeholder fid.**
`ring::table` rebuilds the whole table at every join/leave (the kernel has no
per-entry update — `Ring::register_handles` has `IORING_REGISTER_FILES`
replace-whole semantics), and index == slot index: a live slot holds its
event fid, every other index (free, retiring, or left) holds a read-only
`ctl` PLACEHOLDER fid the ring opens once at `adopt`. The stability is the
point: `loom_drain_sq` can leave an SQE unconsumed (the CQ admission gate,
the chain gate), and a dense rebuild between arm and consume would re-bind
that SQE's index to another surface's fid. A stale read on the placeholder
returns text nobody reads — never another surface's event.

**The slot lifecycle is the I-7-shaped hazard, handled in `ring.rs`.** A
`join` takes a free slot and bumps its generation. A `leave` frees the slot
at once if no read is armed, else marks it RETIRING — kept until the
in-flight read completes, so a stale completion can never write a re-minted
surface's region (the generation in the tag is the belt on that brace). A
completion for a slot whose generation has moved on is dropped; a completion
on a retiring slot frees it; a `result <= 0` (EOF or error) latches the
slot's stream closed so a poll caller cannot re-arm through the end forever
and an errored read's inline CQE cannot satisfy every wait at once (a spin).

**A resize maps the new generation before unmapping the old.** The ack is
the server's generation fence; a *fresh* weave descriptor is opened (the old
one's kernel-side mapping is pinned to the old generation), the geometry
re-read, the new weave mapped, and only then the old descriptor closed — so
the client stays mapped throughout. The slot cursor restarts, because every
slot of the new generation is untouched.

## Data structures

`EventRing { core: Rc<RefCell<RingCore>> }` — a cheap-to-clone handle; every
public entry borrows the core once for its duration.

`RingCore { ring: Ring, staging: RegisteredBuffer, slots: Vec<ring::Slot>,
placeholder: OwnedFd, root: OwnedFd }` — the Loom ring, the one registered
staging buffer (`MAX_RING_SURFACES` × `EV_REGION` = 48 × 128 B), the slot
table, the placeholder fid for empty table entries, and the session root
(closed with the ring). No manual `Drop`; fields drop in declaration order
(the Loom first, the root last).

`ring::Slot { used, retiring, event_fd, armed, closed, gen, pending }` — the
per-slot bookkeeping. `pending` is the surface's event queue; `gen` is the
completion-matching generation; `closed` is the stream-end latch.

`Surface` no longer owns a session. It holds a *clone* of the `EventRing`, a
`slot: u16` (its place on the ring), the ring's session `root` (read-only —
the ring closes it), its four fids (ctl, weave, present, event), the geometry
(`w`/`h`/`stride`), the `slot_stride`/`nslots`/`map_va`/`cur_slot` of the
mapped weave, a `presents` counter, and `slot_seen: [u64; MAX_SLOTS]` (per
present-slot, the `presents` value at which it was last drawn, or
`SLOT_UNSEEN` — the age bookkeeping `age` reads).

Constants: `TEVENT_LEN` = 24 (the wire event record), `EV_REGION` = 128 B
(the per-slot landing zone), `EV_CAP` = 4 × 24 (records per read),
`MAX_RING_SURFACES` = 48 (the session's 64-tag table minus the synchronous
RPCs' share — a parked event read holds a tag), `SLOT_QUEUE_CAP` = 256 (the
client-side per-slot backlog before arming stops), `MAX_SLOTS` = 8 (the
present-slot rotation the age array bounds).

`Event` is the decoded 24-byte record (kind/code/value/rune/mods/flags/tick).
`TapError` has a `Full` variant (no free ring slot) alongside the transport
and protocol errors; its `Busy` is load-bearing rather than informational —
it means a resize offer went stale and the caller should keep draining.

`Mint` (internal) is what `create` mints: `Content`, `Chrome(pane_id)`,
`Menu`, `Status`, or `Claim(token)`.

## Concurrency

Single-threaded by construction. The ring's state lives behind
`Rc<RefCell<RingCore>>`; every public entry takes one short borrow, and
`route` pushes into queues the core owns, so no callback re-enters a Surface
while the core is borrowed. There are no locks and no cross-thread sharing:
a ring and its surfaces are owned by one caller. `Clone` on an `EventRing`
shares the core by reference count, not across threads.

## Invariants enforced

None of the enumerated *system* invariants. This is client code above the
privilege boundary: it reaches the display only through a service its
caller's namespace already grants, and the compositor validates every
request. A defect corrupts this client's own surfaces.

It sits on the *client* side of the surface-share machinery, whose integrity
property (I-40 / I-7) is the kernel's and the compositor's. What this crate
must not do is misuse a mapping it is handed, or route one surface's event to
another — the latter is the slot-lifecycle discipline in `ring.rs`, which is
exactly why that module is factored out syscall-free and host-tested: a
completion lands only in the slot AND generation it was armed for, a retiring
slot is never re-armed and never reused until its last read completes, and
the table index is the slot index.

## Error paths

Every fallible entry returns `TapError`. The surface construction path uses a
cleanup closure (`fail`) that closes the descriptors opened so far, and once
`create` has succeeded server-side it uses `fail_created`, which writes
`destroy` before closing — so a failure past the create point does not leak a
server-side surface for the session's life (the H-3c-2 round F2 fix: the mint
already took a per-conn slot, so a bare close would pin it).

`route` latches a slot `closed` on any `result <= 0`, so both EOF and a
transport error end that surface's stream for good, and `take_event` returns
`Err(Closed)` once the stream ends and the backlog is drained. `join` returns
`TapError::Full` when the ring's 48 slots are taken.

A resize failure *after* a successful ack is unrecoverable for that surface:
the server has moved to the new generation while this client still holds the
old mapping, so presents would show zeroed slots. Callers treat any non-busy
error there as fatal, and aurora does.

## Performance

One ring and one registered staging buffer per client, shared across up to 48
surfaces; no allocation on the present path beyond the command strings. A
present blocks until its own completion — the recycle gate — so a client's
frame rate is the compositor's. `wait` wakes on the first event across all
surfaces and drains in one enter, so a session with many idle surfaces pays
one blocking syscall per event batch, not one per surface.

## Prosecution

- **A completion routes only to the slot AND generation it was armed for.**
  The tag packs both; `route` drops a mismatched generation. Without it a
  re-minted slot would inherit a departed surface's in-flight read.
- **A retiring slot is never re-armed and never reused until its read
  completes.** `leave` marks retiring when armed; `join` skips it; `route`
  frees it on the completion. A reuse-before-completion would let a stale read
  write a live surface's region.
- **The table index is the slot index, rebuilt whole.** A dense rebuild that
  moved a live slot would re-bind an unconsumed SQE to the wrong fid.
- **Every construction failure past `create` says `destroy`.** Otherwise a
  refused weave/present/event open pins a server-side slot for the session.
- **A stream-end latches closed.** Otherwise a poll re-arms through EOF
  forever and an errored read spins every waiter.
- **The geometry reply's stride is validated before the mapping is used.**
  Four of the five fields are checked; the fifth — `slot_stride` — is the one
  the `pixels()` safety argument names and the parser trusts.
- **Authority writes stay on the caller's own connection.** A shared mount
  would carry the mounter's identity; the surface-ctl and global-ctl shapes
  ride the owning conn by construction.

## Seams

No damage-tracking help — a client computes its own rectangles. No
double-buffer abstraction beyond the slot rotation. No timeout on any
operation.

**SQPOLL (KT-1.5b).** `EventRing::connect_sqpoll` / `adopt_flags(root,
SETUP_SQPOLL)` sets up a kthread-driven ring: the kernel poll-thread drives
the session's reader and posts completions asynchronously, so the ring fd
(`poll_fd`) becomes pollable and a multiplexing client can wait in one
`poll(2)` over the ring plus its other streams instead of blocking in `wait`.
The `/srv/tapestry` srvconn transport is deadline-capable, which the SQPOLL
reader requires (the kernel loom register gate rejects a non-deadline-capable
handle on an SQPOLL ring). halcyond's unified poll uses this; aurora's
single-surface blocking `wait` does not.

## Caveats

- **The one unsafe construction's safety comment names the one geometry field
  the parser skips** (task #154). `pixels()` computes its slice base from
  `slot_stride` and states "slot_stride >= w*h*4" as established. The parser
  checks the other four fields of the reply (width, height, row stride ==
  w*4, slot count against `MAX_SLOTS`) and not `slot_stride`. The property
  holds — the compositor computes the slot stride as a page-rounded span — so
  this is a trust-the-server fact written as a checked one, the shape
  [[sub-netdev]] carries at a different joint.

- **The present wait has no bound.** It loops until its own completion
  arrives; a wedged compositor blocks the client indefinitely. Arguably
  correct — the compositor *is* the display, and a renderer with nowhere to
  present has nothing useful to do — but it is worth seeing next to
  [[sub-tls]]'s inverse defect, where the bound sits on the one loop that
  cannot stall.

- **Event reads stay single-shot.** Until the ring can hand out distinct
  buffers per delivery, a multi-shot read would overwrite an undrained one.
  The single-shot re-arm after each drain is correct by construction, one
  arm per delivery batch.

- **The slot bookkeeping is host-tested; the syscall half is not.** The
  crate splits into a default `guest` feature (the `Surface` + the Loom +
  session halves, which need libthyla-rs and the guest) and, with
  `--no-default-features`, the wire types plus `ring.rs` alone — nine host
  tests drive the routing invariants against a synthetic staging buffer
  (`cargo test -p libtapestry --no-default-features --target
  aarch64-apple-darwin`): an error/EOF stream-end, a dropped older
  generation, a retiring slot freeing on any completion, index stability
  across a rebuild, the full-queue arm stop, whole-record clamping, a foreign
  tag ignored, the join bound. The tests are the H-3c-2 audit's regressions
  (F1 re-arm-forever, F2 refused-create leak, F3/SA-4 table stability, F4
  unbounded queue) as executable counterexamples. What stays unproven by test
  is everything behind `guest`: the actual weave map, the resize reweave, the
  present/recycle path — their whole proof is still that the console comes up
  and halcyond drives a session, which exercises the happy path of every
  guest function and few of its failure paths.

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

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
