---
id: sub-sdl-port
type: sub
title: "The SDL2 Thylacine backend — the video driver, the OSMesa GL path that acquires CAP_JIT, and the two-sided Vulkan consent"
parent: moc-userspace
code: [usr/ports/sdl2/thylacine/SDL_thylacinevideo.c, usr/ports/sdl2/thylacine/SDL_thylacinevideo.h, usr/ports/sdl2/thylacine/SDL_thylacineopengl.c, usr/ports/sdl2/thylacine/SDL_thylacineopengl.h, usr/ports/sdl2/thylacine/SDL_thylacinevulkan.c, usr/ports/sdl2/thylacine/SDL_thylacinevulkan.h, usr/ports/sdl2/thylacine/SDL_thylacineevents.c, usr/ports/sdl2/thylacine/SDL_thylacineevents_c.h, usr/ports/sdl2/thylacine/thyla_tap.c, usr/ports/sdl2/thylacine/thyla_tap.h, usr/ports/sdl2/thylacine-nogl/SDL_thylacineopengl_nogl.c, usr/ports/sdl2/glapi-probe.c, usr/ports/sdl2/SDL_config.h, usr/lib/thylajit/thyla_capjit.h]
audit: hard
guarded-by: [inv-i45, inv-i40, inv-i7]
validated-by: [prose, gate-interactive]
locks: []
hazards: []
abis: [abi-caps]
design: ["docs/LLVM-DESIGN.md", "docs/GPU-DESIGN.md"]
created: 2026-09-07
updated: 2026-09-07
---
## Purpose

The Thylacine backend for a **ported** program: SDL2 (musl, the pouch
boundary-line), not native libthyla-rs. It is the platform abstraction layer
that lets stock SDL applications — 2D-framebuffer, OpenGL, or Vulkan — run
against the Tapestry compositor without knowing Thylacine exists. Its
load-bearing act is not graphics but a capability: an SDL-GL program's request
for a GL context IS, on this platform, a request to JIT, and the backend
acquires **CAP_JIT** (I-42) on the program's behalf here rather than putting a
capability protocol in every future GL port. It reaches the compositor only
through `thyla_tap`, a small C mirror of [[sub-libtapestry]]'s `Surface`,
and the framebuffer it hands SDL is a zero-copy view of weave slot 0.

## Contract

**The SDL video driver.** `THYLACINE_CreateDevice` (`SDL_thylacinevideo.c`, `:56`)
allocates an `SDL_VideoDevice` and installs the vtable: the framebuffer path
(`CreateWindowFramebuffer`/`UpdateWindowFramebuffer`/`DestroyWindowFramebuffer`,
`:79-81`), `PumpEvents` (`:76`), the **nine GL hooks wired unconditionally**
(`:91-99`), and the **five Vulkan hooks wired unconditionally** (`:110-114`) —
the weak-symbol check lives *inside* the hooks, not at the vtable, so one
`libSDL2.a` carries all three graphics modes and each degrades on its own. It
registers as `VideoBootStrap THYLACINE_bootstrap` (`:121`), driver name
`"thylacine"` (`:36`), placed ahead of the platform drivers in `SDL_video.c` by
patch 0001 so `SDL_VideoInit` picks it first.

**One window per process** (`:174-176`): the compositor session is
process-scoped, so a second `CreateWindow` is refused.

**The config switch.** `SDL_config.h` is hand-generated for `aarch64-thylacine`
(the libsodium precedent — no autoconf). `SDL_VIDEO_OPENGL 1` (`:184`) is an
**API switch only** — `SDL_VIDEO_OPENGL_EGL`/`_ES`/`_ES2` and
`SDL_VIDEO_RENDER_OGL` are deliberately off (there is no dynamic loader and no
GLES). `SDL_DYNAMIC_API` is *not* forced here (SDL `#error`s on a config-forced
value); the sanctioned off-switch is the `__thylacine__` arm patch 0001 adds to
`SDL_dynapi.h`, keyed on `-D__thylacine__` (pouch is static-only: no dlopen,
ET_EXEC).

## Mechanism

### CAP_JIT is acquired in the platform layer, before the rasteriser (I-42)

`THYLACINE_GL_CreateContext` (`SDL_thylacineopengl.c`, `:162`) calls
`thyla_acquire_cap_jit("sdl-gl")` (`:202`) before anything reaches llvmpipe.

