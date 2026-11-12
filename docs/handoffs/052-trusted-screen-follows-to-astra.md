# Handoff 052 -- the trusted screen follows its display: what was changed in Lictor while you were away (to Astra)

**From**: aux, 2026-11-12. **To**: Astra, for when you are back.

Handoffs 050 and 051 both ended with the same thing the trusted screen did not do: follow a
display that changes. This is that, done under the operator's word of 2026-11-11 ("Astra is on
vacation until further notice. Feel free to change Lictor."). Your worktree and anything you had
not committed were not touched. Everything below is yours to change or take out; the reasons are
here so that you can judge them without me.

## Why

The trusted screen was made once, for the display Lictor started on. On a Mac the display is a
window: it is resized, made full screen, and moved between a Retina panel and an external
monitor. A screen made at one for a small window was then shown enlarged and coarse for the
rest of the boot, and a screen made at two was shown shrunk where its layout no longer fitted.

## What changed in Lictor

- **`main.rs`: one line in the pass.** After `Seat::step`, and only when the seat's phase is 0,
  `self.screen.follow(&mut self.device.gpu)`. Nothing else in the pass moved.
- **`render.rs`: `Refit` and `refit`.** `refit(stands, host, declared, mm, side, fallback)` is
  `geometry_for` put beside the screen that stands. It answers `Stands`, `Scale(k)` (the same
  size at another scale: nothing is made) or `Size(size, k)` (a new image). It is pure and
  host-tested. `MAX_PIXELS` became `pub(crate)`.
- **`backend/screen.rs`: `follow`, `refit`, `remake`, `back`.** `follow` has its own
  `libhalcyon::display::Follow` over `Gpu::display_events`, the same quiet time and retries the
  compositor's following has. It returns at once while the screen is up. `refit` sends
  GET_DISPLAY_INFO and GET_EDID again. `remake` makes the new image beside the old one and
  swaps only when the device has answered for it whole, so a failure anywhere leaves the image
  secure attention would have entered before. `paint` and `activate` are as you wrote them,
  except that the resource they name is `RESOURCES[self.image]`.
- **The backing is whole 1 MiB chunks, grown and kept.** Your loop made chunks up to the
  image's rounded size, the last one short. `back` now makes whole chunks, chunk `i` at `BASE`
  plus `i` MiB, adds more when a larger image needs them, and never gives one back. The
  device is handed the first chunks that cover the image, so its backing can be up to 1 MiB
  longer than the image. `MOST` is the chunks of the largest screen (34) and `END` the address
  past them.
- **Two reserved resources for the image, named in `objects.rs`.** The image alternates
  between them. `objects.rs` now names every reserved ID: `TRUSTED_IMAGES` (plus 1 and plus 3)
  and `TRUSTED_CURSOR` (plus 2, your cursor plane's, unchanged), with a compile-time check that
  they differ. `backend/gpu/cursor.rs` changed in two lines: `RESOURCE` reads `TRUSTED_CURSOR`,
  and a compile-time check holds its `BASE` at or after the screen's `END`.

## A mistake of mine you should know about

The first version of this (f282857b0, a work-in-progress commit that no guest ran) took
`TRUSTED_ID_START + 2` for the second image. That is your cursor plane's resource. The first
making would have been refused by the device, and the try after it would have released the
cursor's resource and made the trusted image under the cursor's ID. I found it by reading
`cursor.rs` while checking the address layout, before any guest run. The IDs are in one place
now so that the next one added is seen beside the others.

## Three decisions that are yours and the operator's to change

They were made under the operator's standing authorization of 2026-09-01 and are written in
`docs/GRAPHICAL-SAK-OWNERSHIP.md`, "The trusted screen follows its display".

1. **It follows at all.** The alternative is the boot's screen, fitted by whatever shows it.
2. **It follows at the display's event, in the normal phase, and never on entry.** Entry to
   secure attention neither allocates nor asks the device anything it did not ask before. A
   display that changes while an episode is up is followed when the seat is normal again.
3. **Memory is grown when a display needs it, and kept.** The alternative is to reserve the
   largest screen's workspace and backing (about 68 MiB) at every start.

## What it does not do

- It does not follow during an episode. Until the episode ends, the screen is shown as the
  frontend shows a scanout of another size.
- A making that fails is tried again six times at the compositor's waits, and then only at the
  next display event. Nothing drives that path in a test: there is no lever for a refused
  create or an unanswered attach on this lane.
- The trusted images are released with a new `Gpu::unref_own`, not `Gpu::resource_unref`.
  What I first wrote here was wrong, and the audit round showed it (R1 F6): at phase 0 no
  trusted resource is in the condemned list, because restoration's accepted bind drains it,
  so the deferral that could reach a trusted release was the overflow arm, which answers Ok,
  records nothing and is drained by nothing. `unref_own` always sends the command. I did not
  change `condemn` or `resource_unref`.
- The commands are synchronous in the pass, as a client's are: up to eight for one change.

## After the audit round (R1: 0 P0, 0 P1, 0 P2, 6 P3; Opus 5.5 reviewing)

Four of its fixes are in your code:

- `Gpu::unref_own` (above). It moves neither `cmd_seq`/`last_unref_seq` nor the retirement
  journal. tapestryd's presentable self-test orders by those ticks, and a trusted release
  between two of its requests would have moved them (F3).
- `Gpu::test_stall_set_aside` and `test_stall_put_back` (test builds). `Screen::follow` runs
  before the pass serves its connections, so its `GET_DISPLAY_INFO` could have taken a shot a
  client armed for its own bind. It now takes the shot off the lever while its commands go
  and puts it back (F2). The seat's commands on entry and the cursor plane's are as they were.
- `Screen::release_spare` is the one place a spare is let go of, and `follow` calls it after
  an asking that succeeded while the spare is still owed, with the same bounded retries. Before,
  a release that stalled left the old image on the device until the display next changed (F5).
- `resource_unref_raw` was split so that `unref_own` shares its wire half (`unref_wire`).

None of the four is driven by a test: there is no lever that stalls one of Lictor's own
commands. If you add one, a stall at each of `remake`'s commands is what it should reach.

## Where the evidence is

The Lictor dossier's section "The trusted screen follows its display (2026-11-12)" has the
mechanism and what no test shows. The host test is
`render::tests::a_display_that_changed_asks_for_the_screen_it_would_have_had_from_the_start`.
The guest test is `tools/interactive/ls-graphical-sak-follow.exp`: a smaller display, a change
while the trusted screen is up, and a larger display afterwards, each read off Lictor's line
and a dump of the scanout under secure attention. `docs/JOURNAL.md` has the run, the audit
round and what they found.
