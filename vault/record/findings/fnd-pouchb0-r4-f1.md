---
id: fnd-pouchb0-r4-f1
type: fnd
title: "a console poller registered during a trusted episode is stranded on the episode list after END: the re-arm kept hooks where the first scan put them"
round: adt-pouchb0-r4
severity: P1
status: fixed
surface: [sub-kernel-poll, sub-kernel-cons]
threatens: [inv-i9, inv-i27]
fixed-by: chg-2026-09-21-srvconn-two-endpoint-poll
regression: "cons.episode_frozen_poller_follows_end, cons.episode_prior_poller_not_woken_by_keys; specs/cons_poll.tla NoMissedConsPoll + NoSecretCadence with cons_poll_buggy_no_reregister.cfg and cons_poll_buggy_no_reregister_cadence.cfg"
created: 2026-09-21
---
## Prosecution

**File**: `kernel/cons.c` `cons_poll` (the list chosen by `cons_caller_frozen()` at REGISTER time) + `kernel/poll.c` the re-arm loop (sample-only `pw = NULL` re-scans; hooks persist)
**Invariant**: I-9 (and IM-1's "a frozen poller is not woken per keystroke")
**Prosecution**:
1. A non-attached caller polls `/dev/cons` during an episode: its hook goes on `episode_poll_list`.
2. END walks both lists; the poller re-samples an empty ring and sleeps AGAIN -- its hook still on the episode list.
3. The per-byte relay walks `poll_list` only: `poll(-1)` never returns until the next SAK. Secondary: a pre-SAK poller stays on `poll_list` across the episode and is woken in-kernel per key byte.
**Suggested fix**: re-register the hook when the episode state changed.

## Disposition

Fixed differently. The suggested fix leaves the secondary as a side channel (each wake is a kernel pass whose CPU cost the caller can time -- the cadence of the secret), so the spec was extended first (`cons_poll.tla` NoSecretCadence) and the poll loop re-registers EVERY pass: unhook all, put all, clear, then each `.poll` WITH its hook. The Dev re-chooses its list each pass; the fd re-resolves with its hook (the parked S1 item). Both real-poller tests fail on the sabotaged sample-only loop.
