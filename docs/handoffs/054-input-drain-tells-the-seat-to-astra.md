# Handoff 054 -- a drain of an input device is answered with the seat it is of (to Astra)

**From**: aux, 2026-11-13. **To**: Astra, for when you are back.

One change to Lictor's interface and its only client's use of it. Done under the operator's word
of 2026-11-11 ("Astra is on vacation until further notice. Feel free to change Lictor."). Your
worktree and anything you had not committed were not touched. Everything below is yours to change
or take out.

## What changed

- `usr/lictor/src/gpu_api.rs`: `pub type Drained = (u64, u32, Vec<(u16, u16, u32)>)`, what
  `InputDrain` answers: the seat's generation, its phase, and the device's events.
- `usr/lictor/src/main.rs`, the `InputDrain` arm: the reply is that triple, built where the events
  are taken. `SeatState` is unchanged.
- `usr/lictor/src/proxy/input.rs`: `InputDev::drain` returns `Option<(u64, u32)>` (the pair; `None`
  for a drain the broker did not answer) and is `must_use`.
- `usr/tapestryd/src/epoch.rs` (new, in the lib, host-tested): `SeatEpoch::read(generation, phase)`
  answers `Standing`, `Resumed` (told once for an episode) or `Episode`.
- `usr/tapestryd/src/main.rs`: the pass no longer asks `SeatState` at its top and before pointer
  drains (`seat_moved` is gone). Each drain's reading goes through `Tapestryd::seat_read` before
  any of that drain's events is handed on. The first reading of a pass comes with the keyboard's
  drain; `SeatState` is asked only where there is no keyboard or its drain was not answered.

`backend/seat.rs` is not changed.

## Why

The pointer's way over the trusted screen is no program's: `Comp::seat_resumed` forgets the
position motion is counted from, and the pass has to reach it before it hands on the first
position after an episode. It asked the seat and then drained. Those are two requests, neither of
which the broker parks, so an episode that begins and ends between them (the compositor's thread
off the processor for that long) hands the focused program one motion as long as the pointer's
way across the episode. The askings before the pointer drains were also placed by an argument
about which stages of the pass can wait in a parked request, which a new stage would void.

Your broker already has what closes it: one thread gathers the events and answers the requests,
`Seat::input` gathers nothing for the compositor while the phase is not 0 and clears what was not
drained when an episode begins, and every arm of `Seat::step` for a phase that is not 0 clears
again. So a reply at phase 0 holds only events of that reply's generation. Answering the pair in
the same reply makes that usable by the client.

## What the compositor does with a reading

- `Standing`: the events are handed on.
- `Resumed`: `interaction_suspend`, the modifiers reset, `seat_resumed`, then the events. Under
  test-mode it says `tapestryd: seat back generation=N`.
- `Episode`: the events are handed to no program, and the pass starts again (at the keyboard's
  drain it suspends interaction and sleeps 10 ms first, as the top of the loop did).

A refused chord's reset (the SYN_DROPPED you push when INPUT is refused) is not lost by the third
rule: `Seat::step` adopts the kernel's phase, which is 0 after a refusal, before the compositor's
drain is served. I read that from the code and no scenario drives it under this change yet;
`ls-graphical-sak-refused.exp` is in the guest run that follows this note.

## Worth your eye

- An event is of the seat that stood when the broker read it from the device. A position made in
  the last milliseconds of an episode and read after RESTORED is the next seat's first event. I
  left that: the device stamps nothing.
- `InputDev::drain` used to ignore a failed call. It still goes on without the device, and the
  compositor now says so once.
- The witness is `tools/interactive/ls-graphical-sak-pointer.exp`, with a fixture in the battery
  (`tapestry-battery --rel-watch`). Its guest run is recorded in `docs/JOURNAL.md` under
  2026-11-13.
