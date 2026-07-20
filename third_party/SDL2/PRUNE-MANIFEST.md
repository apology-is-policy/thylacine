# SDL2 vendored source — prune manifest

Upstream: SDL2 2.32.10 (the final SDL2 release line; zlib license).
Tarball: `SDL2-2.32.10.tar.gz` from
`https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/`
sha256 `5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165`.

Every file KEPT here is byte-pristine from that tarball. The tree is
PRUNED (whole files/dirs removed, none edited) to the subset the
Thylacine pouch cross-build compiles — the G-7 `SDL_thylacine` port
(`docs/TAPESTRY.md` §9/§18.9). Re-vendoring: fetch the tarball, verify
the sha256, apply the removals below.

## Removed (relative to the tarball root)

- Everything at the top level except `src/`, `include/`, `LICENSE.txt`,
  `CREDITS.md`, `README-SDL.txt` (build systems, docs, tests, IDE
  projects, wayland-protocols).
- `src/hidapi/` (vendored hidapi; HIDAPI disabled), `src/test/`
  (the SDL_test library).
- All platform backend dirs except the ones the Thylacine build uses:
  - `src/audio/` keeps only `dummy/` (no virtio-sound yet — TAPESTRY
    §10 item 4; games run `-nosound`).
  - `src/core/` keeps only `unix/`.
  - `src/filesystem/` keeps only `dummy/`.
  - `src/haptic/`, `src/joystick/` (+`virtual/`), `src/loadso/`,
    `src/locale/`, `src/main/`, `src/misc/`, `src/sensor/`,
    `src/video/` keep only `dummy/` (+ `src/video/yuv2rgb/`).
  - `src/thread/` keeps only `pthread/` (pouch pthreads, patch 0004).
  - `src/timer/` keeps `unix/` + `dummy/`.
  - `src/render/` keeps only `software/` (the two-tier rule: Halcyon
    and the SDL seam are pure 2D; GL is v1.1+ via a Mesa port).
- `src/video/SDL_egl.c`, `src/video/SDL_vulkan_utils.c`.
  (`SDL_vulkan_internal.h` + `include/SDL_vulkan.h` are KEPT:
  `SDL_sysvideo.h` includes the internal header unconditionally; its
  content is fully `SDL_VIDEO_VULKAN`-guarded — inert here.)
- `include/` GL/EGL headers (`SDL_opengl*.h`, `SDL_opengles*.h`,
  `SDL_egl.h`) — unreachable with GL disabled (guarded includes
  verified) — and the non-Thylacine `SDL_config_<platform>.h`
  variants (`SDL_config.h` + `SDL_config_minimal.h` kept as the
  dispatcher + reference).

## The Thylacine driver is NOT here

The `thylacine` video/events driver is OUR code and lives in
`usr/ports/sdl2/` (with the build config + the bootstrap-registration
patch applied to a build-dir COPY of this tree — the pouch-on-musl
idiom; this vendored tree itself is never edited).