**Why a capability at all.** llvmpipe *is* a JIT — it compiles rasteriser
variants at draw time and executes them in this process, which on Thylacine
requires **CAP_JIT** (I-42: executable-code emission is a capability, not an
ambient power). CAP_JIT is `CAP_ELEVATION_ONLY` ([[sub-kernel-caps]]): held by
no Proc at creation and stripped at every fork, so no parent hands it down and a
program must walk the corvus clearance path *itself*.

**Why here.** It happens in the platform layer, not in each program
(`:176-201`), because "stock SDL-GL programs recompile unchanged" is the entire
delivery shape of `LLVM-DESIGN.md` §9 step 2. TyrQuake's `vid_sgl.c`
([[sub-tyrquake]]) is ordinary SDL-GL code that has never heard of Thylacine; a
rule that every GL app must first speak a capability protocol would make that
claim false and put a Thylacine-shaped patch in every port. The application
already declared its intent by asking for a GL context — a platform abstraction
exists to carry exactly that kind of fact.

**This grants nothing** (the soundness claim). SDL only *asks*:
`thyla_acquire_cap_jit` reaches corvus, which decides against the **calling
principal's own eligibility** and refuses cleanly when the user has no jit
clearance. A program that acquired CAP_JIT itself (gl-sdl-prove does, to print
which form won) finds this a no-op, and vice versa. No authority is manufactured
in SDL; the kernel's I-2/I-6 monotonicity is untouched.

**Ordering** (`:199-201`): *after* `THYLACINE_GL_Available()`, so a program
linking no rasteriser never opens a corvus connection; *before* the context
allocation, so a refusal has nothing to unwind. On refusal → `SDL_SetError` +
NULL, no context, no leak.

