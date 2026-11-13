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
  the device answered the bind or the flush with an error. The frame is then
  given up: `presentation = Presentation::None`, one line on the console, and
  the compositor repaints as it does after any episode.
- `Unrestored::Waiting`: the device has not answered. The engine took no
  synchronous command at entry (`Gpu::takes_commands`, now `pub`), or a stall
  began across the restore's commands. `gpu_api::met(dead, stalls_before,
  stalls_after)` is the one place that tells an answer from a stall, and it has
  a host test. RESTORING returns and asks again next pass; the kernel's five
  seconds from END are the bound.
- `Unrestored::Lost`: the engine is dead, or the device refused the disable.
  Only this fails the episode in RESTORING. FAILED asks again every pass.

The restore's binds are `Gpu::set_scanout_restore` and
`Gpu::set_scanout_blob_restore`: the broker's bind, no lever, and no drain of
the parked frees. Those frees are a client's and now wait for a client's next
accepted bind. `Gpu::set_scanout` (the trusted image's bind) and
`Gpu::set_scanout_client` drain as before.

`DetachBacking` is NOT refused. tapestryd calls `detach_backing` at nine sites
(usr/tapestryd/src/server.rs; not read for this change), and the restore no
longer depends on what a client detaches.

A closed episode now says `lictor: episode closed: frame rebound generation=N`
or `... output blank ...`.

## Test levers and scenarios

- `SeatShot::StallInRestore` is the shot before the last. The table in
  `Device::test_latch_seat_shot` counts from 13 now, and `TestArmLatch` arms 13
  seat shots. `ls-graphical-sak-dead` has leg T for it, between R and 2.
- `caps-probe` accepts `--seat-waited` (leg T's witness).
- `ls-graphical-sak` reads its transcript at the end: no `seat failed`, and four
  `episode closed: frame rebound`. Before, it passed over an episode whose
  restore failed, because the seat recovers by itself.

## If you change this

Keep the two rules of 054. For the restore specifically: nothing but the
device's answer may blank the output, and nothing a client sets may fail an
episode that has ended.
