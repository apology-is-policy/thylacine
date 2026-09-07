---
id: sub-tyrquake
type: sub
title: "TyrQuake — the GL Quake port: the ramfs launcher, the GPU-preference /env write, and the two boundary-line patches"
parent: moc-userspace
code: [usr/ports/tyrquake/tyr-glquake-launcher.c, usr/ports/tyrquake/patches/0001-tyrquake-nosound-guard.patch, usr/ports/tyrquake/patches/0002-tyrquake-condebug-fd.patch]
audit: light
guarded-by: [inv-i38]
validated-by: [prose, gate-interactive]
locks: []
hazards: []
abis: []
design: ["docs/LLVM-DESIGN.md"]
created: 2026-09-07
updated: 2026-09-07
---
## Purpose

TyrQuake is the worked example that closes `LLVM-DESIGN.md` §9 step 3: an
ordinary SDL-GL application, unpatched at the graphics layer, running on
Thylacine because [[sub-sdl-port]] carries the platform facts (CAP_JIT
acquisition, OSMesa-into-weave) for it. This dossier is not the game engine —
it is the two things Thylacine adds around a pristine TyrQuake tree: a small
**ramfs launcher** that gives the pool-resident GL binary a bare shell name and
prefers the GPU when one is present, and two **boundary-line patches** that fix
a null-deref and a log-path bug the port surfaced. It holds no capability and no
§28 invariant of its own; its one soundness-adjacent obligation is respecting
the guest FS cache's close-to-open contract ([[inv-i38]]).

## Contract

`tyr-glquake` resolves from the shell as a bare name, but the real GL binary
lives at `/clade/bin/tyr-glquake` — the GL build (llvmpipe + LLVM) is too large
for the ramfs, so it is staged into the clade pool. The launcher is the ramfs
face that `execv`s the pool binary; a later union bind of `/clade/bin` onto
`/bin` supersedes the need for it, but it remains the documented shape. argv is
passed through unchanged.

## Mechanism

### The launcher prefers the GPU through a surviving /env write

