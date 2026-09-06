---
id: seam-nuname-trust-stamp
type: seam
title: "The n_uname identity-forward gate for a foreign 9P server (corvus trust bit)"
status: open
surface: [sub-kernel-syscall-dispatch, sub-kernel-ninep-attach]
opened-by: chg-2026-09-06-9p-identity-absorb
tracker: "v1.x (the n_uname trust-stamp seam family)"
created: 2026-09-06
updated: 2026-09-06
---
## Owed

When a v1.x remote/foreign 9P transport lands, the kernel must gate the M4
`n_uname` assertion on a corvus-stamped trust bit on the service/connection —
before asserting the caller's identity to a server whose peer it does **not**
kernel-stamp.

## The gap

At v1.0 every attach is local (a `SrvConn`), so the identity the server sees is
the kernel-stamped `SYS_srv_peer` principal (A-3 M1) — unforgeable, and already
revealed to any server the Proc connects to (that is how `SO_PEERCRED` works).
The attach handlers also substitute that principal for the `n_uname` Tattach
field (M4), but against a trusted-local Stratum server that is inert: Stratum
ignores `n_uname` and stamps from `SO_PEERCRED`. There is no
assert-identity-to-untrusted path because no remote/foreign 9P transport exists,
so `n_uname`-as-asserted-identity is dormant, not load-bearing.

## What closes it

A `trusted_for_identity_fwd` bit (or equivalent) on `SrvService` / `SrvConn`,
set by corvus's trust-stamp, checked before the kernel forwards the asserted
principal as `n_uname` to a server whose peer it does not itself kernel-stamp.
Ground truth: no such field exists today — the seam is a clean additive field,
not a redesign. The `n_uname` wire value is kept precisely so this path has
something to gate; demoting it to "vestigial" at v1.0 is what makes the future
gate cheap.

## Risk while open

None at v1.0: every server is a trusted local Proc whose peer the kernel stamps
via `SO_PEERCRED`, and the asserted `n_uname` is inert. Becomes load-bearing the
day a remote/foreign 9P mount lands — that chunk MUST pick this seam up in its
design pass, not discover it in audit. Related but distinct from
[[seam-845-untrusted-server]] (the one-reply-per-tag tag-generation envelope on
the same untrusted-server path) and [[seam-stratum-notify-peercred]] (peer-cred
gating on corvus's notify socket): all three are the untrusted-9P-peer family,
but this one is specifically the identity-forward gate.
