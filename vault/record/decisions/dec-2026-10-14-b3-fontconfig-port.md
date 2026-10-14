---
id: dec-2026-10-14-b3-fontconfig-port
type: dec
title: "fontconfig is ported, and fonts live in /lib/font"
date: 2026-10-14
status: standing
decided-by: user-vote
affects: [sub-webkit, sub-substrate-build]
created: 2026-10-14
---
## Fork

BROWSER-DESIGN O-4: fontconfig as a port, or a small static-configuration
shim. Both WebKit backends require it; Thylacine has no system font
directory (its fonts are compiled into halcyond).

## Research

- Skia creates its font manager through fontconfig on every non-Android,
  non-Windows target: `FontCacheSkia.cpp:70`, `SkFontMgr_New_FontConfig(
  FcConfigReference(nullptr), SkFontScanner_Make_FreeType())`; its fallback
  cache calls `FcFontSort` (`SkiaSystemFallbackFontCache.cpp:109`). WebCore's
  Skia path calls 20 distinct Fc* functions, the FreeType/Cairo path 50.
  WebKit's Skia CMake references Skia's directory font managers 0 times.
- fontconfig 2.18 falls back to `read()` when mmap of a cache file fails
  (`fccache.c:1173-1203`), so it needs no shared file mapping. Its build runs
  gperf on the host and needs expat or libxml2 for the target.
- Plan 9 keeps fonts in `/lib/font`.

## Options

- **Port fontconfig + `/lib/font`**: fontconfig 2.18 with libxml2 (already on
  B-3) as its XML parser; host gperf.
- **A static shim** of the Fc* calls WebCore and `SkFontMgr_fontconfig` make,
  over a fixed font list: less code, a reimplementation of fontconfig's
  matching rules to own.
- **Patch WebKit off fontconfig** (Skia's directory font manager plus a
  rewritten fallback cache): a permanent patch to WebKit's font code.

## The call

Port fontconfig; fonts in `/lib/font` (operator vote, 2026-10-14).

## Rationale

The real library keeps WebKit's font selection and fallback exactly as
upstream tests them, at the cost of one more routine port whose XML parser
is already on the list. `/lib/font` is the heritage path and gives the system
the font directory BROWSER-DESIGN P6 named as owed.
