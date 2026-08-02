---
id: seam-expect-channel-close
type: seam
title: "macOS expect 5.45 closes its channel spuriously under host load"
status: open
surface: [sub-substrate-interactive]
opened-by: chg-2026-08-01-substrate-sweep
tracker: "#78 residual"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

Either a newer/patched expect, a replacement driver for the PTY side, or a
proof that the remaining occurrences have a different cause.

## The residual

`reason=stdout-broken` with the guest still `R+` recurs during **small
post-login output** when the host is heavily loaded — e.g. a second QEMU
running concurrently. It is macOS expect 5.45 closing its own channel, not
the relay and not the guest: the pre-#78 blocking relay reproduces it
identically, which is the discriminating evidence.

Two earlier causes wore this same symptom and are both fixed — BSD `nc -U`
dying of SIGPIPE (#72), and the 2000-byte default `match_max` forcing ~55
discard-and-rescan cycles over a ~110 KB boot burst. Raising `match_max` to
200000 took the observed rate from 2/10 to 0/10 but did not eradicate this
last one.

## Why it is recorded rather than tolerated

Because it is the one remaining way a HARNESS-FAIL can occur, and the gate
counts that as RED with coverage LOST. The `reason=` field is what keeps it
attributable: `stdout-broken` (reader closed) versus `socket-eof` (guest
gone) is "the difference between chasing the relay and chasing expect."

## Risk while open

An interactive gate run on a busy host can lose a scenario's remaining legs
and report red for a harness reason. The operational mitigation is to run
interactive gates with the host otherwise idle — which is a constraint, not
a fix.
