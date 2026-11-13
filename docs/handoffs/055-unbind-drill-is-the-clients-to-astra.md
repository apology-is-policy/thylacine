# Handoff 055 -- the unbind drill is the compositor's alone (to Astra)

**From**: aux, 2026-11-13. **To**: Astra, for when you are back.

A fix in your broker, made under the operator's word of 2026-11-11 ("Astra is on vacation until
further notice. Feel free to change Lictor."). Your worktree and anything you had not committed
were not touched. Everything below is yours to change or take out.

## What was wrong

Round R3 of the display-follow review (Opus 5.5, 2026-11-13) found that `ArmScanoutDisableRefusal`,
wire code 10, was served to the compositor in every image, with no test-mode and no boot word, and
that what it armed sat in `Gpu::set_scanout`: the next display disable anywhere in Lictor was
refused without being issued. `Device::restore` makes one at the end of every episode whose
presentation is none. A compositor that kept its presentation at none (a bind and then the
resource's unref does it) and the drill armed made the restoration fail; `Seat::step`'s restoring
arm then failed the seat, and the kernel's failure path cancels the grant the user has just made
(`cap_cancel_imperium_pending`). The lever is one shot, so the seat recovers at once with nothing
conferred, and the compositor can arm it again after each episode. The doc comment ("Deliberately
NOT a client verb") had stopped being true when the lever became a broker request.

## What changed

- `usr/lictor/src/backend/gpu.rs`: the drill's two fields, its two methods and its check in
  `set_scanout` are gone.
- `usr/lictor/src/backend/device.rs`: `Device` holds the drill (`disable_drill`, `drill_refused`).
  `SetScanout` naming resource 0 with the drill armed spends it and answers `Error::Hardware`
  without issuing the command, as `set_scanout` did; `TakeInjectedRefusal` takes the marker.
  `restore` and the trusted screen's bind call `Gpu::set_scanout` directly and never see it.
- The wire is unchanged. tapestryd's `warp_img_selftest` runs as before: its disable is a client
  `SetScanout`, and the refusal it sees is the one it saw. The one difference is that an injected
  refusal no longer stamps `cmd_seq` and `last_scanout_seq` for a command it did not issue; the
  drill reads neither.

## Not done, and why

The drill is not put behind test-mode or the boot word: the self-test runs it on every Venus boot,
and what it refuses now is only the caller's own request, so it is no lever over anyone else.

## How it was checked

Host: lictor 76 tests, tapestryd 130, the guest and production builds. In a guest (the second run
of 2026-11-13): a compositor made to disable the display and arm the drill on every pass, against
the broker as changed (no seat failed) and against a `restore` that reads the drill again (the
seat failed: `lictor: seat failed: normal scanout restoration`). Both on ce4243c64. The dossier `sub-lictor` and row 209 of
`docs/AUDIT-TRIGGERS.md` (item f) say the rule.

## Main closes the same hole its own way

corona's landing, on main at 7da0341f4, closes it in `Gpu`: `set_scanout_client` reads the
drill and `set_scanout`, which `restore` calls, does not (corona's round 24). When aux-3-mac
merges main, main's form stands and this branch's `Device` form goes.

## At the merge (2026-11-13)

aux-3-mac merged main e28cc34ea that day. In `device.rs` and `gpu.rs` main's form stands:
`Device` holds no drill, `SetScanout` goes through `Gpu::set_scanout_client`, and `restore`
binds through `Gpu::set_scanout_restore`. The other Lictor changes the merge brought together:
- `Gpu::unref_wire` (the trusted screen's `unref_own` shares it) now judges an unref's answer as
  main does (`gpu_api::unref_frees`).
- `Stats` carries both `display_events` (aux) and `stalls_begun` (main).
- `TestArmLatch` gives 14 seat shots and 3 stall shots, as on main; the display's three binds take
  connection-bound `TestStallShot`s instead (round R4), which neither the owed-free pass nor a takeover
  can take.
- The trusted screen's `follow` runs after main's owed-free pass, at phase 0.

## Round R4 (2026-11-13)

Round R4 audited the merge and R3-F9 (Opus on Opus; 0 P0, 0 P1, 0 P2, 4 P3). In Lictor:

- `objects::parks_on_late_bind`: a bind of one of the service's own ids whose deadline passed parks nothing
  (`Gpu::condemn_if_stalled`). Main's restore binds without draining the list, so a late trusted bind used to
  leave the trusted image in the condemned list into phase 0, holding a client's slot and moving
  `CondemnedCount`. `unref_own`'s comment now gives the real reason a phase-0 release is safe: every return
  from a trusted bind is an accepted restore bind queued behind it, or a dead engine.
- `gpu_api::scanout_unsettled_after`: the proxy reads a scanout command against `Stats::stalls_begun` and the
  count at the end of its own request, not against the last reply it had. `Gpu::stalls_seen` is gone.
- `TestArmLatch` gives 3 stall shots again, as on main: tapestryd's display aims use `TestStallShot`.
- Open, not changed: a frame `Device::restore` gives up (`Met::Refused`) leaves the output blank and nothing
  tells the compositor; tapestryd then never binds again while it stays composed. Two designs are written up
  in the aux track's notes; neither is chosen.
