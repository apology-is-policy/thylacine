# mesa-gl-headers — vendored Mesa public GL/OSMesa/KHR headers

Why these are in-tree (#153): `usr/ports/sdl2/thylacine/SDL_thylacineopengl.c`
hard-includes `<GL/osmesa.h>`, and until 2026-08-06 the only copies lived in
gitignored, thyla-keep-pulled `build/` trees — so a clean checkout could not
build at all, failing at a missing header instead of a missing artifact fetch.
Headers are small and stable; the multi-hundred-MB GL *archives*
(`libOSMesa.a` etc.) stay pulled, and consumers that need them at link time
already degrade with an announced skip when they are absent.

`tools/build.sh` installs these into the pouch sysroot at the sysroot-rebuild
chokepoint (the #146 fix), from THIS directory — never from `build/`.

Contents (byte-identical to the mesa-thylacine fork's install tree on
thyla-keep, pulled 2026-08-05):

- `GL/gl.h`, `GL/glcorearb.h`, `GL/glext.h` (`GL_GLEXT_VERSION 20250203`),
  `GL/osmesa.h` (OSMesa interface 11.2)
- `KHR/khrplatform.h` (glext.h includes it)

License: MIT (Mesa/Brian Paul) and Khronos MIT-style; the full notice is
inline at the top of each header. No modifications — byte-pristine from the
fork's install output.

Refresh protocol: when the builder's Mesa moves, re-copy from
`build/clade/gl/include/GL` + `build/clade/stage/sysroot/include/KHR` in a
dedicated commit. `build.sh` warns on every sysroot build while the pulled
copy and this one differ; it never silently prefers either.
