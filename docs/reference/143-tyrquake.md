# 143 — TyrQuake port + the G-7 acceptance gate

**Status**: as-built at G-7b (`908d8bc6`) on `gfx-3`. The `docs/TAPESTRY.md`
§17 acceptance gate — "if SDL/Quake maps cleanly, the API is proven before
the riskiest client (Halcyon) is built."

---

## What it is

TyrQuake 0.71 (Kevin Shanahan's maintained Quake port; GPLv2), the **NQ
single-player, software-renderer, SDL** build, cross-compiled via Pouch
against `libSDL2.a` + the `SDL_thylacine` backend (doc 142). Original Quake
shipped a software rasterizer, which is exactly why it is the §9 gate title
— a 2D milestone gated only on the SDL→Tapestry backend, no GL.

`+timedemo demo1` renders + presents **969 frames** to the compositor
scanout at **~550–600 fps** and shows Quake's textured 3D world live on the
display, tiled beside the aurora console.

## Layout

```
third_party/tyrquake/        TyrQuake 0.71 vendored PRUNED-PRISTINE (GPLv2)
                            PRUNE-MANIFEST.md (QW/launcher/wine/icons dropped)
usr/ports/tyrquake/patches/0001-*.patch   two guards vs the pristine tree
tools/interactive/ls-gfx-quake.exp + gfx_fp.py   the gate scenario
```

`build_tyrquake()` (in `tools/build.sh`) copies the vendored tree, applies
the `0001` patch, and glob-compiles a **curated object list** (mirroring the
upstream Makefile's COMMON/CL/SV/NQCL/SW groups + the sdl/null driver
selections; the x86-asm `.S` files stay unbuilt on aarch64) → `tyr-quake`
1.8 MB static ET_EXEC.

## The shareware data

`pak0.pak` (id shareware 1.06) is fetched at BUILD time from the id
`quake106.zip` installer (sha256-pinned) and extracted with the host
`bsdtar` (macOS libarchive reads the Deice/LHA `resource.1` natively — no
`lha` dependency). Staged lowercase to `build/quake/stage/id1/pak0.pak`,
`populate_stratum_pool` puts the stage at `/quake` (the compiled-in
`QBASEDIR`). The pak is **never committed** — build-time fetch only.

## The four port fixes (`0001` patch + `0023-pouch-fopen`)

Applied to a build-dir COPY (the SDL2/musl idiom); the vendored tree stays
byte-pristine.

1. **`S_ClearOverflow` NULL-deref** (`snd_dma.c`): with `-nosound` (no
   virtio-sound at v1.0) `S_Init` returns before allocating the static
   `known_sfx`, but `Host_ClearMemory` calls `S_ClearOverflow` on every map
   load, and it derefs `known_sfx->overflow` with NO guard — while its
   siblings `S_StopAllSounds`/`S_ClearBuffer` both bail on `!sound_started`.
   A genuine upstream omission; the fix adds the matching
   `!snd_initialized || !known_sfx` guard.
2. **`setvbuf(stdout, _IONBF)`** (`sys_unix.c`): the NQ `Sys_Printf` never
   `fflush`es, so block-buffered stdout stalls the demo when the
   `Con_Printf` burst back-pressures on the pts (correct POSIX flow-control
   — the pre-fix "frozen 260s" was the block-flush blocking on the drain,
   NOT a hang; verified by the fact that the ONLY change was the buffering).
   Console programs commonly run unbuffered.
3. **`0023-pouch-fopen`** (a musl boundary-line patch, NOT tyrquake): musl's
   stdio openers (`fopen`/`__fopen_rb_ca`/`tmpfile`) call the RAW `sys_open`
   MACRO (`SYS_openat`, an unwired `0xFFFF` sentinel), bypassing the
   0009/0021 boundary-line `openat()` FUNCTION. No prior pouch program
   `fopen`ed by path (stratumd uses `open()`; the hello probes use stdio on
   inherited fds), so the gap sat latent until TyrQuake's
   `fopen(pak0.pak)` silently found no pak. Rerouted through the patched
   `open()` (SYS_open=65 stalk resolution).
4. **`S_UnblockSound` missing from the null driver** (`snd_null.c`, CL-7 §9
   step 3): `sound.h` declares the `S_BlockSound`/`S_UnblockSound` pair and
   every other stub driver (`snd_oss`, `snd_sndio`) defines both, but
   `snd_null.c` defines only the first — the same shape of upstream omission
   as fix 1. `vid_sdl.c` calls neither, so the SOFTWARE build links happily;
   `vid_sgl.c` calls both around `VID_SetMode`, so **tyr-glquake is the first
   configuration in this tree that can see it**, as an undefined symbol at
   link rather than a runtime fault.

   Worth recording for the mechanism rather than the symbol: the fix did not
   work the first time, and it *reported success*. The hunk header promised
   seven new lines where the body had eight, and `patch` believes the header —
   it applies the first seven and discards the rest, silently, exit 0, no
   `.rej`. The discarded line was the function definition; the comment above
   it survived. The check that missed it was `grep -c S_UnblockSound`, which
   counts a string the comment and the definition BOTH contain, so it could
   not distinguish "the fix landed" from "only the comment landed". The
   discriminating check is the exact definition line. `tools/check-patch-hunks.py`
   now validates every hunk count in the tree at the top of `build.sh`.

## The kernel dependency (G-7b)

`EXEC_USER_STACK_SIZE` 256 KiB → 1 MiB (`kernel/include/thylacine/exec.h`).
TyrQuake's model loader (`Mod_ForName` → `Mod_LoadAliasModel`, large
on-stack temp buffers) overflowed the 256 KiB main stack into the guard
page — correctly caught by the P5 guard VMA (`snare:segv`, not corruption).
1 MiB is eager-anon; the 1 GiB gap above `STACK_TOP` (0x80000000) leaves
headroom. The Linux-model lazy demand-grown stack (commits only touched
pages) is the tracked v-next lift (doc: the overcommit
`BURROW_TYPE_ANON_LAZY` infra already exists). User-voted "eager 1 MB now,
lazy later" 2026-07-20.

