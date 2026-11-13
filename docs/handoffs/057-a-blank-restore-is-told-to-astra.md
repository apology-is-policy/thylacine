# 057 -- a restore that ends blank is told to the compositor (to Astra)

From aux, 2026-11-13. Branch `aux-3-mac`. Built and host-tested; the scenario has
run against a scripted guest, but NOT YET IN A GUEST when this note was committed,
and not yet audited (round R5). The dossier's Open list
(vault/system/userspace/services/sub-lictor.md) says when that changes. You were
away; the operator said agents may change Lictor and leave you a note. This is
the note. It follows 055, whose `Restored::Blank` paragraph named the gap.

## What was wrong (audit round R4, F3, P3)

055 gave up a frame the device refuses at the restore, and said that tapestryd's
repaint after the episode does not bring the picture back: it binds its screen
only when it enters composed mode, so a composed display stayed blank until its
next mode change. Round R4 reported that as F3. The operator chose the fix
(2026-11-13, "Lictor reports it"; turned down: tapestryd rebinds after every
episode; leave it until 3D). corona agreed on yip, and asked that the count
cover every blank restore, not only a refused frame.

## What changed in Lictor

- `Stats.restores_blank: u32` (usr/lictor/src/gpu_api.rs) is on the wire after
  `stalls_begun`, 4 more bytes, in every reply as the rest of `Stats` is. It wraps.
- `Device::restore` (usr/lictor/src/backend/device.rs) adds one on every
  `Ok(Restored::Blank)`: a frame given up, or none presented. A `Rebound` restore
  leaves it alone. The backend's copy is `Gpu::restores_blank`.
- `gpu_api::scanout_unsettled_by_restore(before, seen, now)` is the rule, with a
  host test: the scanout is unknown once the count has moved.
- The GPU proxy (usr/lictor/src/proxy/gpu.rs) adopts the count on every reply.
  When it moved, it sets `scanout_unsettled`, so tapestryd's `bind_pump` binds its
  screen again, and `restore_blanked`, which names the cause until a scanout
  command is accepted. tapestryd's fence pump makes a call every pass, so the bind
  comes on its first pass after the seat returns.

## The test lever

- `TEST_RESTORE_WORD` is `thylacine.test-latch-restore` (run-vm.sh:
  `THYLACINE_TEST_LATCH_RESTORE=1`). Lictor reads it from the boot line
  (`Device::test_declare_restore`, from main.rs), as it reads the other two.
- `Request::TestArmRestore` is opcode 74. Without the word it is refused through
  `test_undeclared(2, ...)`; `test_refusal_said` is `[bool; 3]` now. With it, one
  shot.
- `SeatShot::RefuseRestore` is taken by `Device::test_latch_seat_shot` before any
  other shot. That keeps `TestArmLatch`'s 14 in their order; its last shot kills
  the engine, so nothing could follow it in one boot.
- The shot sets `Device::test_refuse_restore` at the takeover (seat.rs). At the
  restore, `Device::test_restore_refusal` (called by `rebind_image` and
  `rebind_blob` after `check`) answers the rebind `Error::Hardware` before anything
  is sent and says `lictor: the restore's rebind refused (INJECTED by
  test-latch-restore)`. `met` reads that as `Met::Refused`: no stall, live engine.

## Tests

- Host: lictor 128 (two new: `a_restore_that_ended_blank_unsettles_the_scanout`,
  `the_restore_lever_has_its_own_opcode_and_word`; the Stats wire test covers the
  new field). Seven mutants of `gpu_api` (the rule's three ways to be wrong, the
  opcode both ways, the word, the field's place on the wire) each turn a test red.
- Guest: `tools/interactive/ls-graphical-sak-blank.exp`. Episode 1 takes the shot.
  It wants Lictor's injected refusal, the frame given up, `episode closed: output
  blank`, and tapestryd binding the same resource again with nothing typed. Then
  typing must change the scanout QEMU dumps: a disabled output shows QEMU's
  placeholder, which nothing drawn can change. Episode 2 is the control: no shot
  left, `frame rebound`, nothing bound.

## If you change this

- Every restore that ends blank moves the count, and nothing else does.
- The count rides `Stats`, so the compositor reads it on a reply it already gets
  every pass and sends no request of its own for it.
- `RefuseRestore` stays first, or the chained and latch levers' sequences change
  under their scenarios.
