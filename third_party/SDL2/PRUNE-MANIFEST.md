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
  - `src/render/` keeps only `software/`. This survives CL-7 (below):
    that dir is SDL_Renderer's GL backend, gated by `SDL_VIDEO_RENDER_OGL`,
    which we do not define. GL programs here issue raw GL against an
    `SDL_GL_CreateContext` context and never call `SDL_RenderCopy`, so
    the GL *renderer* has no consumer even once GL is on.
- `src/video/SDL_egl.c`, `src/video/SDL_vulkan_utils.c`.
  (`SDL_vulkan_internal.h` + `include/SDL_vulkan.h` are KEPT:
  `SDL_sysvideo.h` includes the internal header unconditionally; its
  content is fully `SDL_VIDEO_VULKAN`-guarded — inert here.)
- `include/SDL_egl.h` — its only consumer is `src/video/SDL_egl_c.h`,
  which nothing includes (verified by grep over the whole kept tree);
  `SDL_sysvideo.h`'s `egl_data` member is `SDL_VIDEO_OPENGL_EGL`-guarded.
  OSMesa binds a context to a client memory buffer, so no EGLDisplay
  ever enters the picture.
- `include/SDL_opengles*.h` (`SDL_opengles.h`, `SDL_opengles2*.h`,
  the ES2 khrplatform/gl2ext set) — `SDL_video.c` includes those only
  under `SDL_VIDEO_OPENGL_ES{,2}` *and* `!SDL_VIDEO_OPENGL`; desktop GL
  is the CL-7 target, so that arm is unreachable.
- The non-Thylacine `SDL_config_<platform>.h` variants (`SDL_config.h`
  + `SDL_config_minimal.h` kept as the dispatcher + reference).

## Un-pruned at CL-7 (#109) — the desktop-GL headers

`include/SDL_opengl.h` + `include/SDL_opengl_glext.h` were originally
removed as "unreachable with GL disabled". `SDL_VIDEO_OPENGL` is now
defined (LLVM-DESIGN.md §9 step 2 — llvmpipe through the gallium OSMesa
frontend), which makes them reachable, so both are restored
byte-pristine from the same tarball. Two files, not the whole GL
surface: `SDL_video.c` includes `SDL_opengl.h` directly under that
switch, `SDL_opengl.h` pulls `SDL_opengl_glext.h` (unless
`NO_SDL_GLEXT`), and `SDL_video.c`'s `SDL_GL_GetAttribute` reads
framebuffer-attachment tokens that live in glext, not in the GL 1.1
core header. `glext` is also what makes the promise in §9 step 2 —
"stock SDL-GL programs recompile" — actually true, since a stock
program including `<SDL_opengl.h>` expects the extension tokens.

`SDL_opengl.h`'s dead `#include "gl_mangle.h"` (guarded by
`USE_MGL_NAMESPACE`, and the file is absent from the tarball entirely)
stays dead — it is not a missing dependency.

Worth knowing when reading these alongside a Mesa port: `SDL_opengl.h`
opens with `#ifndef __gl_h_ / #define __gl_h_`, the same guard Mesa's
`GL/gl.h` uses. The two are mutually exclusive by construction —
whichever is included first wins and the other becomes empty. That is
how a program can include both (TyrQuake's `vid_sgl.c` includes
`<GL/gl.h>` and `"SDL.h"`) without a redefinition storm, and it means
Mesa's header is the one in force for GL programs here.

## The Thylacine driver is NOT here

The `thylacine` video/events driver is OUR code and lives in
`usr/ports/sdl2/` (with the build config + the bootstrap-registration
patch applied to a build-dir COPY of this tree — the pouch-on-musl
idiom; this vendored tree itself is never edited).
