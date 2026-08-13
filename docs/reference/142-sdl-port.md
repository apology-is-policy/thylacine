# 142 — The SDL2 port + the `SDL_thylacine` backend (G-7)

**Status**: as-built at G-7a (`632961ed`) + G-7b (`908d8bc6`) on `gfx-3`.
The SDL seam (`docs/TAPESTRY.md` §9/§17/§18.9) — the on-ramp that proves
the Tapestry API under a demanding non-Halcyon client (software Quake)
before Halcyon, the last-phase client, commits to it.

---

## Purpose

Make stock SDL2 programs run on Thylacine by mapping SDL's video/events
onto the audited tapestry compositor protocol — the §9 triple:

| SDL call | maps to |
|---|---|
| `SDL_CreateWindow` / `CreateWindowFramebuffer` | `thyla_tap_open` — mint a tapestry surface + `SYS_WEFT_MAP` its weave (slot 0, zero-copy) |
| `SDL_UpdateWindowSurface` / `UpdateWindowFramebuffer` | one blocking 32-byte `tpresent` write (the `Rwrite` IS the completion under the stage-0 synchronous engine → tear-free by construction) |
| `SDL_PumpEvents` | a pthread parked on the event fid → a bounded mutex ring drained + translated on the SDL thread |

A Thylacine SDL program is a **ported** program (Plan 9 split, ARCH §3.5):
it builds via Pouch (musl + the boundary-line patch series), NOT native
libthyla-rs. The compositor client is just files, so — like the pouch
AF_INET backend — the C driver re-implements the tapestry client rather
than linking the native `libtapestry` (that ported-links-native boundary
is a separate, unbuilt direction).

---

## Layout

```
third_party/SDL2/            SDL2 2.32.10 vendored PRUNED-PRISTINE (zlib)
                            PRUNE-MANIFEST.md = the reproducibility contract
usr/ports/sdl2/
  SDL_config.h              hand config (no autotools; the libsodium idiom)
  patches/0001-*.patch      bootstrap extern + array entry + the __thylacine__
                              dynapi off-arm (3 hunks vs the pristine tree)
  thylacine/
    thyla_tap.{c,h}         the C tapestry client (mirrors libtapestry::Surface)
    SDL_thylacinevideo.{c,h}   the video driver (CreateWindow/Framebuffer)
    SDL_thylacineevents.{c,h}  the event pump thread + PumpEvents translation
usr/sdl-probe/sdl-probe.c   the proving binary (/sdl-probe)
```

The vendored `third_party/SDL2` tree is never edited (the pouch/musl
idiom); `build_sdl2()` copies it into `build/pouch/sdl2-src`, applies the
`0001` patch, overwrites `include/SDL_config.h` with the hand config, and
copies OUR `thylacine/` driver in as `src/video/thylacine/`.

## The config + driver set (`SDL_config.h`)

The pruned tree IS the compile list (every `.c` under `src/` except
`src/main`). Driver selections: **video** = thylacine + dummy; **audio** =
dummy (no virtio-sound at v1.0 — §10 item 4); **thread** = pthread (pouch
patch 0004); **timer** = unix (`clock_gettime` = 75; `nanosleep` = torpor,
patch 0022); everything else = dummy/disabled stubs.

