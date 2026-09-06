---
id: chg-2026-09-06-pouch-libsodium-doc-absorb
type: chg
title: "absorb docs/reference/84-pouch-libsodium (first cross-compiled C lib): zero-fold, build-recipe redirect"
date: 2026-09-06
arc: arc-vault
commits: ["fc4d406d"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---

# docs/reference/84-pouch-libsodium.md -> ABSORBED

Absorbed the 277-line pouch-libsodium reference doc into a redirect stub. It is a
build-recipe / proof-of-concept doc (the first C library cross-compiled against
pouch, needing NO boundary-line patch). The cross-compilation substrate (sysroot,
patch-apply, build/link) -> sub-substrate-build; the pouch seam it validates ->
sub-pouch-seam. Zero fold -- libsodium demonstrates the substrate, it is not a
mechanism with its own dossier.

103 -> 104 absorbed of 157. lint 0-fail.
