---
id: moc-stratum
type: moc
title: "Stratum: the sibling system, and the four seams it meets us on"
parent: home
created: 2026-08-02
updated: 2026-08-02
---
Stratum is the filesystem — a separate project, separately audited, with
its own on-disk format and its own invariants. It is not a Thylacine
subsystem and this area does not document it. What this area documents is
the **boundary**: the four places where the two systems touch, and the
obligations each side owes the other at those places.

Thylacine's own 9P *client* stack is [[moc-kernel-ninep]] — nine dossiers
covering the wire codec, the session machine, the shared elected-reader
client, and the Larder. This area is the other end of that wire.

## The four seams

- **[[sub-stratum-bdev]]** — Stratum drives the disk. Its virtio-blk
  backend runs *in-process inside stratumd*, holding `CAP_HW_CREATE`
  granted by joey at spawn. There is no daemon protocol between the FS and
  its block device; the seam is a C vtable.
- **[[sub-stratum-boot]]** — joey brings the FS up and pivots onto it.
  Spawn with capabilities, wait for readiness, attach 9P, swap the root,
  re-graft everything the old root carried.
- **[[sub-stratum-server]]** — the 9P server's Thylacine-facing behaviour:
  the four `aname` kinds, whose identity it believes, and the two
  Thylacine-authored extensions it answers.
- **[[sub-stratum-session]]** — the per-user encrypted home: a second
  stratumd per login, the three access gates, and the DEK's lifetime.

## The shape of the boundary

Stratum is **not** trusted to enforce Thylacine's access control, and
Thylacine is **not** trusted to hold Stratum's keys.

The division is sharp and worth stating once. Per-file rwx is the
*kernel's* job — Stratum enforces dataset scope only, never file
permissions, so the kernel's `perm_check` at the FS chokepoint is the
whole of it. In the other direction, `login` never holds a raw DEK; it
forwards an opaque corvus token and the coordinator does the unwrap. Each
side holds exactly what the other must not.

That leaves **identity** as the one thing that must cross, and it crosses
by a single channel: `SO_PEERCRED` on the Unix socket, kernel-stamped and
unforgeable. Everything else is derived from it.

## The load-bearing correction

Scripture once specified 9P's `n_uname` field as the identity channel
(the "F-4" design). **Stratum ignores `n_uname`.** `h_attach` reads it and
discards it — "we already have peer-creds" — and ignores `afid` too, since
Tauth is a no-op when the daemon has already authenticated the socket.

So the channel that actually carries identity is the one Thylacine's pouch
layer marshals underneath `getsockopt(SO_PEERCRED)`, onto `SYS_SRV_PEER`.
Stratum's own source carries the warning back across the boundary: *"This
is LOAD-BEARING … do not 'simplify' the marshal back toward 0."* Thylacine
still forwards `n_uname`, correctly — but as the *foreign-server* path, for
a future server with no `SO_PEERCRED` to read. At v1.0 nothing consumes it.

## Cross-cutting

Hazards: [[haz-unread-pipe-wedge]] (both instances live on this boundary
and in the harness beside it) · [[haz-harness-fail-open]] (the readiness
handshake is its positive form).

## Not here

The Stratum FS itself — Bε-trees, extents, AEAD, snapshots, scrub, the
`/ctl` topology as a Stratum surface. That belongs to Stratum's own
reference. A fact appears here only when Thylacine's correctness depends
on it.
