---
id: dec-2026-10-05-sak-first-chunk-residue
type: dec
title: "One pre-SAK console chunk may still appear after the trusted episode begins"
date: 2026-10-05
status: standing
decided-by: user-vote
affects: [sub-kernel-cons, sub-imperium, inv-i27]
created: 2026-10-06
---
## Fork

When the secure-attention key fires, a program that is not attached but
already holds the console's transmit role can still put one chunk of its
pre-SAK output, 512 bytes at most, on screen after the trusted episode begins.
The flash could be a fake prompt fragment, but it has no input path:
keystrokes go only to the trusted login. Dropping one `i > 0` guard in
`cons.c`'s write loop would make output exclusivity total, at the cost its
author recorded: such a write could then return a count of zero. The audit
round over IM-1 and IM-2 (its F1) kept the residue and left the call to the
operator.

The question put to the operator was "Change it?", with two options: keep the
residue, the ratified behaviour documented in AUDIT-TRIGGERS row 155 and
IMPERIUM-DESIGN 11.3; or make it total, so that no byte of a non-attached
writer reaches the screen after the episode begins, a write caught at that
instant returns 0 and its caller retries.

## Decision

The operator voted on 2026-10-05: **keep the residue**. The guard stays: a
writer that holds the role when an episode opens finishes the chunk in flight
and stops at the next boundary, with a short count. IMPERIUM-DESIGN 11.3
classes the echo of a feed byte accepted just before BEGIN with it: output,
bounded and pre-SAK.

## Rationale

A zero count reads as an error to a caller that saw no freeze. The residue
is bounded, it was written before the key, and nothing typed after the key can
reach it, so it cannot carry a credential.
