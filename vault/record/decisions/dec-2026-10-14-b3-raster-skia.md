---
id: dec-2026-10-14-b3-raster-skia
type: dec
title: "WebKit draws with Skia (bundled), not Cairo + pixman"
date: 2026-10-14
status: standing
decided-by: user-vote
affects: [sub-webkit]
created: 2026-10-14
---
## Fork

B-3 ports WebKit's C libraries. The PlayStation port Thylacine's WebKit is
modelled on draws with Cairo by default, and Cairo (with pixman) was not on
B-3's list. `USE_SKIA` is an ordinary option (`WebKitFeatures.cmake:327`,
`PRIVATE OFF`); with it ON, `OptionsPlayStation.cmake:270-276` drops Cairo,
`USE_CAIRO` and `USE_FREETYPE`, and `PlatformPlayStation.cmake:9-10` takes
`platform/Skia.cmake`.

## Research

- The fork's base is WebKit 2.54 stable (`5220e80b97`), not main.
- GTK and WPE turned Skia on and Cairo off in 2.46 (`OptionsGTK.cmake:162,
  :313`; `OptionsWPE.cmake:155, :442`); Windows defaults to Skia with a Cairo
  fallback. Upstream is trimming Cairo ("[Cairo] Remove threaded rendering
  support", 2026-10-08).
- Skia draws on the CPU on PlayStation under `USE(COORDINATED_GRAPHICS)`:
  `PlatformDisplaySkia.cpp:361-374` returns no GL display, and
  `SkiaPaintingEngine.cpp:68-84` then runs the "SkiaCPUWorker" pool.
- Skia stays bound to EGL and GLES at link time (`ThirdParty/skia/
  CMakeLists.txt:855-866`; the Ganesh GL sources always compile), and needs
  FreeType, fontconfig (`SkFontMgr_fontconfig`), libpng and libjpeg
  (`:827-852`, `:969-972`). Every one is already on B-3's list.
- Thylacine has no EGL (BROWSER-DESIGN 3); its GL is OSMesa over llvmpipe
  or virgl.

## Options

- **Skia** (bundled, upstream's direction, CPU drawing; EGL/GLES link-only,
  the real-or-stub question settled in B-5 with P5).
- **Cairo + pixman** (PlayStation's default; two more libraries to port;
  50 fontconfig calls in its font path against Skia's 20; being trimmed).

## The call

Skia (operator vote, 2026-10-14).

## Rationale

Skia ships in WebKit's tree, so nothing extra is ported, and it is the only
backend GTK and WPE keep, so it is the one upstream maintains. The CPU path
exists already for a port without a GPU process. The EGL link dependency
belongs with P5 in B-5, where WebKit's process layout and its GL are decided.