**The walk** lives in `thyla_capjit.h` (`usr/lib/thylajit/`, header-only, so it
pulls in no library SDL lacks): it reaches corvus with raw `svc` for the two
syscalls musl does not carry, opening `/srv/corvus` with **`SYS_OPEN`(65), not
`SYS_WALK_OPEN`(34)** — 34 walks one component and cannot resolve the
multi-segment `/srv/corvus`. It tries the **SELF form** (verb 18, no bearer
secret — corvus authorizes the connection's own principal) before the bearer
form, redeems via `SYS_CAP_USE`, and returns 0 when the Proc holds CAP_JIT else
a distinct non-zero per failure (`ENOSRV`/`ENOCTL`/`EGRANT`/`EREDEEM`). The
`jit` clearance level (`name b"jit", caps T_CAP_JIT`) and its default eligibility
are corvus policy ([[sub-corvus]]). The CAP_JIT bit is `1<<11` ([[abi-caps]]).
The duplication between this header and thylajit's own copy is deliberate and
bounded — if the corvus wire or the clearance name changes, both move
(grep-anchored).

**I-42 enforcement is the kernel's**, not SDL's: [[sub-kernel-mmu]] (the
dual-mapped `BURROW_TYPE_CODE` RW/RX aliases, `arch_icache_sync_range` at
create-invalidate and `SYS_ICACHE_SYNC` at publish; W^X never W∧X at PTE
granularity) and [[sub-kernel-burrow]]. SDL is the userspace consumer: it only
ensures the process is *eligible* to JIT before llvmpipe does.

### The OpenGL context renders straight into the weave (OSMesa on llvmpipe)

The backend is Mesa's gallium OSMesa frontend on llvmpipe (`SDL_thylacineopengl.h`).
**Why OSMesa and not EGL**: OSMesa renders into a caller-supplied memory buffer —
here, the weave slot — so there is no window-system context to loader-resolve.
`THYLACINE_GL_CreateContext` creates the context with **`OSMESA_BGRA`**
(`:244`): a weave slot is `w*h` little-endian `0xAARRGGBB` words, i.e. bytes
B,G,R,A in ascending address order, which is precisely what OSMesa calls BGRA —
so llvmpipe rasterises *straight into* the pixels the compositor reads and §9's
"rendered into (or blitted into) the weave" resolves to "into". Changing that
constant reintroduces a full-frame conversion.

`gl_bind` (`:120`) binds the context to the window's **current** weave slot via
`OSMesaMakeCurrent(thyla_tap_pixels(...))`, then `OSMesaPixelStore(OSMESA_Y_UP,
0)` — the weave is top-down like every framebuffer while GL defaults to
bottom-left, so without this every frame presents vertically mirrored; it is
context state, set after the context is current and again after every re-bind.

### The weak-symbol link discipline — one libSDL2.a serves GL and non-GL

Every OSMesa entry point is `extern __attribute__((weak))`
(`SDL_thylacineopengl.c`, `:34-52`). A weak undefined reference resolves to 0 when
`libOSMesa.a` is absent, which is what lets **one `libSDL2.a` serve both GL and
non-GL programs** — `libOSMesa.a` plus its 73 LLVM archives is ~365 MB of link
input nobody wants in every SDL program. Redeclaring the symbols after
`<GL/osmesa.h>` with the weak attribute is what applies it (clang honours a weak
redeclaration of an already-declared function). `THYLACINE_GL_Available()`
(`:74`) ANDs *every* entry point — a partial link is not a configuration anyone
should get a half-working context out of. The **linking model**: a weak
reference does not extract archive members, so an SDL-GL program must force-link
its rasteriser — `-Wl,-u,OSMesaCreateContextExt` (the build does this at
`tools/build.sh`, `:4284`, and `gl_assert_resolved` nm-asserts the entry points
actually got pulled in). The same force-link shape governs the Vulkan half.

### The reweave hazard and the swap

A `TEV_CONFIGURE` reweave swaps the weave generation and **moves `map_va`**
(TAPESTRY §18.3), so a context bound to the old mapping would render into freed
pages. The context records what it was last bound to (`bound_va`/`bound_w`/
`bound_h`, `:56-64`) and `THYLACINE_GL_SwapWindow` (`:316`) re-binds *before*
`glFinish` if any of them changed — the re-bind is driven by the state that
actually changed, so it is correct no matter which layer noticed the resize
first. `glFinish` (`:355`) is the one GL call the backend itself makes:
llvmpipe rasterises on a thread pool, so it is what makes the present a present
rather than a race with partially-drawn tiles. Then `thyla_tap_present` with
full-surface damage (GL gives no damage information). `SetSwapInterval` (`:291`)
accepts only 0 (unthrottled) and 1 (frame tick) and refuses the rest honestly —
there is no back buffer to flip and no tearing to guard, so an adaptive or >1
interval has nothing to mean.

The **Warp-4 direct present** (`:46-52`, `:137-159`, `:378-385`) is negotiated on
*every* bind through the weak `OSMesaThylacineDirect`/`DirectOff` (mesa fork
patch 0007): the compositor displays the GL color resource itself (fullscreen
SET_SCANOUT of the 3D resource; windowed server-side readback + compose), the
OSMesa readback is suppressed, and the swap needs only `glFlush` (ordering is
the controlq FIFO). A reweave reallocates the framebuffer and the standing
consent names one BO, so it is re-issued for the new one (both handshake halves
idempotent server-side). The fork's export refuses on llvmpipe (a driver-name
gate), so a plain llvmpipe bind pays one cheap failed call. **Both halves must
land**: a half-negotiated state suppresses the readback with no display source —
a frozen pane — so any failure explicitly restores the readback (`:151-156`).
`DeleteContext` withdraws a live direct consent *first*, because the warp ctx
(per-process, on the shared screen) outlives the GL context and the consent
would otherwise dangle.

### The Vulkan surface is a two-sided consent (W-3e)

`SDL_thylacinevulkan.c` provides five hooks over stock `VK_EXT_headless_surface`;
the venus driver's W-3d WSI sits behind the headless slot. `Vulkan_LoadLibrary`
(`:95`) resolves the **weak** `vk_icdGetInstanceProcAddr` by symbol (no dlopen);
a GL-only program links with all three venus weaks NULL and fails *here* with a
specific missing-library message. `Vulkan_CreateSurface` (`:148`) creates the
headless surface via the stored gipa (hand-declared 3-field create-info, sType
1000256000) and then performs **the arming move** (`:184-204`) — the two-sided
consent that is this surface's whole soundness contribution:

1. read `pub = vn_renderer_thylacine_warp_ctx_pub()` (`:194`),
2. `thyla_tap_glsrc(tap, pub)` — the **surface half**, *first* (`:195`),
3. `vn_renderer_thylacine_set_surface(tap.id)` — the **ctx half**, only after
   the surface half accepted (`:196`).

