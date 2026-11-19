# 061 -- Lictor's news carries the display's counts (to Astra)

From aux, 2026-11-19. Branch `aux-3-mac-trust`, in the merge of main ee7271715
(the promotion of M15 and the Mac arc's Lictor and tapestryd work). Built for the
guest and host-tested; NOT YET IN A GUEST when this note was written, and under
audit. You were away; the operator said agents may change Lictor and leave you a
note. This is the note. It follows 060 (corona's news broker) and 057 (the blank
restore).

## Why

060's broker tells the compositor when one of Lictor's answers changed, and
tapestryd asks only then. Two of the answers it acts on were not news:
`Stats.display_events`, which tapestryd's display follow reads to follow the
host's window, and `Stats.restores_blank`, which 057 added so a composed display
is rebound after a restore that ended blank. Both ride on every reply, and the
compositor read them off the last one. Under the broker an idle compositor makes
no call, so a host display change at idle woke nothing and was never followed.
The operator chose (vote M37, 2026-11-19) to make both part of the GPU class,
which already means "the `Stats` every reply carries, changed since the reader
was told". The rejected option was a fourth class for the display.

## What changed in Lictor

- `src/news.rs`: `Facts::display: (u32, u32)` -- the display's event count and
  the restores that ended blank. `Heard` keeps what each reader was told of it
  beside the engine pair; `pending` reports GPU when either pair differs, and
  `told(GPU)` marks both. Two host-test arms: a display event or a blank restore
  is GPU news and is quiet once told; the display moving mid-episode is news, as
  the engine moving is. Three mutants are red (pending ignoring it, told
  forgetting it, and tapestryd's epoch below).
- `src/main.rs`: the pass-end facts take `display` from the same `Stats` as the
  engine pair. The InputDrain arm counts its take for news (060) and answers with
  the seat it is of (`Drained`, aux's clause 15); the merge keeps both.

## What changed in tapestryd

The serve loop keeps 060's gating, and every reading of the seat -- `SeatState`
on SEAT news, and the seat each drain's reply carries -- goes through
`epoch::SeatEpoch` before anything it covers is handed on. While the last
reading was of an episode (`SeatEpoch::away`, new and host-tested) the loop
waits on the news watch. A seat that has come back re-pends INPUT and GPU. 060
did that on a generation change; the epoch also does it on phase 0 after a
non-zero phase at the same generation. `display_pump` and `bind_pump` run every
pass after the fence pump, which takes on GPU news, so a changed count has been
asked for by the time they read it.

## What is owed

The audit round on the merge (with aux's owed round R5), then the guest gates on
QEMU 11.1.3: the display-follow and graphical-SAK scenarios, the throttle and
idle gates, and a control with the display facts left out. sub-lictor's News
section and sub-tapestryd's serve-loop section say what is shown once it is.
