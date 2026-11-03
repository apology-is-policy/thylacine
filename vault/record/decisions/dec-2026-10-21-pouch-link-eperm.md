---
id: dec-2026-10-21-pouch-link-eperm
type: dec
title: "Pouch's link and linkat answer EPERM: Thylacine makes no hard links"
date: 2026-10-21
status: standing
decided-by: user-vote
affects: [sub-pouch-fs, sub-pouch-seam]
created: 2026-10-21
---
## Fork

fontconfig (B-3g) takes the lock for a cache write with `link()`, and falls
back to a directory lock only when `link()` fails with EPERM, ENOTSUP or
EACCES (`src/fcatomic.c`, `FcAtomicLock`, 2.18.3). Pouch's `link` and `linkat`
reach the unimplemented-call sentinel 0xFFFF, which patch 0001 answers with
ENOSYS without a trap (invariant P-3). So fontconfig could never write a font
cache, and the upstream tests that build one would fail. What should `link`
answer on a system that makes no hard links?

## Research

- Plan 9 has no hard links. APE's `link` (Plan 9 4e and 9front,
  `sys/src/ape/lib/ap/plan9/link.c`) sets `errno = EMLINK` and returns -1,
  under the comment "BUG: LINK_MAX==1 isn't really allowed". POSIX requires
  `LINK_MAX` of at least 8.
- Linux's link(2) answers EPERM when the filesystem holding both paths cannot
  make hard links (FAT, some 9P servers). Ported code that tries a hard link
  first falls back on that errno: fontconfig's lock keys on it, and git's
  object store falls back to `rename` on any failure.
- 9P2000.L has Tlink. Real hard links would need Stratum support, a native
  call and namespace rules, none of which exist.

## Options

- **EPERM, as Linux.** Ported code's fallbacks work unchanged. A Pouch patch
  and an ERRORS.md line.
- **EMLINK, as APE.** fontconfig, and every later port with the same reflex,
  needs a patch to fall back on it.
- **Keep ENOSYS and patch fontconfig.** The libc keeps saying the call is
  unimplemented; each port that tries a hard link first needs the patch.
- **Real hard links.** New kernel and Stratum work, against Plan 9's design.

## The call

Pouch's `link` and `linkat` answer EPERM (operator vote, 2026-10-21). No
hard link is ever made.

## Rationale

EPERM is the answer ported code is tested against for a filesystem without
hard links, so its fallbacks work as they do on Linux and no port carries a
patch for it. ENOSYS describes the libc, not the system: Thylacine refuses
hard links by design, and the refusal should read as one.
