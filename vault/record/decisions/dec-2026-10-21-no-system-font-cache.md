---
id: dec-2026-10-21-no-system-font-cache
type: dec
title: "No system font cache: fontconfig scans /lib/font and caches only per user"
date: 2026-10-21
status: standing
decided-by: user-vote
affects: [sub-substrate-build]
created: 2026-10-21
---
## Fork

fontconfig keeps a cache of each font directory's scan (`FcDirCacheWrite`).
Where should that cache live on Thylacine?

## Research

- fontconfig runs without a cache. When it finds no valid cache for a
  directory it scans the fonts with FreeType, and `FcDirCacheWrite` tries
  each configured cache directory in turn and gives up quietly when none is
  writable.
- A cache file records its directory's modification time and is
  architecture-tagged; fontconfig rescans a directory whose date disagrees.
- The pool has had no world-writable system tree since M7
  (`dec-2026-10-18-create-speaks-for-a-session-end`).
- The upstream `fonts.conf` names a per-user cache
  (`<cachedir prefix="xdg">fontconfig</cachedir>`, under `$XDG_CACHE_HOME` or
  `~/.cache`).

## Options

- **No system cache.** fontconfig scans `/lib/font` when a program first
  asks and caches only into the user's own cache directory, when one exists.
- **A cache baked into the image.** A host-side `fc-cache` would have to write
  aarch64 cache files for the device's paths and dates.
- **A boot-time service writing the system cache.** It needs a writable
  system directory.

## The call

No system cache (operator vote, 2026-10-21).

## Rationale

`/lib/font` holds a few dozen faces, so a scan is cheap, and with no system
cache nothing can go stale against the pool. A baked cache needs a host tool
whose output must match the device's paths and dates, and a boot service
needs the writable system directory M7 removed.
