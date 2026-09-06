---
id: chg-2026-09-06-devproc-atomic-cdebugowner
type: chg
title: "sub-kernel-devproc de-stale: CDEBUGOWNER is now read/written atomically (spoor_flag_get/set) -- the release gate that keeps the debug_owner no-dangle argument sound under a concurrent fcntl"
date: 2026-09-06
arc: arc-vault
commits: []
touched:
  - sub-kernel-devproc
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
[[sub-kernel-devproc]] (updated 2026-08-16), `audit: hard` (I-39), had two
post-dossier changes to `devproc.c`, both settled, verified in source.

- **The substantive one (`34ff46df`, aux's git-stash atomic-flag sweep).** The
  close hook releases `debug_owner` only when the ctl Spoor's `CDEBUGOWNER` flag
  is set, and that flag is now accessed via `spoor_flag_get`/`spoor_flag_set`
  rather than a bare `|=`/`&`. The Spoor's `flag` word is RMW'd cross-domain (a
  fork shares the Spoor; `fcntl(CNONBLOCK)` writes the same word under a table
  lock), so a non-atomic RMW that dropped `CDEBUGOWNER` would silence the release
  and reintroduce the stale-pointer match the dossier's "the pointer can never
  dangle" bullet rules out. This directly reinforces that bullet -- the close
  hook IS conditional, and the atomic accessors are what keep the ruling true
  under a concurrent `fcntl`. Folded a companion bullet into the debug-attach-slot
  section.
- **Noted, not folded (`71306b60`, main's Warp-6 V-2 close).**
  `maps_type_name` gained `case BURROW_TYPE_HOSTMEM: return "hostmem"` -- a
  one-line display arm so a hostmem burrow shows as `hostmem` in `/proc/<pid>/maps`
  instead of `?`. Below this dossier's granularity (it documents the read-dispatch
  partition, not the per-type display names), so recorded here rather than folded --
  the same disposition used for trivial display facets elsewhere this run.

`updated:` -> 2026-09-06. Stale backlog 26 -> 25.
