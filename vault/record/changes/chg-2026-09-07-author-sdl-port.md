---
id: chg-2026-09-07-author-sdl-port
type: chg
title: "author the ports plane: sub-sdl-port (audit:hard, I-42 CAP_JIT + W-3e Vulkan) + sub-tyrquake (audit:light)"
date: 2026-09-07
arc: arc-vault
commits: ["PENDING"]
touched:
  - sub-sdl-port
  - sub-tyrquake
  - moc-userspace
established:
  - sub-sdl-port
  - sub-tyrquake
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-07
---
Operator-directed (the docs/reference retirement's authoring phase): the two
ports-plane dossiers that were UNOWNED redirects at the retirement flip, authored
as an audit:hard round. The `vault/system/userspace/ports/` directory was empty
(`.gitkeep` only) until now; a `## Ports` section in [[moc-userspace]] registers
the plane.

## sub-sdl-port (audit:hard) -- the reason the round is audit:hard

The SDL2 Thylacine backend, covering the two load-bearing surfaces the operator
named:

- **I-42 CAP_JIT / W^X**: `THYLACINE_GL_CreateContext` acquires CAP_JIT on the
  program's behalf, in the platform layer, before llvmpipe (a JIT) touches
  anything -- because "stock SDL-GL programs recompile unchanged" is the delivery
  shape and a per-app capability protocol would break it. The claim that keeps it
  sound: SDL only ASKS (`thyla_acquire_cap_jit`, the corvus SELF-form walk in
  `usr/lib/thylajit/thyla_capjit.h`); corvus decides against the caller's own
  eligibility, nothing is granted in SDL. CAP_JIT is elevation-only /
  fork-stripped, so the acquire MUST be a per-process self-walk. Enforcement is
  the kernel's ([[sub-kernel-mmu]] W^X/dual-map/I-cache + [[sub-kernel-caps]]
  I-2/I-6). The weak-OSMesa-symbol link discipline (one libSDL2.a serves GL and
  non-GL; `-u OSMesaCreateContextExt`), the OSMESA_BGRA zero-copy-into-weave, the
  reweave re-bind, the LP_NUM_THREADS pool-sizing (#150), and the Warp-4 direct
  present are all documented.
- **W-3e Vulkan glue**: the five hooks over `VK_EXT_headless_surface`; the weak
  `vk_icdGetInstanceProcAddr` + the `-u` linking model; THE ARMING MOVE -- the
  surface half (`thyla_tap_glsrc`) FIRST, then the ctx half
  (`vn_renderer_thylacine_set_surface`), so no img poke can name a ctx-less
  surface; a skipped consent degrades (display-inert), not fails. Server half is
  [[sub-tapestryd]] (I-40/I-45).

Plus the video driver vtable, the pump-thread/bounded-ring/translation-switch
input path, `thyla_tap` (the C mirror of [[sub-libtapestry]]), and the nogl
fallback. guarded-by [[inv-i45]]/[[inv-i40]]/[[inv-i7]] (I-42 has no inv-note --
prose + ARCH section 28 + the CL-7k row).

## sub-tyrquake (audit:light) -- the consumer

GL Quake: the worked example that a stock SDL-GL app runs unpatched at the
graphics layer because sub-sdl-port carries the platform facts. The ramfs
launcher (GPU preference via a surviving /env GALLIUM_DRIVER write; execv-first,
posix_spawn + ^C-forward fallback; NO CAP_JIT -- acquired by the SDL backend) and
the two boundary-line patches: 0001 nosound-guard (the S_ClearOverflow snare:segv
+ the S_UnblockSound link fix) and 0002 condebug-fd (the log-PATH pin + the
LOAD-BEARING per-line close that publishes under the I-38 close-to-open cache,
[[sub-kernel-larder]]). guarded-by [[inv-i38]] -- its one invariant obligation.

## Verification

Every claim carries a file:line, verified against main @70f91be3 (the CAP_JIT
core read directly; the video/events/tap/nogl/vulkan/tyrquake/build facts from a
scoped survey). The frame-intent / thyla_tap_intent symbol is aux-side (ABSENT
from usr/ports/sdl2) and deliberately NOT documented. vault-lint 0 fail.
