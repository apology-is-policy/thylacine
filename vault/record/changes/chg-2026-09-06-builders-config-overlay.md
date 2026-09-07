---
id: chg-2026-09-06-builders-config-overlay
type: chg
title: "sub-substrate-builders de-stale: the clade builders must sync build-config.sh + configs/ (the build-configurator arc's silent dependency)"
date: 2026-09-06
arc: arc-vault
commits: ["c694c315"]
touched:
  - sub-substrate-builders
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
[[sub-substrate-builders]] (updated 2026-08-02) missed `1a899a31` (2026-08-24),
the only post-dossier commit on the two builder drivers (`audit: none`; verified
in source). The build-configurator arc made `tools/build.sh` source
`tools/build-config.sh` and apply the `default` preset on EVERY invocation, but
both clade drivers synced only `build.sh` (they predate the arc), so a clade
rebuild via either died immediately at build.sh's config-source line
(`build-config.sh: No such file or directory`).

Fix: `clade-keep-build.sh` (cmd_sync) and `clade-gcp-build.sh` now pack + place
`build-config.sh` + `configs/` beside `build.sh`; the disposable tool, which
clones `main` and overlays the working copy, overlays the working copies of all
of them because the cloned tree may predate the arc and cannot supply them.
Folded into the "No config is duplicated" Mechanism section (the overlay now
carries the configurator, not just build.sh) -- the artifact-assertion lesson in
reverse: a dependency the recipe silently acquires silently breaks every caller
still shipping the old file set.

`updated:` -> 2026-09-06. Stale backlog 30 -> 29.
