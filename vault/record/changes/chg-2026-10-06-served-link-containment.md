---
id: chg-2026-10-06-served-link-containment
type: chg
title: "Served links: a link a remote session serves resolves beneath its mount"
date: 2026-10-06
arc: arc-net
commits: ["a24b0212b", "1434415da"]
touched:
  - sub-kernel-stalk
  - sub-kernel-dev
  - sub-kernel-ninep-dev9p
  - sub-kernel-ninep-client
  - sub-kernel-syscall-abi
  - sub-haul
  - sub-substrate-interactive
  - sub-lantern
  - inv-i28
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-10-06
---
The operator voted on 2026-10-05 to contain links served by a remote mount
([[dec-2026-10-05-served-link-containment]]). A link whose Dev answers the new
`remote` slot ([[sub-kernel-dev]]; dev9p fills it from the session's Haul
declaration, [[sub-kernel-ninep-dev9p]], [[sub-kernel-ninep-client]]) now
re-anchors the resolution at the root of the innermost mount crossed on the
way to it and restarts from there, so its target and every later `..` stay
beneath that mount ([[sub-kernel-stalk]], [[inv-i28]]). The resolver records
each crossing's logical offset beside the trail; a union member's root is
found again by name and must be on the link's own session; a union-handle
base anchors at its walkable form; the phenotype carries across the restart.
The declaration stops being display-only but still grants nothing: it only
narrows ([[sub-kernel-syscall-abi]], [[sub-haul]]). `lantern`'s deck-path
limit, audit IMG-SLIDE F6, closes ([[sub-lantern]]). `haul-links` is the
device witness ([[sub-substrate-interactive]]), added to the boot banner's
mirror set without a change to the banner ([[abi-boot-banner]]). Containment is the
resolver's: `readlink` returns the server's text verbatim, and a program that
re-resolves it itself (musl's `realpath(3)`) gets the Linux answer.
