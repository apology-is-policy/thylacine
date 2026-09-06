---
id: chg-2026-09-06-libthyla-rs-currency
type: chg
title: "sub-libthyla-rs de-stale: a date-field staleness, not a content one -- the H-4d-1 fold brought the body current but left updated: at 2026-08-15, so the stale tool re-flagged 12 byte-identical files"
date: 2026-09-06
arc: arc-vault
commits: []
touched:
  - sub-libthyla-rs
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
[[sub-libthyla-rs]] (updated 2026-08-15), `audit: light`, was the second of the
two BIG de-stales -- the stale tool reported 12 changed files, ~488 lines,
led by `fs/file.rs` at +/-188. Ground-truthing dissolved almost all of it: this
is a `updated:`-field staleness, not a content one.

The finding, and the checks behind it:

- **The dossier's last BODY edit was `d1a4b8e4`** (2026-09-05, "fold H-4d-1
  into sub-tapestryd + sub-libthyla-rs"), not the 2026-08-15 its frontmatter
  claimed. That fold -- and the two rewrites before it (`bbcd9d4c` the native-
  runtime rewrite, `1de67850` the kernel-resolves/Path rewrite) -- edited the
  body but none bumped `updated:`. The stale tool dates churn by each file's
  last-commit date against the `updated:` FIELD, so every file that legitimately
  changed before the fold got re-flagged against a date three weeks stale. The
  "A STATUS FIELD WHOSE FLIP IS NOBODY'S STEP STAYS UNFLIPPED" shape.
- **All 12 flagged files are byte-identical `d1a4b8e4..HEAD`** (`git diff --stat`
  empty across file.rs, fs/mod.rs, notes.rs, hardware.rs, loom.rs, alloc.rs,
  fs/metadata.rs, io.rs, err.rs, territory.rs, process.rs, ninep.rs). So no
  in-scope code has changed since the body was written; the body describes the
  current bytes.
- **The one genuine post-edit change is `8f553c78` (H-4d-2a), and it touched
  only `lib.rs`** (8 ins / 6 del) -- which this dossier EXPLICITLY excludes
  (its own scope note: "lib.rs ... is described by [[sub-kernel-syscall-abi]],
  which owns it. The subject here is the other 28"). Out of scope by the
  dossier's own boundary; noted, not folded.
- **The two H-1c-2 console-probe functions** (`fd_devclass`, `stdout_is_terminal`
  -- documented this run in the presenters + devdev dossiers) also live in
  `lib.rs`, so they too fall to [[sub-kernel-syscall-abi]], not here.

Rather than take "byte-identical" as sufficient, spot-verified the richest body
claims against current code (== d1a4b8e4 for these files): the allocator's 4 MiB
lazy reservation + three-state atomic init (`alloc.rs` INITIAL_HEAP_SIZE =
4*1024*1024, the UNINIT/INITIALIZING/READY CAS); and the #100 File-rights caveat
-- both constructors still record the constant `Rights::READ|WRITE|TRANSFER`
(file.rs:235, :291) while `rights()`'s doc describes the kernel's A-3b
mode-derivation, so "the most precise statement is the most precisely wrong"
holds verbatim. The Path-rewrite (#87) and #185 splitter caveats likewise track
byte-identical files. Nothing to fold.

`updated:` -> 2026-09-06 (asserting the body IS current as of today, verified).
Stale backlog 25 -> 24.
