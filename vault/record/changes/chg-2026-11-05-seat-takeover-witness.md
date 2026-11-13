---
id: chg-2026-11-05-seat-takeover-witness
type: chg
title: "Vote 50 round 2: the takeover's stall says so, a client that survives its own load, a witness for each arm"
date: 2026-11-05
arc: arc-halcyon-interaction
commits: []
touched:
  - sub-lictor
  - sub-tapestryd
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-05
---
Vote 50's audit, round 2, read the code half of the takeover fix as sound and the witnesses as weak: the client meant to reproduce it died on the refusal its own load causes, an episode failing right after takeover passed all three scenarios, and nothing proved a stall had happened at the chord. It also found scripture still stating the removed behaviour, and one of my edits placing a client-aimable denial in a paragraph that says October 2 approved it. That approval covered stalled session brokers. The clause is out, and the question is marked as an open operator vote in SEAT-REVIEW, PORTABILITY, TRUSTED-PATH and GPU-DESIGN, which now also state the ordinary command's wide wait behind a readback.

The takeover now says when it meets a stall (`lictor: takeover met a stall, starting over`), and the chain scenario requires that line in at least one chord behind a client's draws and forbids it with no client; a run where no chord met one is inconclusive, not a pass. `warp-prove chain-hold` treats EAGAIN as the broker's stall and counts it, keeps one transfer of the busy target in flight when asked, and ends by itself so a witness can type with nothing queued, since keys typed behind a queue can be lost (enqueued). Failure arms come first in each episode, because Corvus prints its end line for a seat that failed too. Each of the fix's arms now has a leg that reaches it: a device's answer fails the takeover at once (leg A), a wait in an episode does not widen behind a readback (leg W, a lever with no client or host), a cursor chain past the abandonment bound frees the lane when it retires and tapestryd's refused update then goes through (leg X), and on the Pi a pointer motion behind a real readback stalls its lane and comes back. A frame paint that meets a stall stays owed instead of failing the episode. A refused pointer update forgets what it last sent, and a failed cursor upload forgets that the host's copy is known. A scripted serial found one more scenario bug before any boot: arms for lines from three processes lost a line to a later one that matched first, now one alternation.

Not booted when written.