Arming the ctx half only after the surface half lands is the invariant: no img
poke can ever name a surface that has not named the ctx back. A skipped or
failed consent is **not** a surface-creation failure (`:198-205`): it
`SDL_LogWarn`s and returns `SDL_TRUE`, and the surface presents display-inert.
The three venus symbols are all weak and degrade **together** — an SDL-only
Vulkan app (vkQuake) must link `-u vk_icdGetInstanceProcAddr`, and a program
linking the venus archive *without* it gets a working `LoadLibrary`=NULL failure,
never a half-armed consent. The server half of this handshake — `img_poke_complete`
and the display bind — is [[sub-tapestryd]]'s, bounded by [[inv-i40]]/[[inv-i45]].

### The framebuffer path is slot 0 of the weave

For a 2D (non-GL) window, `CreateWindowFramebuffer` (`:229`) hands SDL
`*pixels = thyla_tap_pixels(...)` — the framebuffer *is* weave slot 0, zero-copy —
with `ARGB8888` and `*pitch = tap.stride`. `UpdateWindowFramebuffer` (`:244`)
clips up to 63 dirty rects into a fixed `ThylaRect tr[63]` (an overflow-safe
clamp, audit F4), paces the frame, and issues one `thyla_tap_present` (a blocking
`t_write` whose Rwrite is completion; `n==0` means full-surface).
`DestroyWindowFramebuffer` is a no-op — the framebuffer is the weave slot, not a
private allocation.

### One pump thread, one bounded ring, one translation switch

