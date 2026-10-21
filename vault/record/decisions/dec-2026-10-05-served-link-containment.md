---
id: dec-2026-10-05-served-link-containment
type: dec
title: "A link a remote export serves resolves beneath the mount that served it"
date: 2026-10-05
status: standing
decided-by: user-vote
affects: [sub-kernel-stalk, sub-haul]
created: 2026-10-06
---
## Fork

The resolver contains a symlink at the caller's own Territory root (DISTRO
4.2, I-28). For a link that a remote 9P export serves, that boundary lets the
export's author steer a guest resolution into the guest's own files: an
absolute target re-anchors at the caller's root and a `..`-bearing relative
target climbs out of the mount. Found as audit IMG-SLIDE F6 (OPEN-BUGS
2026-09-29): `lantern` refuses a link at a deck file, but not one in a
directory above it.

The question put to the operator was "Contain links served by a remote
mount?", with three options: contain beneath the mount (an extension of I-28
in the resolver, scripture first); no-follow on Haul mounts (refuse every
link, as Linux and FreeBSD `nosymfollow` do -- simpler, but it breaks an
export's legitimate internal links); or leave the limit documented
(LANTERN-DESIGN 8).

## Decision

The operator voted on 2026-10-05: **contain beneath the mount**. A served
link -- one whose Spoor belongs to a session declared remote (HAUL-DESIGN
4.8) -- re-anchors the resolution at the root of the mount it was reached
through; from the link on, no `..` rises above that root until a later
absolute link re-anchors. An export's own internal links keep working. The
design, its mechanism and its edges are DISTRO 4.6; the invariant text is the
I-28 row of ARCH 28.

Keying on the existing remote declaration needs no new syscall bit. The
declaration was display-only (HAUL-DESIGN 4.8); the resolver now reads it, and
only to narrow a resolution, so it still grants nothing.
