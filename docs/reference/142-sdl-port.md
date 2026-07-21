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
- **Software renderer only**: GL is v1.1+ (a Mesa port), never on Halcyon's
  path (ARCH §17 two-tier rule). SDL's `SW_RenderPresent` wraps the window
  framebuffer, so `SDL_Renderer` programs (like TyrQuake) route through the
  framebuffer path.
- **The present is synchronous** (stage-0 tapestryd): each present blocks on
  the compositor. The pipelined-controlq drain (with a real quiesce before
  retire) is the recorded obligation; timedemo throughput (~600 fps at
  640×480) shows the synchronous path is fast enough for the gate.

See `docs/reference/143-tyrquake.md` for the Quake port + gate.