Input is a dedicated pthread (`THYLACINE_PumpMain`, `SDL_thylacineevents.c`, `:44`)
that blocks on the tapestry event fid via `thyla_tap_read_events` and feeds a
bounded mutex ring (`q[256]`, **drop-newest on full**). `THYLA_TEV_FRAME` never
rides the ring — the pump consumes it, bumps `frame_seq`, and signals `frame_cv`
(`:59-66`), which is the pacing wake. `PumpEvents` (`:294`, the vtable hook)
drains up to 32 records under the lock on the **SDL main thread** and runs
`THYLACINE_HandleEvent` for each: `TEV_KEY` → `linux_scancode_table` →
`SDL_SendKeyboardKey` (+ `SDL_SendKeyboardText` for a printable rune),
`TEV_CONFIGURE` → reweave/`RESIZED` (or `EXPOSED`/letterbox for a non-resizable
window), `TEV_PTR_MOVE`/`PTR_REL` (relative-mode-aware), `TEV_PTR_BTN` (evdev
`BTN_*` → SDL buttons), `TEV_SCROLL`, `TEV_FOCUS`, `TEV_CLOSE`. `StopEventPump`
requests the surface close *then* joins — a sibling close cannot cancel a parked
read (#844), so the close is what wakes the pump to EOF.

### thyla_tap is a C mirror of libtapestry

`thyla_tap.c` binds the surface over blocking `t_read`/`t_write`, wire-identical
to the native Loom client: `thyla_tap_open` connects `/srv/tapestry`, mints via
`surface/new`, `create`s at W×H, maps the weave zero-copy (`t_weft_map`), and
opens the `present` and `event` fids; `thyla_tap_present` writes a 32-byte
header + inline rects for slot 0; `thyla_tap_glsrc` writes `glsrc <ctx>` (the
Warp-4 surface half, used by **both** the GL bind and the Vulkan arm);
`thyla_tap_reweave` maps-new-before-clunking-old on a generation swap; and
`thyla_tap_pixels` returns `map_va` (slot 0). The nine event kinds and the wire
sizes (`TEVENT_LEN 24`, `TPRESENT_LEN 32`) live in `thyla_tap.h`.

### The nogl fallback keeps libSDL2.a honest without Mesa

When the sysroot has no `<GL/osmesa.h>` (a fresh checkout, any sysroot rebuild),
`build_sdl2` compiles `thylacine-nogl/SDL_thylacineopengl_nogl.c` **instead of**
the real file (never alongside it; both include the same header, so a signature
change breaks whichever compiles). Every hook returns one honest error message;
`GL_Available()` is a hard `SDL_FALSE`; and the one behavioral subtlety it
preserves is `MakeCurrent(NULL, NULL)` returning 0 (SDL's unbind-teardown case),
matching the real backend. It carries no CAP_JIT, corvus, or OSMesa code at all —
those exist only in the real file, so a no-GL build cannot even reference the
capability path.

## Data structures

`SDL_WindowData` (`SDL_thylacinevideo.h`, `:30`): the `ThylaTap tap`; the pump
`pthread_t` + `pump_started`; the ring `ThylaEvent q[256]` with `q_head`/`q_len`
under a `pthread_mutex_t lock`; pointer state (`ptr_x`/`ptr_y`/`ptr_valid`); and
the pacing quartet `pthread_cond_t frame_cv`, `frame_seq`, `presented_seq`,
`nopace`. `SDL_VideoData` (`:59`): the single `SDL_Window *window` and
`gl_swap_interval` (default 1). `THYLACINE_GLContext`
(`SDL_thylacineopengl.c`, `:54`): the `OSMesaContext`, the `bound_va`/`bound_w`/
`bound_h` re-bind trigger, the `SDL_Window*`, and the `direct` flag. `ThylaTap`
(`thyla_tap.h`, `:64`): the root/ctl/weave/present/event fids, the id, the geometry,
and `map_va` (slot 0 pixels).

## Concurrency

Two threads per window and a clean split. The **pump thread** only ever blocks
on the event fid and writes the ring under `lock`; the **SDL main thread** reads
the ring under `lock` in `PumpEvents` and owns every `SDL_Send*` call, the GL
context, and the present. The only shared state is the mutex-guarded ring and
the `frame_cv`/`frame_seq` pacing pair (the pump signals, the main thread's
`PaceFrame` waits, 50 ms wall-clock bounded so a compositor that never ticks
cannot wedge a frame). The ring **drops newest on full** rather than blocking
the pump — an input storm loses the freshest events, never the pump's liveness.

## Invariants enforced

**I-42** (JIT-as-a-capability) is the dossier's reason to be audit:hard. SDL
acquires CAP_JIT on the program's behalf before llvmpipe JITs, by *asking* corvus
against the caller's own eligibility — nothing is granted in SDL. The invariant
has no dedicated `inv-` note in the vault yet; it is defined in ARCH §28 and the
CL-7k audit-trigger row, and its *enforcement* is the kernel's ([[sub-kernel-mmu]]
W^X + dual-map + I-cache; [[sub-kernel-caps]] CAP_JIT elevation-only + I-2/I-6
monotonic).

[[inv-i45]] (GPU authority bounded by the context): the Warp-4 direct present
and the Vulkan consent bind only the caller's own ctx — the arming order and the
per-bind re-negotiation keep a present naming the surface's own context, never
another's.

[[inv-i40]] (no torn scanout / surface-share integrity): the reweave re-bind
(render into the live mapping, not the retired one) and the arming order (a poke
can never name a ctx-less surface) are the client-side obligations; the server
half is [[sub-tapestryd]].

[[inv-i7]] (Burrow lifetime): the weave map is a `t_weft_map` share whose pages
outlive the client mapping per the dual-count rule; `thyla_tap_close` drops the
fids and the reweave maps-new-before-clunking-old so a present is never issued
against an unmapped slot.

## Error paths

`GL_LoadLibrary(path != NULL)` → error (no dynamic loading). `GL_CreateContext`
with no OSMesa → error+NULL; with CAP_JIT refused → `SDL_SetError` + NULL (no
context); with `OSMesaCreateContextExt` failure → free + error. A Vulkan surface
whose consent is skipped/failed → warn, present-inert, `SDL_TRUE` (not a
creation failure). `VideoInit` with `/srv/tapestry` unreachable → driver init
fails. `CreateWindow` a second time → refused (one window per process). A present
`t_write` failure → `SDL_SetError`. The nogl fallback turns every GL entry into
one honest message.

## Performance

Zero-copy is the point: the 2D framebuffer and the GL color buffer are both
weave slot 0, so a frame is one blocking `tpresent` write with no intermediate
copy (BGRA makes the GL path a straight rasterise-into-place). **LP_NUM_THREADS**
(`SDL_thylacineopengl.c`, `:208-230`): llvmpipe sizes its worker pool from Mesa's
`util_get_cpu_caps()->nr_cpus`, hardcoded 1 on this platform (the POSIX_LITE
`u_cpu_detect.c` carries no sysconf arm, #150), so the pool never spawned and an
`-smp 4` guest rasterised on one core. The backend seeds Mesa's own
`LP_NUM_THREADS` override with the real `SDL_GetCPUCount` (`sysconf`-backed,
honest since pouch 0032 reads `/ctl/sched`), the user's own value wins
(`overwrite=0`), a 1-CPU guest is untouched. Measured GLQuake demo1 (640×400,
`-smp 4`, HVF): 21.6 → 29.1 fps. Frame pacing is one 50 ms-bounded
`pthread_cond_timedwait`; `SDL_THYLACINE_NOPACE` opts out.

## Prosecution

- **The CAP_JIT acquisition must ASK, never grant.** The whole soundness of
  putting it in the platform layer rests on SDL manufacturing no authority —
  corvus decides against the caller. A future change that caches, forwards, or
  pre-acquires the capability across a fork would violate I-2/I-42; the acquire
  must stay a per-process self-walk.
- **A new graphics mode's slots must weak-check inside the hook**, not at the
  vtable — the unconditional wiring is what lets one `libSDL2.a` carry GL and
  non-GL, and a vtable-level gate reintroduces the 365 MB link.
- **A present must re-bind on a stale mapping.** The reweave moves `map_va`; any
  swap path that presents without the `bound_va`/`w`/`h` check renders into freed
  pages (I-40).
- **The Vulkan arming order is load-bearing.** Arm the ctx half before the
  surface half and a poke can name a surface with no context; the surface half
  must land first.
- **A half-negotiated direct present must restore the readback.** A consent that
  lands one half and not the other freezes the pane; every failure arm must
  fall back to the readback.
- **The pump ring must never block the pump.** Drop-newest is the discipline; a
  future blocking push deadlocks input against a slow main thread.

## Seams

- **The Vulkan consent's single-window argument is not multi-window-proof.** The
  ctx-half global (`vn_renderer_thylacine_set_surface`) carries only the surface
  id; the one-window-per-process rule is what guarantees a new `CreateSurface`
  re-arms before any present can name a stale surface. A future multi-window SDL
  would need the consent keyed per-surface, not per-process.
- **The direct present lights up only on a real GPU.** llvmpipe refuses the
  `OSMesaThylacineDirect` export (a driver-name gate), so the Warp-4 direct path
  engages on virgl/V3D and the shipping llvmpipe path is always the readback; a
  future software-direct would remove that gate.

## Caveats

- **The `-u` force-link requirement is a footgun, not a check.** A program that
  links `libSDL2.a` but forgets `-u OSMesaCreateContextExt` (or
  `-u vk_icdGetInstanceProcAddr`) gets a clean-but-surprising `DllNotSupported`,
  because a weak reference does not extract archive members. The build wires it
  for the in-tree programs; an out-of-tree port must know to.
- **The two `thyla_capjit.h` copies move together.** The SDL header-only copy and
  thylajit's own duplicate the corvus wire and the clearance name by design; a
  protocol change must touch both (grep-anchored, not enforced).
- **The input ring drops the newest event on overflow.** The 256-slot pump ring
  favours liveness over completeness — an input storm loses the freshest events,
  never the pump thread. A consumer that needs every event (a recorder) cannot
  assume the ring is lossless.

## Provenance

The G-7 arc: the tapestry video backend + zero-copy framebuffer (G-7a), the
OSMesa/llvmpipe GL path with the CAP_JIT acquisition (CL-7 §9 step 2 / #138), and
the W-3e Vulkan glue (the two-sided consent + the headless surface). Swept into
the vault by [[chg-2026-09-07-author-sdl-port]]. The CAP_JIT client is
`usr/lib/thylajit/thyla_capjit.h`; the clearance policy is [[sub-corvus]]; the
kernel enforcement is [[sub-kernel-mmu]] + [[sub-kernel-caps]].

## Tests

Three witnesses, at three link tiers. `glapi-probe.c` (#109) is a compile-and-
link assertion that never runs: it proves a stock SDL-GL program (the
`SDL_GL_*` call set from TyrQuake's `vid_sgl.c`) recompiles against the shipped
`libSDL2.a`, and `#error`s if `SDL_VIDEO_OPENGL` is off. `gl-sdl-prove` (#138)
is the GL-runs prover — it acquires CAP_JIT itself and prints which form won, so
the backend's acquisition is a proven no-op for a program that already holds it.
`thylacine_vk_sdl_prove.c` (mesa fork, W-3e) is the first-Vulkan-frame witness:
an SDL window → the glue → an instance via the SDL-handed gipa → the W-3d
swapchain → a real render pass → a copy-out pixel-pair check → three presents;
the gate requires **both** halves — the app PASS line and the compositor
`scanout direct N img res R` bind line. The interactive gates `ls-gfx-gl.exp`
and `ls-gfx-glquake.exp` ([[gate-interactive]]) drive a live GL session. The
nogl fallback is exercised by any sysroot without the Mesa headers — the build
swaps it in and `libSDL2.a` still links.