`prefer_virgl_when_present` (`tyr-glquake-launcher.c`, `:50`) is the Warp-4 GPU
preference and does **no** capability work. If `GALLIUM_DRIVER` is already set
it returns immediately (`:57-58`) — an explicit choice is a default it must not
override. Otherwise it attaches `/srv/warp`, walks `ctl`, and reads a leading
`virgl 1` (`:62-71`); on a match it writes `virpipe` to `/env/GALLIUM_DRIVER`
via `open(..., O_WRONLY|O_CREAT) + write` (`:73-79`). It rides a **/env write**
(patch 0025's environ snapshot), not `setenv`, precisely so the value survives
the exec/spawn into the pool binary. There is no CAP_JIT here — that is acquired
later, by the SDL GL backend, on the program's own behalf ([[sub-sdl-port]]).

### The launch is execve-first, spawn-and-forward as the fallback

`main` (`:90`) calls the GPU preference, then tries `execv(TARGET, argv)`
(`:94`) as the primary arm. Because pouch had no `execve` until LINEAGE L-6
(patch 0026), a return from `execv` is expected on older builds: it then
installs `forward_interrupt` on SIGINT/SIGTERM (`:107-108`, which `kill`s the
child), `posix_spawn`s the target (`:110`), and reaps with a
`waitpid(WNOHANG)` + `usleep(50 ms)` poll loop (`:119-126`) — the 50 ms is the
^C-forwarding latency bound. It returns the child's `WEXITSTATUS`, or 127 if the
target is unreachable.

### Two boundary-line patches, applied to a build-dir copy

`third_party/tyrquake` stays pristine; `build_tyrquake` (`tools/build.sh`, `:4334`)
copies it, applies these patches, fetches the sha256-pinned `quake106.zip`
shareware pak, and builds both `tyr-quake` (software) and `tyr-glquake` (GL,
staged to the clade pool).

**0001 — nosound-guard** (G-7b): three hunks. `snd_null.c` gains
`S_UnblockSound(){}` — `sound.h` declares it and `vid_sgl.c` calls it around
`VID_SetMode`, but the null driver defined only its `S_BlockSound` half, an
undefined symbol the GL link surfaced first. `snd_dma.c` gains an
`if (!snd_initialized || !known_sfx) return;` guard at the top of
`S_ClearOverflow` (mirroring `S_StopAllSounds`/`S_ClearBuffer`), which fixes a
NULL deref of `known_sfx->overflow` — `snare:segv addr=0xb008` — on the first
map load under `-nosound`. `sys_unix.c` gains `setvbuf(stdout, NULL, _IONBF, 0)`
so `Con_Printf` progress reaches the serial tee in real time.

**0002 — condebug-fd** (#232): one rewrite of `Sys_DebugLog` (the `-condebug`
sink), fixing two bugs. First, it pins the log **path**: `com_gamedir` is empty
for most of a run, so the sink alternated between `<gamedir>/qconsole.log` and a
bare `/qconsole.log` (EACCES); the rewrite remembers the first path that worked
and reuses it, reports each distinct failing path once via `strerror`, and
checks the `write` result instead of discarding it. Second — and this is the
[[inv-i38]] obligation — it **keeps the per-line `close(fd)`**: each line is
published under Thylacine's close-to-open guest FS cache ([[sub-kernel-larder]]),
so holding the descriptor open across writes would leave a concurrent reader
seeing 0 bytes. The per-line open/write/close is load-bearing, not wasteful.

## Data structures

None of its own — the launcher carries a `static pid_t g_child` (the reap target
for `forward_interrupt`) and the patches add only file-scope statics in
`Sys_DebugLog` (`good[1024]`, `refused[1024]`, `wr_complained`).

## Concurrency

Single-threaded launcher. The only cross-actor concern is the ^C forwarding: a
console interrupt reaches the launcher, which relays it to the spawned child by
PID and polls for the reap — the 50 ms poll is the latency bound on that relay.

## Invariants enforced

[[inv-i38]] (guest FS cache close-to-open coherence): the `-condebug` sink's
per-line `close` is what publishes each log line to a concurrent reader under
[[sub-kernel-larder]]'s close-to-open contract. It is the only load-bearing
invariant TyrQuake touches; it upholds it by *not* optimizing the close away.

## Error paths

Target unreachable → exit 127. `execv` returns (no execve on the build) → fall
through to `posix_spawn`. `posix_spawn` failure → propagated. The `-condebug`
sink reports a refused path once and continues (logging is best-effort). The
GPU-preference read failing → the software `virpipe` default simply is not set,
and the GL stack falls back on its own.

## Performance

Not a measured surface here — the frame budget lives in [[sub-sdl-port]] (the GL
present + LP_NUM_THREADS pool sizing). The launcher's only cost is the 50 ms
reap-poll granularity, which bounds ^C latency, not throughput.

## Prosecution

- **The GPU preference must stay a default, never an override.** An explicit
  `GALLIUM_DRIVER` must win; a change that unconditionally writes `virpipe`
  would silently redirect a user who chose otherwise.
- **The per-line close in `Sys_DebugLog` is not removable.** It is the I-38
  publish point; folding it into a held descriptor for "efficiency" makes the
  log unreadable to a concurrent tail.
- **A new SDL-GL port needs no new capability code.** TyrQuake is the proof that
  the CAP_JIT acquisition belongs in [[sub-sdl-port]], not the app — a port that
  adds its own capability walk has misread the layering.

## Seams

- **The launcher is a transitional shape.** The union bind of `/clade/bin` onto
  `/bin` makes the pool binary resolve by bare name directly, retiring the
  ramfs-face launcher; until that lands everywhere, the launcher is the bridge.

## Caveats

- **The GL binary lives in the clade pool, not ramfs.** A boot that cannot reach
  `/clade/bin/tyr-glquake` gets the launcher's exit-127, not the game — the GL
  build (llvmpipe + LLVM) is too large for the ramfs by design.
- **The shipped pak is shareware.** The build fetches a sha256-pinned
  `quake106.zip` (demo levels only); a full-game run needs the user's own pak.
- **The `execv`-first arm is dormant on modern builds.** Once pouch carries
  `execve` (LINEAGE L-6, patch 0026) the primary `execv` succeeds and the
  `posix_spawn` + reap-poll fallback never runs; the fallback is the pre-L-6
  path, kept because the launcher must work on both.

## Provenance

The G-7 arc (G-7b, the GL Quake gate) and `LLVM-DESIGN.md` §9 step 3. Swept into
the vault by [[chg-2026-09-07-author-sdl-port]] alongside its platform layer
[[sub-sdl-port]].

## Tests

The GL Quake gate builds `tyr-glquake` against the shipped `libSDL2.a` (the link
that would fail if the sdl-port GL path regressed) and runs the demo1 timedemo;
the interactive `ls-gfx-glquake.exp` ([[gate-interactive]]) drives a live
session. The `-nosound` first-map-load path that 0001 guards is exercised by
that gate (the software and GL builds both boot `-nosound`). The `-condebug`
path fix (0002) is witnessed by the log actually being readable from a
concurrent reader after each line.
