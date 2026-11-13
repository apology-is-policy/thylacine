# 055 -- the seat's restore: a refused frame, a stall, and no client free (to Astra)

From corona, 2026-11-13. Branch `corona/restore`, on top of the landing branch.
Written and NOT YET BUILT OR RUN IN A GUEST when this note was committed; the
dossier's Open list (vault/system/userspace/services/sub-lictor.md) says when
that changes. You were away; the operator said agents may change Lictor and
leave you a note. This is the note. It follows 054.

## What was wrong (audit round 25; on main too)

1. The restore rebinds the compositor's last frame, and it trusted the device
   to accept it. A compositor that detaches the backing of the resource on the
   display makes QEMU's 2D device refuse that rebind (read from QEMU's source;
   not yet run). In RESTORING that failed the episode after authentication and
   cancelled its grant. In FAILED the same refused rebind was asked every pass,
   so the seat never came back for the boot.
2. RESTORING failed the episode on a stall (a synchronous chain outstanding when
   the restore began, or a stall begun inside it), where every other phase
   waits. One such stall could come from a client's deferred free, which the
   restore's bind drained before its flush.

## What the restore does now

`Device::restore` (usr/lictor/src/backend/device.rs) returns
`Result<Restored, Unrestored>`.

- `Restored::Rebound`: the frame is bound again.
- `Restored::Blank`: scanout 0 is disabled. Either nothing was bound, or the
  frame could not be bound again: the ledger no longer holds its resource, or
  the device answered the bind, or an image frame's flush, with an error (a
  blob frame's bind and flush are one chain, and a flush error there is only
  logged, so that frame counts as rebound). The frame is then
  given up: `presentation = Presentation::None`, one line on the console, and
  the output stays blank until the compositor binds a frame again. Its repaint
  after the episode does not do that: tapestryd binds its screen only when it
  enters composed mode (`let entering = self.scanout != Scanout::Composed;`,
  usr/tapestryd/src/server.rs), so a tapestryd whose frame was refused shows a
  blank output until its next mode change. If it should come back sooner,
  tapestryd would bind its screen again after each episode; this branch does
  not change tapestryd.
- `Unrestored::Waiting`: the device has not answered. The engine took no
  synchronous command at entry (`Gpu::takes_commands`, now `pub`), or a stall
  began across the restore's commands. `gpu_api::met(dead, stalls_before,
  stalls_after)` is the one place that tells an answer from a stall, and it has
  a host test. RESTORING returns and asks again next pass; the kernel's five
  seconds from END are the bound. No chain a client parked reaches the device
  meanwhile (`Gpu::hold_parked`, below), so a client cannot time work into
  that wait.
- `Unrestored::Lost`: the engine is dead, or the device refused the disable.
  Only this makes Lictor fail the episode in RESTORING; the kernel fails it
  itself if its five seconds run out first. FAILED restores the same way and
  asks again every pass while the restore waits or is lost on a live engine;
  with the engine dead it returns the seat without its display.

The restore's binds are `Gpu::set_scanout_restore` and
`Gpu::set_scanout_blob_restore`: the broker's bind, no lever, and no drain of
the parked frees. Those frees are a client's and now wait for a later bind
that drains: a client's accepted image or blob bind, or the next takeover's
trusted bind (`Gpu::set_scanout`); a client's blob probe drains nothing, as
before.

`DetachBacking` is NOT refused. tapestryd calls `detach_backing` at nine sites
(usr/tapestryd/src/server.rs; not read for this change), and the restore no
longer depends on what a client detaches.

A closed episode now says `lictor: episode closed: frame rebound generation=N`
or `... output blank ...`. If the generation moved at RESTORED (the kernel
failed the episode at its deadline and then took RESTORED from FAILED), it says
`lictor: seat failed: the kernel's deadline passed before the restore was
acknowledged generation=N` instead.

## Audit round 27 (same day, Opus reviewing Opus)

- `Gpu::hold_parked`: `Seat::step` sets it from every STATUS, before any pump,
  to `phase != 0`, and `unpark` publishes and releases nothing while it is set.
  Before, the pump ran in every phase, so a client could park a readback behind
  a ring-timeline chain (`ring_idx` above 0, which the takeover does not wait
  for), let that chain retire inside the episode, and put fresh work ahead of
  the restore until the kernel's five seconds ran out: an authenticated episode
  failed. The takeover's synchronous chains are fenced on the device's
  timeline, so what came before the chord has retired once it is up. The 2D
  device runs no fenced chain, so this has no witness there; one is owed on a
  virgl host. Still open, and not reached by the hold: whether a host display
  path makes a restore bind wait behind ring-timeline work that writes the
  displayed resource.
- The FAILED arm clears the test shots armed for an episode's frame or return,
  so a shot whose takeover failed cannot fire at a later episode.

## Test levers and scenarios

- `SeatShot::StallInRestore` (leg T) stalls the engine just before the restore,
  and `SeatShot::StallInsideRestore` (leg B, round 27) makes the restore's own
  first command retire late, the only shot that reaches the stall count
  `gpu_api::met` compares. They are the third and second from last. The table
  in `Device::test_latch_seat_shot` counts from 14 now, and `TestArmLatch` arms
  14 seat shots. `ls-graphical-sak-dead` runs leg T and then leg B between R
  and 2.
- `caps-probe` accepts `--seat-waited` and `--seat-bound` (the witnesses of legs
  T and B).
- `ls-graphical-sak` reads its transcript at the end: no `seat failed`, and four
  `episode closed: frame rebound`. Before, it passed over an episode whose
  restore failed, because the seat recovers by itself.

## If you change this

Keep the two rules of 054. For the restore specifically: only an answer may
blank the output (the device's, or the ledger's for a resource it no longer
holds), no client chain may reach the device while the seat is out of NORMAL,
and nothing a client sets may fail an episode that has ended.
