---
id: sub-imperium
type: sub
title: "Imperium — trusted conferral and a revocable elevated shell"
parent: moc-userspace-tools
code: [usr/imperium/src/main.rs, usr/imperium/Cargo.toml, usr/lib/fasces/src/lib.rs, usr/lib/fasces/Cargo.toml, usr/imperium-probe/src/main.rs, usr/imperium-probe/Cargo.toml]
audit: hard
guarded-by: [inv-i2, inv-i25, inv-i27]
validated-by: [prose, gate-interactive, gate-smp]
locks: []
hazards: []
abis: []
design: [docs/IMPERIUM-DESIGN.md, docs/TRUSTED-PATH.md]
created: 2026-09-17
updated: 2026-09-18
---
## Halcyon operation

Ordinary tool output uses stdout, so the requesting PID, capabilities,
conferral and relinquishment appear in the caller's Halcyon PTY. The key remains
on Corvus's trusted channel. Ctrl-Alt-Delete invokes graphical Lex curiata;
serial BREAK requires the configured recovery posture. The fasces library uses
baked U+2016 rods and a readable `[axe]` terminal cue. The trusted dialog draws
its own fixed fasces emblem. [[sub-lictor]] documents the authority boundary;
`docs/manual/18-imperium.md` documents operation.

## Purpose

An eligible user confers a restricted capability set through a physical SAK
and a trusted corvus key prompt, then works in a revocable elevated shell.
The Haul integration brings the necessary aux-3 implementation to main.

## Contract

`imperium [dac] [chown] [kill] [post]` requests the named subset, or the whole
imperium level when none is named. The untrusted tool prints intent and waits;
it does not authenticate the user. Corvus renders the exact request and reads
the distinct imperium key only inside a kernel-owned trusted episode.
`imperium --list` reports the caller's kernel scope. `abdicate` exits its shell
and tears down the entire elevated process subtree, including background jobs.

## Mechanism

The tool rejects non-interactive invocation and nesting, connects to corvus,
and sends IMPERIUM_REQUEST. After trusted authorization it redeems the grant,
becomes the propagating legate root and spawns ut. The root waits for the shell
then exits. [[sub-kernel-caps]] owns propagation and revocation;
[[sub-kernel-cons]] owns the trusted episode; [[sub-corvus]] owns eligibility,
key verification, rate limits and pending-peer validation. `fasces` parses the
kernel `/proc` snapshot and renders the scope indicator for tool and shell.

## Data structures

The tool carries a restricted cap mask and the deferred protocol reply; it
holds no authorization key. The fasces `Imperium` snapshot contains the scope,
flowing capability mask and deadline. The key verifier belongs to corvus.

## Concurrency

The tool's main thread waits on the deferred request, then on its shell. The
kernel serializes fork publication with scope teardown. The waiting shell
passes console input through so it cannot consume trusted episode keystrokes.

## Invariants enforced

![[inv-i2#Statement]]

A child cannot receive more caps than its parent and requested fork mask.

![[inv-i25#Statement]]

Scope-root death marks all members and prevents late child publication.

![[inv-i27#Statement]]

Only the trusted reader can render/read the SAK episode; the tool is not the
authorization display.

## Error paths

Wrong keys, ineligible users, stale requesters, nesting, non-tty invocation,
busy episodes and invalid cap subsets fail closed. Logout disarms/restores the
console and releases namespace references so the next user can log in.

## Performance

Human authorization dominates latency. The key verifier uses the existing
bounded Argon2id/AEGIS wrap; grants do not repeat that cost for each child.

## Prosecution

Review pending-peer rebinding, exact panel cap names, key isolation, expiration,
partial grant/redeem failure, console restore on every exit, nested redemption,
fork during revocation and same-user/cross-user logout. Existing Imperium TLC
clean and mutant configurations pin propagation; runtime `imperium-probe` and
`ls-imperium` exercise conferral, denial, DAC operations and background teardown.

## Seams

Graphical and serial transport share Corvus policy; [[sub-lictor]] owns the
graphical sink and physical input.

## Caveats

The user requested single-agent implementation/review; there is no new
independent audit claim.

## Provenance