**The dynapi off-switch**: SDL deliberately `#error`s when
`SDL_DYNAMIC_API` is forced from the config/command line ("you have to
edit this file"), so the static-only, dlopen-less off-switch is an in-file
platform arm in `SDL_dynapi.h` — the `0001` patch adds a `__thylacine__`
arm (keyed on `build_sdl2`'s `-D__thylacine__=1`, the port-wide platform
macro), exactly like the PS2/Vita/3DS arms.

## `thyla_tap` — the C tapestry client

A 1:1 mirror of `usr/lib/libtapestry::Surface::open_on` over plain blocking
`t_open`/`t_read`/`t_write`/`t_close` + `t_weft_map` (`T_SYS_WEFT_MAP=82`,
added to header-only libt at G-7a). The native libtapestry client drives
the same fids through Loom, which is **wire-identical** to synchronous
read/write (a `LOOM_OP_WRITE` on a dev9p fid becomes an ordinary `Twrite`),
so the C client needs no Loom ring at all.

`open` sequence: connect `/srv/tapestry` → `surface/new` (mint + rebind the
fid onto the surface's ctl, the netd clone idiom) → `create W H` → read the
`weave` geometry → `SYS_WEFT_MAP` the weave → open `present` + `event`.

**Single-slot discipline**: the weave carries `WEAVE_SLOTS=3` for pipelined
native clients, but a synchronous client draws and presents slot 0 only —
the blocking present means the compositor never reads a slot the client is
still drawing, so one slot is tear-free by construction.

`reweave` (the §18.3 resize): on a size-changing `TEV_CONFIGURE`, write
`resize W H <serial>` (the `Rwrite` is the server's generation fence), open
a FRESH weave fid, re-read geometry, `SYS_WEFT_MAP` the new weave, THEN
clunk the old fid (map-new-before-clunk-old keeps the client mapped
throughout).

## The event pump

The tapestry event fid PARKS an empty read (the server's deferred-reply
mechanism), and `SDL_PumpEvents` must never block — so a dedicated pthread
blocks on the fid (`thyla_tap_read_events`) and feeds a bounded mutex ring;
PumpEvents drains it and translates on the SDL thread. Fd discipline: the
pump thread touches ONLY `event_fd`; every other fid (ctl/present/weave,
including the reweave's close-and-remap) stays on the SDL thread. Shutdown
RETIRES the surface (`thyla_tap_request_close` writes ctl `destroy`) — the
retire makes the event fid read EMPTY, the parked read returns 0, the pump
exits, join succeeds; only then does `thyla_tap_close` close `event_fd`.
(The G-7d F1 correction: closing the fd from a sibling thread does NOT
cancel a parked read — the #844 ref-held Spoor keeps the blocking read's
own ref, so the Dev close hook never runs. The retire is the real, bounded,
frame-clock-independent teardown signal.)

Translation: `TEV_KEY.code` is a raw evdev keycode → `SDL_Scancode` via the
stock `linux_scancode_table`; the compositor-resolved rune → `SDL_TEXTINPUT`
on press; a size-changing `TEV_CONFIGURE` acks + reweaves on the SDL thread
then reports `SDL_WINDOWEVENT_RESIZED`; `TEV_FOCUS`/`TEV_CLOSE` map to the
SDL window events.

**G-7c — the pointer path.** `TEV_PTR_MOVE` carries the surface-relative
position packed `x<<16|y` (TAPESTRY §18.4). In relative mode (Quake
mouse-look) translation computes deltas DRIVER-side from successive
positions and feeds `SDL_SendMouseMotion(relative=1, dx, dy)` — SDL
core's warp emulation needs a warpable host cursor this backend lacks,
so the video init installs a `SetRelativeMouseMode` hook that simply
ACCEPTS the mode (keeping core off the warp path). **Threading (the G-7c
audit F2 correction)**: ALL translation — the `relative_mode` read,
`ptr_x/ptr_y/ptr_valid`, every `SDL_SendMouse*` — runs on the SDL MAIN
thread inside `PumpEvents` (which drains the pump thread's ring); the
pump thread itself never touches SDL state. There is no cross-thread
race here, and translation must NOT be moved onto the pump thread —
`SDL_Mouse` state is unsynchronized. Non-relative mode forwards absolute
positions (SDL derives `xrel/yrel` internally). `TEV_PTR_BTN` maps evdev
`BTN_LEFT/RIGHT/MIDDLE/SIDE/EXTRA` → `SDL_BUTTON_LEFT/RIGHT/MIDDLE/X1/X2`;
`TEV_SCROLL`'s signed delta feeds `SDL_SendMouseWheel` (positive = up).
Edge behavior inherited from the tablet: the compositor clamps
out-of-surface positions to the far edge, so relative deltas die at the
boundary (the classic absolute-device limit — irrelevant for QMP/VNC
injection, which is positional).

**#51 — the FRAME-paced present (default ON).** A 60 Hz compositor can
only show 60 fps; presents beyond that overwrite un-composed pixels and
spin a vCPU (the uncapped timedemo ran ~600 fps with a 122–600 HVF
variance). `UpdateWindowFramebuffer` now waits for the compositor's next
FRAME tick before presenting: the PUMP thread bumps `frame_seq` +
signals `frame_cv` per `TEV_FRAME` it reads off the fid (driver-private
fields — the F2 rule keeps the pump off SDL state; FRAME no longer rides
the ring), and the present path does ONE `pthread_cond_timedwait`
bounded at 50 ms wall-clock (the G-5 F1 lesson: never a wake-count
bound). The bump cannot live at translation — the present's wait runs on
the main thread and would starve `PumpEvents` into timeout-only pacing.
Degradations are all bounded: a frozen/degraded frame clock (clock-rate
ctl, test-mode) or a HIDDEN pane (visible-only FRAME emission) paces at
~20 fps off the timeout — background throttling for free; teardown (the
pump exits after the retire, no further signals) is bounded by the same
50 ms; a spurious wake presents one tick early (pacing slack, never
correctness). `SDL_THYLACINE_NOPACE=1` opts out (benchmarks). The
ls-gfx-quake fps line becomes STABLE ≈ clock_hz — still the
deterministic 969-presents proof, minus the variance.

## `0022-pouch-nanosleep`

musl's `__clock_nanosleep` (which `nanosleep`/`usleep`/`clock_nanosleep`
route through) issued `SYS_nanosleep`/`SYS_clock_nanosleep` — both unwired
`0xFFFF` sentinels, so every pouch sleep returned `-ENOSYS` and `SDL_Delay`
busy-returned. The patch rewrites it onto the one wait-on-address primitive:
`SYS_TORPOR_WAIT` on a private stack word nobody wakes, using torpor's
relative-µs timeout (0 = spurious → re-loop; `-ETIMEDOUT` = the chunk
elapsed → re-measure the deadline on the requested clock; chunked under the
1-hour torpor clamp).

## Build

`build_sdl2()`: copy → patch → config-overwrite → glob-compile the pruned
tree (130 TUs, zero warnings) → `libSDL2.a` (1.5 MB) + headers →
`sysroot/include/SDL2/`. Then `/sdl-probe`. `tools/build.sh sdl2` builds it
standalone; `build_all` calls it before the ramfs bake.

## Proof: `/sdl-probe`

The first SDL program on Thylacine — `SDL_Init(VIDEO)` resolves the
thylacine bootstrap, `SDL_CreateWindow` mints a surface,
`SDL_GetWindowSurface` hands back weave slot 0, `SDL_UpdateWindowSurface`
presents — the whole §9 mapping through stock SDL API. Draws the quadrant
pattern + an animated sweep, pumps events, tears down. On the first live
run the compositor tiled the probe beside aurora and CONFIGURE-resized it,
so the reweave/generation path was exercised on run one; the screendump
pixel-count asserts all four quadrant colors on the scanout.

## Known caveats / seams

- **Fixed-size app in a tiling compositor**: TyrQuake picks a fixed
  640×480 window; the compositor tiles + resizes it. The driver handles the
  async `TEV_CONFIGURE` by reweaving + reporting `RESIZED` so SDL re-queries
  the surface; an app that caches a stale surface pointer past a resize
  faults its own mapping (the standard SDL re-query contract). A compositor
  "floating / fixed-size" surface mode (letterboxed games) is a Halcyon-era
  seam.
- **`SDL_Renderer` is software-only, deliberately, and stays that way**:
  SDL's `SW_RenderPresent` wraps the window framebuffer, so `SDL_Renderer`
  programs (like software TyrQuake) route through the framebuffer path.
  Enabling `SDL_VIDEO_OPENGL` (below) does **not** change this — that is a
  different consumer (`src/render/opengl/`, gated by `SDL_VIDEO_RENDER_OGL`,
  which stays undefined and whose sources stay pruned). Halcyon never needs
  GL (ARCH §17 two-tier rule); a GL program drives GL directly instead.

## OpenGL: the API surface (#109) and the context path (#138)

`SDL_VIDEO_OPENGL` is **defined** as of #109, so `libSDL2.a` carries the
full `SDL_GL_*` layer (all 20 entry points) and the public
`SDL_opengl.h` / `SDL_opengl_glext.h` headers ship in the sysroot. That
is the *API* half of LLVM-DESIGN §9 step 2. #138 supplies the driver
half: `usr/ports/sdl2/thylacine/SDL_thylacineopengl.c` implements the
nine `GL_*` hooks over Mesa's gallium OSMesa frontend on llvmpipe, so
`SDL_GL_CreateContext` returns a real context that JIT-compiles its
rasteriser through the I-42 dual-mapped code Burrow (CL-7b).

**The path is zero-copy, and that is not incidental.** A weave slot is
`w*h` little-endian `0xAARRGGBB` words — byte order B,G,R,A in ascending
address order, which is exactly what OSMesa calls `OSMESA_BGRA`. So
`OSMesaMakeCurrent(ctx, thyla_tap_pixels(tap), GL_UNSIGNED_BYTE, w, h)`
points llvmpipe straight at the pixels the compositor will read: §9's
"rendered into (or blitted into) the weave" resolves to **into**, with no
conversion pass. `OSMesaPixelStore(OSMESA_Y_UP, 0)` matches the weave's
top-down raster (OSMesa defaults to GL's bottom-up, which would present
every frame mirrored). `SwapWindow` is `glFinish()` then the existing
`thyla_tap_present` — the `glFinish` is load-bearing, not hygiene:
llvmpipe rasterises on a thread pool, so without it the compositor reads
partially-drawn tiles.

`GL_LoadLibrary` takes only `NULL` (there is no dynamic loader — the #115
lesson) and `GL_GetProcAddress` is `OSMesaGetProcAddress`. A
`TEV_CONFIGURE` reweave moves `map_va`, so the context is re-bound
whenever the swap path sees the mapping change — keyed on the recorded
binding rather than on the resize event, so it is correct regardless of
which layer noticed the resize first.

### Why the OSMesa symbols are weak, and what that cost

`libOSMesa.a` plus its 73 LLVM archives are ~365 MB of link input. A hard
reference from `SDL_thylacineopengl.o` would force **every** SDL program
to carry the whole rasteriser — `pouch-hello-sdl` would go from ~1 MB to
~70 MB, and a program with no interest in GL could not link at all. So
the OSMesa entry points are declared `__attribute__((weak))`: a program
that links `libOSMesa.a` gets a real context, one that does not gets a
clean `SDL_SetError` from `GL_LoadLibrary`. Both directions are checked
by two real links in `build_sdl2` (`sdl-probe` and the #109 API probe
link without it; `gl-sdl-prove` links with it) rather than by argument.

**The trap that arrangement sets, which it duly sprang:** a weak
*undefined* reference does not cause the linker to extract an archive
member. That is standard ELF, not an lld quirk. So the first GL link
scanned `libOSMesa.a`, pulled nothing, resolved all five OSMesa
references to 0, and **reported success** — a 138 MB binary whose GL path
was inert. `-Wl,-u,OSMesaCreateContextExt` adds a strong undefined symbol
that forces the extraction (one suffices; the member defining it defines
the rest). Since the whole design is arranged so that a broken GL link
still succeeds, "the link returned 0" carries no information, so
`build_sdl2` reads the symbol set back out of the artifact and fails the
build if any of the six stayed undefined. That check is revert-probed:
dropping `-u` alone reddens it, naming exactly the five.

`libSDL2.a` itself gains **no undefined `gl*` symbols** from the
`SDL_VIDEO_OPENGL` switch: every GL call inside `SDL_video.c` goes
through a pointer fetched by `SDL_GL_GetProcAddress`, never a direct
`gl*` reference. The GL dependency arrives only with the driver hooks,
and only weakly.

### The headers are optional too (#239)

Weak symbols made the **rasteriser** optional at *link* time. They did
nothing for the **headers** at *compile* time, and that asymmetry broke
the whole tree once.

`SDL_thylacineopengl.c` includes `<GL/osmesa.h>`. That header is not
vendored — it is fetched from the GCP builder into
`build/sysroot/include/` (`usr/ports/mesa/README.md`, "Fetching the GL
link artifacts"), and `build_sysroot` **recreates** `build/sysroot/`. So
its absence is not an exotic state: it is the state of every fresh
checkout, and the eventual state of every machine that ever rebuilds its
sysroot. Absent it, `build_sdl2` died on a `fatal error: 'GL/osmesa.h'
file not found` — and because SDL2 is built before the ramfs bake,
*nothing downstream ran at all*. A missing optional GPU stage took out
the kernel image.

The fix mirrors the link-time arrangement at compile time.
`usr/ports/sdl2/thylacine-nogl/SDL_thylacineopengl_nogl.c` implements the
same nine hooks plus `THYLACINE_GL_Available()`, names no GL type, and
reports the absence through `SDL_SetError` exactly as the weak path does.
`build_sdl2` selects **one of the two** by testing for
`$sysroot/include/GL/osmesa.h`: it copies the port's `thylacine/*.c` into
the throwaway source tree as before, then — when the header is absent —
deletes `SDL_thylacineopengl.c` from that copy and drops the nogl TU in
its place. The glob over the copied tree stays the compile list, so
exactly one GL backend is ever compiled and the two can never both
define the hooks.

Three things that are easy to get wrong here, and were:

- **A stub is not enough on its own.** `SDL_thylacinevideo.c` installs all
  nine `GL_*` hooks unconditionally (deliberately — see the comment
  there: a NULL `GL_CreateContext` makes SDL report a missing *driver*
  when the truth is a missing *library*). Simply *omitting* the GL TU
  therefore leaves nine undefined symbols and breaks the `sdl-probe`
  link. The nogl TU exists precisely so the vtable stays complete.
- **The mode is a cache-key input, and no timestamp can see it.**
  Fetching the headers into an otherwise up-to-date tree moves no file
  `build_sdl2`'s `find` stats, so the guard would have reported `REUSED`
  and kept serving the GL-less archive — and a later `gl-sdl-prove` would
  have linked the real `libOSMesa.a` against it. `build_sdl2` therefore
  records the mode in `$sysroot/lib/.libSDL2.gl-mode` and treats a
  mismatch as stale. It is written **last**, after `build_gl_sdl_prove`,
  so a build that dies midway leaves no sentinel and the next run
  rebuilds — the #138 output-half lesson applied to a third axis. The
  sentinel lives beside the archive in `$sysroot/lib` on purpose: both
  die together when `build_sysroot` recreates the sysroot, so they cannot
  disagree.
- **The skip is announced, never silent.** A build that quietly drops GL
  reads exactly like a build that has it. `build_sdl2` prints a `NO GL:`
  block naming the missing header and the fetch instructions, and the
  ledger line carries the mode (`libSDL2.a: BUILT (nogl, + sdl-probe)` /
  `REUSED (cached + up-to-date, gl)`).

Verified by a four-step ladder rather than by argument: (A) nogl → cache
reports `REUSED`; (B) a `GL/osmesa.h` appears → **rebuilds** in `gl`
mode, compiling `SDL_thylacineopengl.c`, with no other input changed; (C)
the header is removed → **rebuilds** back to `nogl`; (D) the sabotage —
the header present *and* the sentinel hand-forged to `gl` → reports
`REUSED` and leaves the nogl object in place, which is what proves the
sentinel, and not some incidental timestamp, is the thing that caused the
rebuild in (B).

The limit of that evidence, stated plainly: (B) used a hand-written
stand-in `GL/osmesa.h`, because this tree has no Mesa headers to test
with. It proves the **selection** flips and the real TU still compiles
against a conforming header. It does **not** prove the real TU compiles
against the real Mesa header — only a tree with the fetched headers can
say that, and the `gl` path is unchanged from what already worked there.

### Swap interval

`SDL_GL_SetSwapInterval` accepts 0 (present as fast as drawn) and 1 (wait
for the compositor's frame tick) and refuses anything else honestly
rather than silently rounding. There is no back buffer to flip and no
tearing to guard against — a present is a blocking write the compositor
completes inside one dispatch — so a negative (adaptive) interval has
nothing to adapt and >1 would mean dropping frames on purpose. The
default is **1**, which differs from SDL's own default of 0 deliberately:
on a host GL driver an unthrottled swap costs the app nothing, here it
overwrites un-composed pixels and spins a vCPU (the #51 measurement).
Interval 1 routes through the same `THYLACINE_PaceFrame` the framebuffer
present uses — one pacing policy, not two.

### The prover

`usr/gl-sdl-prove/gl-sdl-prove.c` is the GL twin of `sdl-probe`: it draws
sdl-probe's exact quadrant pattern (TL red / TR green / BL blue / BR
white) with the rasteriser instead of `memset`, plus a magenta triangle
through the full fragment pipeline, swaps 8 frames, then `glReadPixels`
all five regions back and checks them. Reusing sdl-probe's pattern is
deliberate — the screendump family already knows how to check it, so the
rendering path is the only variable between the two provers.

Its **exit code is the verdict**: every station returns a distinct
non-zero, so a gate can treat `rc == 0` as a strong assertion and parse
nothing (the CL-7b lesson — a prover that only *printed* its failures let
a boot with no `CAP_JIT` run on and report a generic NULL-context error
indistinguishable from three other faults). It asserts `GL_RENDERER`
contains `llvmpipe`, which is not cosmetic: a context on some stub would
satisfy every other check while proving nothing about CL-7. Driven by
`tools/interactive/ls-gfx-gl.exp`; staged to `/clade/bin/` rather than
the ramfs because it is a 143 MB static binary.

It also needs `CAP_JIT`, which it acquires **for itself** — llvmpipe JITs
inside the calling process and `CAP_JIT` is elevation-only, so a GL program
on Thylacine is a capability client before it is a renderer. Post-login it
takes corvus's SELF form (verb 18, #139), authorized on the kernel-stamped
principal with no bearer token; the gate asserts the form, not merely the
acquisition. See `docs/reference/102-legate.md`.

## The backend acquires `CAP_JIT` on the program's behalf

`THYLACINE_GL_CreateContext` walks the corvus clearance path itself, before
it creates the OSMesa context. That is deliberate and it is what makes §9
step 2's claim — "stock SDL-GL programs recompile" — literally true.

The alternative is every GL program calling `thyla_acquire_cap_jit()` at
startup, which is what gl-sdl-prove does. That works for a prover written
against this platform. It does not work for a *port*: TyrQuake's `vid_sgl.c`
is 1996-lineage SDL-GL code that has never heard of Thylacine, and requiring
it to speak a capability protocol before `SDL_GL_CreateContext` would mean a
Thylacine-shaped patch in that port and in every future one — at which point
the recompile claim is false.

The application has already declared its intent by asking for a GL context;
on this platform that request *is* a request to JIT, and carrying a fact like
that is what a platform abstraction layer is for. **SDL grants nothing** — it
asks, and corvus decides against the calling principal's own eligibility, so
a user with no `jit` clearance gets a clean `SDL_SetError` and a NULL context
rather than a `Failed to materialize symbols` three layers down.

The acquisition is guarded by a weak-global once-flag in `thyla_capjit.h`, so
the two callers compose in either order and neither double-walks: gl-sdl-prove
links *both* its own copy of the header and libSDL2.a's, and a function-static
would have been per-TU. (A weak *definition* merges across TUs and out of an
archive; a weak *undefined reference* does not extract an archive member at
all, which is the distinction that cost #138 a link that succeeded with an
inert GL path. Both topologies were compiled and their addresses compared
rather than assumed.)

Placement inside `CreateContext` is after the `THYLACINE_GL_Available()`
check, so a program that links no rasteriser never opens a corvus connection,
and before the allocation, so a refusal has nothing to unwind.

**Everything downstream of that acquisition ran for the first time at #139**,
because until then the prover always died at the capability gate. Its first
real execution found two bugs that had shipped with #138 and could not
previously surface, and they are worth stating because the shape recurs:

- The quadrant readback sampled at (W/4,H/4) and (3W/4,H/4). The triangle is
  drawn UNSCISSORED and its NDC span maps to screen x∈[160,480], y∈[100,300]
  at 640×400 — so those two samples land *exactly* on its bottom vertices. BL
  read magenta and the assertion, not the readback, was wrong. Samples moved
  to 1/8 and 7/8; the geometry is written down beside them.
- Both of `ls-gfx-gl.exp`'s regexes spelled the renderer `[^ ]+`, and
  `GL_RENDERER` is `llvmpipe (LLVM 22.1.8, 128 bits)` — spaces. Neither had
  ever matched. The scenario reported "no renderer line within 180s" while
  the log plainly contained it.

Each fix advanced the prover one station and handed the next its first run.
An assertion that has never executed is not a passing assertion; it is an
unknown, and five of these were unknowns until #139 unblocked them.
- **The present is synchronous** (stage-0 tapestryd): each present blocks on
  the compositor. The pipelined-controlq drain (with a real quiesce before
  retire) is the recorded obligation; timedemo throughput (~600 fps at
  640×480) shows the synchronous path is fast enough for the gate.

See `docs/reference/143-tyrquake.md` for the Quake port + gate.