`proc.c::proc_fault_terminate` now also prints the faulting EL0 `pc`+`lr`
(from the #88 `debug_trapframe`) — a static non-PIE pouch pc symbolizes
directly against its ELF via `llvm-symbolizer`, which is how all three port
faults above were root-caused. A diagnostic read of an existing field on
the fault path; no new concurrency.

## The gate (`ls-gfx-quake.exp`)

Runs under HVF (the ls-gfx sibling posture — TCG hangs at the pre-existing
8a-2b hwwatch E2E, task #27):

- **engine leg**: `======= Quake Initialized =======` proves the full boot
  (pak loaded, SW renderer up); the `<N> frames <t> seconds <fps>` line is
  the DETERMINISTIC present proof — 969 frames cannot complete without every
  present landing on the scanout.
- **render leg**: a screendump carries Quake's textured-world color richness
  (the HARD gate is a color-bucket floor — a flat/black surface fails it); a
  frame-advance delta between two dumps is LOGGED, not gated (post-timedemo
  Quake may idle on a static console frame, so gating on it would be
  timing-flaky).

`gfx_fp.py` fingerprints a PNG as `<rolling-hash> <color-buckets>` (stdlib
PNG decode, no PIL) for both legs.

## `tyr-glquake` — the CL-7 §9 step 3 acceptance gate

The GL sibling: same engine, `GL_OBJS` + `vid_sgl.c` instead of `SW_OBJS` +
`vid_sdl.c`, `-DGLQUAKE`, linked against `libOSMesa.a` + the 73 LLVM archives
so it rasterises through llvmpipe. §9 calls it "the poetic echo of G-7" —
software Quake proved the 2D present path, GLQuake proves the GL stack,
through llvmpipe, through the JIT capability, onto the same scanout.

`build_tyrquake()` builds both from one source copy. Upstream's own
`nqsw-list` and `nqgl-list` are `COMMON + CL + SV + NQCL + <renderer> +
<video driver>` and differ in exactly those last two, so the object lists here
are written the same way — shared part once, the two differences separately.
Two flat lists would be free to drift apart, and only one of them has a gate
watching it. The objects go to **separate directories**, which is required
rather than tidy: `-DGLQUAKE` changes the shared sources too (`screen.c`,
`view.c`, `cl_main.c` all carry `#ifdef GLQUAKE`), so a `cl_main.o` built for
one renderer is wrong for the other.

`gl_model.o` joins `model.o` rather than replacing it — upstream's own list,
not an oversight: `model.c` keeps the format-independent loader and
`gl_model.c` adds the GL-side surface/texture build.

Output is ~145 MB, so it lands in `build/clade/gl/` and is staged to
`/clade/bin/tyr-glquake` (stripped, ~70 MB) like `gl-sdl-prove` and
`osmesa-prove`, rather than into the ramfs. Data still comes from `/quake`,
which the software build already bakes. Optional and **announced** when
skipped: the GL archives are fetched from the GCP builder
(`usr/ports/mesa/README.md`), so a fresh checkout legitimately has none.

### What it does NOT contain

**No Thylacine patch for the capability.** `vid_sgl.c` is stock SDL-GL code;
`CAP_JIT` is acquired by the SDL backend on its behalf (doc 142, "The backend
acquires `CAP_JIT` on the program's behalf"). That is the whole reason this is
an *acceptance* gate rather than another prover — gl-sdl-prove was written
against this platform, TyrQuake was not. If reaching a GL context required a
port patch, §9 step 2's "stock SDL-GL programs recompile" would be false, and
every future GL port would carry the same patch. The gate asserts the prefix
`sdl-gl:` on the acquisition line precisely to catch a regression into
patching the port.

### The gate (`ls-gfx-glquake.exp`)

Four legs, HVF, skipping cleanly when the GL half is not baked:

- **capability**: `sdl-gl: CAP_JIT acquired (SELF)`. Asserted first because
  without it the failure surfaces three layers away as llvmpipe's "Failed to
  materialize symbols" — a Mesa-shaped message for a capability-shaped
  problem.
- **renderer**: `GL_RENDERER:` contains `llvmpipe`. Matched to end-of-line,
  because the string is `llvmpipe (LLVM 22.1.8, 128 bits)` and a `[^ ]+`
  pattern is exactly the bug that made ls-gfx-gl's two regexes never match
  anything (#139).
- **engine**: `Quake Initialized`, through the GL renderer.
- **present**: the timedemo's `<N> frames <t> seconds <fps>` line, plus the
  screendump colour-bucket floor. **fps is reported, not gated** — §9 commits
  no budget beyond "GLQuake is playable, measured honestly at CL-7", and a
  number nobody has measured yet is how a flaky threshold gets born.

A third trap lives in that renderer regex, and it pulls the OPPOSITE way from
#139's. `([^ ]+)` was too STRICT and never matched; `([^\r\n]*)` with nothing
after it is too PERMISSIVE and matches a PREFIX — expect tests the buffer as it
fills, and a trailing `*` is satisfied by whatever has arrived, including
nothing. Measured, not theorised: three consecutive attempts captured `llvm`,
`''` and `''` from a guest printing the same string each time, because the
capture tracked where the socket read happened to split. The trailing `[\r\n]`
is what makes the match wait for a terminated line. ls-gfx-gl escapes this only
by accident — its pattern requires a literal ` version=` after the group.

### First measurement (CL-7, HVF, 640×400)

```
969 frames in 48.9s @ 19.8 fps    llvmpipe (LLVM 22.1.8, 128 bits), GL 4.6
```

Honest reading: **playable, and genuinely rasteriser-bound.** The present path
is frame-paced at ~57–60 fps (doc 142 / #51), so 19.8 is well clear of the cap
and is real llvmpipe cost, not pacing. It is ~3× slower than the software
renderer's 969 frames at ~57–60 fps, which is the expected direction — Quake's
software path is a hand-tuned span rasteriser written for exactly this scene,
while llvmpipe is doing general per-fragment texture filtering through
JIT-compiled shader variants. No budget is committed here; the number exists so
a later change has something to be compared against.

### Second measurement (#150): the rasteriser pool engages

The 19.8 was measured with llvmpipe rasterising **inline on one thread** —
`nr_cpus` was a hardcoded 1 on this platform (doc 142, the `LP_NUM_THREADS`
default section). The #150 decomposition, boot-per-run on the same demo1
gate command (HVF, -smp 4; a fresh guest per number):

```
inline (pre-fix shipped)      969 frames  44.8s  21.6 fps
wrapper LP_NUM_THREADS=0      969 frames  45.9s  21.1 fps   (control == inline)
wrapper LP_NUM_THREADS=3      969 frames  37.8s  25.6 fps
wrapper LP_NUM_THREADS=4      969 frames  33.3s  29.1 fps
post-fix shipped (no config)  969 frames  33.1s  29.3 fps   (and one 37.4s/25.9 outlier)
```

The shipped path now self-defaults the pool to the CPU count (pouch 0032
sysconf + the SDL glue seed), so the plain gate command does **21.6 →
29.3 fps (+36%) with zero configuration**. Sublinear vs 4 workers'
theoretical ceiling because ~2/3 of frame time is serial on the calling
thread. Threaded runs show wider single-run spread
than inline ones (25.9–29.3 across two post-fix boots); quote the pair,
not one number. fps remains REPORTED, not gated.

### The host reference (2026-08-05): uncapped M2, same demo, same 640×400

Host tyr-glquake (macOS arm64, same vendored tree, brew SDL2, Apple's GL
2.1-on-Metal — i.e. the **M2 GPU**, not a software rasteriser):

```
host, vsync ON (default)      969 frames  16.3s    59.6 fps   (display cadence, NOT a perf number)
host, swap interval forced 0  969 frames   0.5s  2061.8 fps
```

Two conclusions this pins:

- **The vsync trap**: tyrquake applies `vid_vsync` only at video-mode set,
  which happens before `+cvar` commands execute — so `+vid_vsync 0` on the
  command line silently does nothing and the "benchmark" reports the
  display's refresh rate. The uncapped number required forcing
  `SDL_GL_SetSwapInterval(0)` at the call site (scratch build). Any host
  number that sits at ~59.6 or ~120 is the compositor talking, not the GPU.
- **The serial share is Mesa-side, not game logic**: the host does engine +
  GL dispatch + GPU in ~0.5 ms/frame, and HVF guest CPU is near-native — so
  the guest's ~31 ms/frame serial floor is ≥98% software-GL caller-side work
  (immediate-mode dispatch through the state tracker + llvmpipe's
  caller-thread vertex/setup/binning; `LP_NUM_THREADS` parallelises only the
  fragment side). The earlier "the engine is the lever" reading is
  **refuted**. #155 owns the decomposition (`LP_PERF=no_rast` partition);
  the ~70× host-GPU gap is the price of software rasterisation itself,
  closable only by a GPU-in-guest arc, not by tuning this stack.

### Third measurement (#155): the serial share partitioned, flag-free

Same demo1 gate command, all four boots same-day (2026-08-06, HVF -smp 4;
threaded spread is wide, so comparisons are within-day only):

```
base            640x400  4t   969 frames  37.5s  25.8 fps   (38.8 ms/frame)
LP_PERF=no_rast 640x400  4t   969 frames  36.9s  26.3 fps   (38.1)
LP_PERF=no_rast 640x400  0t   969 frames  57.0s  17.0 fps   (58.8)
base            320x200  4t   969 frames  32.6s  29.7 fps   (33.6)
```

- **`LP_PERF=no_rast` does NOT engage in this build.** The parse table is in
  the binary (`strings` shows it) but the pool moved a no_rast run 26.3 →
  17.0 fps — impossible if rasterisation were actually skipped, since
  nothing else is pool-parallel. The env delivery itself was proven per-run
  (`setrun ok`). Enable/verify LP_PERF at the next builder cycle (batched
  with the owed `DETECT_OS_THYLACINE` sysconf arm).
- **The resolution probe partitions without any flag**: removing 75% of the
  pixels saved 5.2 ms/frame → pixel-proportional cost visible past the
  4-worker overlap ≈ **7 ms (18%)**; the remaining ≈ **32 ms is
  pixel-independent** — matching the host-GPU-derived ~31 ms caller-side
  floor. Two independent instruments agree: the wall is per-draw/per-vertex
  caller-side work (immediate-mode dispatch + llvmpipe caller-thread
  vertex/setup/binning), not fragment rasterisation and not game logic.
- Implied asymptote at 640×400 with pixels free: ~31 fps — more threads and
  lower resolution are exhausted levers. The remaining lever with real
  headroom is cutting per-call caller-side cost (vertex arrays in the port
  — engine surgery, deliberately not done in #141; a scope decision).

## Known caveats / seams

- Mouse-look (virtio-tablet → `TEV_PTR`) is G-7c — keyboard already plays
  Quake, so the gate is met without it.
- No sound (`-nosound`) — virtio-sound is unbuilt; game audio waits on the
  audio server (§10 item 4).
- The fixed-size-window-in-a-tiling-compositor friction is the doc-142
  seam.
