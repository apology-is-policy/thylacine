# Handoff 053 -- Lictor arms its levers only on a declared boot; the old image's release has its own askings (to Astra)

**From**: aux, 2026-11-13. **To**: Astra, for when you are back.

Two changes to Lictor, both from an audit round on the work of handoff 052 (round R2: Opus 5.5
at max effort reading Fable 5.2's changes, `1658f24b8..1d866803e`). Done under the operator's
word of 2026-11-11 ("Astra is on vacation until further notice. Feel free to change Lictor.").
Your worktree and anything you had not committed were not touched. Everything below is yours to
change or take out.

## 1. `TestArmLatch` needs the kernel's command line (the round's F1, P1)

This one was not in my diff. The reviewer was asked whether the lever I had added in 052
(`test_stall_set_aside`) is absent from release builds, and answered with what is true of every
lever.

- `test-mode` is a default feature of `lictor` (`usr/lictor/Cargo.toml`), and `tools/build.sh`
  builds with the defaults, so every image carries the levers. That is #880 and it is recorded.
- What was not recorded: `Request::TestArmLatch` armed two latch shots, four seat shots and six
  stall shots for any client at phase 0. Only tapestryd read `thylacine.test-latch-dead`. So the
  word bound an honest compositor and nothing else. A compositor that is not honest could send
  the request on any boot, and then the first secure attention fails between exclusion and bind
  (`SeatShot::StallAfterExclusion`), it can arm again, and the fourth shot (`StallHeld`) kills
  the engine and withholds input for the rest of the boot.
- The dossier said the lever "needs test-mode, the bootarg and the operator's chord". The
  bootarg was not a condition in Lictor.

The change: Lictor reads `/hw/chosen/bootargs` once at its start (`backend::bootargs`, which
the scale's reader now shares), `gpu_api::bootarg_declared` looks for the whole word
`thylacine.test-latch-dead` (host-tested), `Device::test_declare` keeps the answer, and
`TestArmLatch` answers `BadField` without it. `TestStallNext` and the latch and seat shots only
ever fire from counts that `TestArmLatch` sets, so they follow.

Evidence, and what it does not cover: the guest runs are in the journal entry of 2026-11-13.
The gate's decision in `Device::execute` is in no host build; it is shown in a guest by a
compositor made to ask with no declaration (refused) and by the same with the gate taken out
(armed).

**Not changed, and worth your eye.** The context-hold verbs (`TestHoldCtx`, `TestAbandonCtx`)
need no arming, and tapestryd's `warp-hold` hands them to any client for its own context. A
held context's retirements are swallowed (`gpu.rs`, the hold branch of the drain), so
`controlq_retired()` and `all_work_retired()` stay false and the seat's entry returns until the
kernel's quiesce deadline fails the takeover. I read that from the code and did not run it.
The failure is the recoverable one, with authorization closed. The verbs are main's and
corona's (the warp batteries use them on default boots), so I told them and did not gate the
verbs. Corona answered the same morning: it is their entry of 2026-11-03 (a poisoned fence
denies the graphical SAK), it needs a warp context and so a virgl device, and their landing
branch cures it (`corona/ji-fix` 802b53bc3: the seat asks `Gpu::takeover_clear`, vote 50,
which reads no fenced slot; `ls-graphical-sak-poison` is its scenario). I read that much off
the branch. It reaches main with that landing, which also rewrites `seat.rs`, `screen.rs` and
the takeover half of `gpu.rs`, so both changes below will meet a large diff of Lictor there.

## 2. The release of the image that was (the round's F2, P3)

In 052 a release the device did not answer was asked again "with `Follow`'s retries". Those
were the making's retries. A making that succeeded on its seventh asking left the release
none, and a making given up on left a spare the device might hold (a create answered late, or
a create made and an attach refused) until the display next changed.

- `Follow::renew` (libhalcyon) starts the count again. `Screen::follow` calls it when the pass
  made a new image and the old one was not let go of, so the release has six askings whatever
  the making took.
- When a following is given up on with the spare owed and the engine live, `follow` calls
  `release_spare` once, after its `stays` line. Both are inside the bracket that sets a
  client's stall shot aside.
- Not done, on purpose: asking without end. An asking the device does not answer is a stall,
  and a stall parks every client's command. What is still owed after the bound stays owed until
  the display next changes; `docs/GRAPHICAL-SAK-OWNERSHIP.md` says so now.

`tools/interactive/ls-graphical-sak-unmade.exp` drives the second of these with a real device:
QEMU's `max_hostmem` refuses the trusted image seven times, and the scenario reads the two
releases after the seventh and a next display made with nothing to let go of. The first has no
test: nothing stalls one of Lictor's own commands.
