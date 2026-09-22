---
id: adt-shed-r3
type: adt
title: "Mount-table shed round 3: an unprivileged route to disarm the console drain (aux had already fixed it) and six union seams in the resolver"
date: 2026-09-21
scope: [sub-kernel-territory, sub-kernel-stalk, sub-kernel-spoor, sub-kernel-devdev]
reviewer: opus
model-start: "claude-opus-5"
model-end: "claude-opus-5"
verdict: clean
counts: {p0: 0, p1: 0, p2: 1, p3: 6}
findings: [fnd-shed-r3-f1]
round-of: chg-2026-09-21-mount-shed
prior-round: adt-shed-r2
created: 2026-09-21
---
## Scope

Branch `browser-b0` @ 36878c94 (code d5c58d76): the shed, the dissolved-union rule, the walkable degrade, the `..` floor. The FALLBACK tier (Fable credits exhausted; same-family preamble). Read-only; TLC on scratch copies reproduced the shed spec's clean counts and five buggy verdicts.

## Convergence

Clean by count (0 P0 / 0 P1 / 1 P2 / 6 P3), but the fixes changed the resolver (wbase, the point-unreachable fallback, the remove-parent report, STALK_MOUNT at a crossed base, a new dissolved-union helper in two syscall consumers), so a round 4 on the fixes follows. The P2 ([[fnd-shed-r3-f1]]) was aux's H9, fixed on aux's branch before the round; cherry-picked. The P3s were the live-union opened-form walk, the dissolved "." cloning an unreachable point, the remove caller's post-stalk re-probe, `unmount("/")` unable to name a mounted-over root, the rule enforced in stalk only, a probe stage that passed on a non-shedding kernel, and stale claims. All fixed; a new probe stage uses Stratum members.
