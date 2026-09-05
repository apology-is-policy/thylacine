# vkQuake vendored source — prune manifest

Upstream: vkQuake 1.05.3 (Axel Gneiting's Vulkan Quake port; QuakeSpasm
0.93 lineage; GPLv2 — `LICENSE.txt`). Chosen at W-4 over the 1.20+/1.3x
lines deliberately: single-threaded renderer (clean attribution — the FPS
delta measures the render+present path, not a task scheduler), the same
engine generation as the tyrquake 0.71 GL baseline (minimizes the engine
confound in the vk-vs-GL comparison), and the exact vintage v3dv used as
its own bring-up validation target on this same V3D hardware.
Tarball: `vkQuake-1.05.3.tar.gz` from
`https://github.com/Novum/vkQuake/archive/refs/tags/1.05.3.tar.gz`
sha256 `1e06d8c9067651df79f955efe0108be165aa0e6e3679a2c231f819d9469d2c43`.

Every file KEPT is byte-pristine from that tarball. PRUNED (whole
files/dirs removed, none edited) to the Linux/SDL2 Vulkan build the
Thylacine pouch cross-build compiles (`build_vkquake()`; the curated
object list mirrors the upstream `Quake/Makefile` OBJS groups with
IN=sdl, CDA=null, SND=sdl, codecs=wave-only).

## Removed (relative to the tarball root)

- `Windows/` (MSVC projects + Windows SDL/codec binaries), `Misc/`
  (retexturing patches, the vq_pak SOURCE tree — its built form ships as
  `Quake/vkquake.pak`, kept), `.vscode/`, `.gitignore`,
  `vs-chromium-project.txt`, `Shaders/compile.bat`.
- `Quake/`: the Windows-only halves — `net_win.c`, `net_wins.{c,h}`,
  `net_wipx.{c,h}`, `pl_win.c`, `sys_sdl_win.c`, `wsaerror.h`,
  `build_cross_win32.sh`, `build_cross_win64.sh` — and `detect.sh`
  (host-OS probe for the upstream Makefile; the cross build pins its
  answers).

## Kept

- `Quake/` (the compile set + headers; the unbuilt music-codec sources
  — `snd_flac.c`, `snd_mp3.c`, `snd_mpg123.c`, `snd_opus.c`,
  `snd_vorbis.c`, `snd_mikmod.c`, `snd_xmp.c`, `snd_umx.c` — stay
  vendored-but-unbuilt, the tyrquake `.S` idiom; wave is the one codec
  compiled), `Quake/Makefile` (the object-list reference the build.sh
  list is curated from), `Quake/vkquake.pak` (the engine's own content
  pak, upstream-built from the pruned `Misc/vq_pak`; staged at
  `/quake/vkquake.pak` by the pool bake), `Shaders/` (GLSL sources +
  `Compiled/*.c` SPIR-V-as-C arrays — upstream-committed, so no
  glslang at build time — + `compile.sh` + `bintoc.c` as generation
  provenance), `LICENSE.txt` (GPLv2), `readme.md`.

## Game data is NOT here

The shareware `pak0.pak` ride is `build_tyrquake()`'s (fetched at BUILD
time, never committed); `build_vkquake()` reuses the same
`build/quake/stage` and adds `vkquake.pak` beside `id1/`.
