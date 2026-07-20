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

## The three port fixes (`0001` patch + `0023-pouch-fopen`)

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

## Known caveats / seams

- Mouse-look (virtio-tablet → `TEV_PTR`) is G-7c — keyboard already plays
  Quake, so the gate is met without it.
- No sound (`-nosound`) — virtio-sound is unbuilt; game audio waits on the
  audio server (§10 item 4).
- The fixed-size-window-in-a-tiling-compositor friction is the doc-142
  seam.
