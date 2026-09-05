# 152 — DOSBox-X port (Cryptid) + the CAP_JIT dynarec + the Duke3D showcase

## What it is

DOSBox-X is a ported DOS/Win9x emulator running as a native Thylacine graphical
application (a Tapestry pane). It is a **Pouch port** (foreign POSIX code cross-
built against musl + the boundary-line patch series), not libthyla-rs-native, and
follows the TyrQuake port idiom at larger scale (`docs/reference/143-tyrquake.md`).
The Thylacine-side DOS/Win9x-emulation capability is named **Cryptid** (operator-
ratified 2026-09-03; the cryptozoology / Lazarus-species angle — software thought
dead, sighted alive); the upstream emulator keeps its **DOSBox-X** name. Chunk
prefix **DX**. Design scripture: `docs/DOSBOX.md`.

The port sits on three already-built layers: SDL2 + the `SDL_thylacine` backend
(zero-copy render to a Tapestry weave, evdev→SDL input over 9P — the same software
path TyrQuake proved), the Pouch C++ runtime (static libc++/libc++abi/libunwind
over musl, Clade CL-2), and — for `core=dynamic_rec` — the **CAP_JIT** JIT-as-a-
capability substrate (I-42), which is what makes a runtime x86→ARM64 recompiler
legal under strict W^X (I-12).

## Layout

- `third_party/dosbox-x/` — the vendored pruned-pristine upstream tree (DOSBox-X
  2026.08.31). Stays byte-pristine; `build_dosbox_x` copies it to
  `build/pouch/dosbox-x-src` and applies the patch series **to the copy**.
- `usr/ports/dosbox-x/patches/*.patch` — the boundary-line series (see below),
  applied in sorted order with `patch -p1 -t` (fail-loud: a non-applying hunk
  aborts the build — DX-4 audit F3).
- `usr/ports/dosbox-x/glue/` — host-API stubs Thylacine lacks (the serial-port
  stub `thylacine-serial-stub.cpp`; opusfile/speexdsp shims for CD audio).
- `usr/ports/dosbox-x/config.h` + `config_package.h` — the hand build config
  (`C_DYNREC`, `C_TARGETCPU=0x07` = ARMv8LE) placed on the `-I` path.
- `usr/ports/dosbox-x/duke3d/` — the Duke3D showcase fixtures: `DUKE3D.CFG` (a
  SETUP-generated game config) + `dosbox-x.conf` (DX-8: the per-game DOSBox-X
  config -- autolock + dynrec + fixed cycles + an `[autoexec]` that mounts `.`
  and runs the game; see DX-8 below).
- `usr/ports/dosbox-x/tombraider/dosbox-x.conf` — the Tomb Raider per-game
  config (the same, plus `voodoo_card=software`).
- `tools/build.sh::build_dosbox_x` — the curated object-list build (static
  ET_EXEC, links libSDL2.a + libc++.a + libz.a). Staleness-cached against the
  vendored tree + port dir + extractor + thylajit + the linked archives; a newer
  patch auto-triggers a clean rebuild (`rm -rf` the copy + obj, re-extract,
  re-patch). `DBX_FORCE=1` forces it.
- `tools/build.sh::build_duke3d_fixture` / `build_tombraider_fixture` — fetch +
  stage the two games (sha256-pinned archive.org fetches; see the showcases).
- `tools/build.sh::stage_dosbox_sysconf` — renders the SYSTEM default config
  (`/lib/dosbox-x/dosbox-x.conf`) from `THYLACINE_DOSBOX_CPU_PRESET` (DX-8);
  `populate_stratum_pool` bakes it under `/lib` and the bake-verify asserts it.
- `THYLACINE_BAKE_DOSBOX` (default-on) bakes the emulator + the system config;
  `THYLACINE_BAKE_DUKE3D` / `THYLACINE_BAKE_TOMBRAIDER` (default-on with the
  emulator) bake the games. Any `=0` opts out for a fast/offline iteration loop
  (populate + bake-verify key on the same flag+stage predicate, so a stale stage
  never bakes past an opt-out); an absent LLVM C++ fork skips the emulator
  gracefully. The build configurator exposes them as `CHUNK_DOSBOX` /
  `CHUNK_DUKE3D` / `CHUNK_TOMBRAIDER` / `DOSBOX_CPU_PRESET`
  (`docs/reference/150-build-config.md`); `tools/build-manifest.toml` pins the
  two game fetches as `[network.duke3d]` / `[network.tombraider]`.

## The patch series

| Patch | Role |
|---|---|
| `0001-thylacine-byteorder` | byte-order shims for the musl/aarch64 target |
| `0002-thylacine-bios-logo-libpng-gate` | gate the BIOS-logo libpng dependency |
| `0003-thylacine-whereami-platform` | platform-detection (`whereami`) for Thylacine |
| `0004-thylacine-force-dummy-audio` | force the SDL dummy audio driver — sound is a v1.0 non-goal, but the emulated sound devices still enumerate so DOS software detects a card and plays silently rather than hanging on init |
| `0005-thylacine-non-resizable-window` | pin the window non-resizable — the Tapestry compositor is authoritative on size and letterboxes; a resizable window starts a CONFIGURE resize-war |
| `0006-thylacine-dynrec-capjit` | **DX-4**: the CAP_JIT dynarec alloc arm — `__thylacine__` `DYNCOREALLOC_THYLACINE_JIT`; emit at `writer_va`, execute at `exec_va`, publish via `SYS_ICACHE_SYNC` (see below) |
| `0007-thylacine-cycle-telemetry` | uncomment DOSBox-X's built-in per-adjust `cyclelog` (silent unless `cycles=auto/max`; a diagnostic — see the cycles fix) |
| `0008-thylacine-system-config` | **DX-8**: a system-wide BASE-LAYER config — `/lib/dosbox-x/dosbox-x.conf` is parsed first (values only; the loaded-file list is cleared so upstream's search runs unchanged), logged as `CONFIG: Loaded system config: …`; `-defaultconf` skips it (see DX-8) |

## The CAP_JIT dynarec (DX-4 — I-42 / I-12)

DOSBox-X's `dynamic_rec` core translates x86 basic blocks to ARM64 at runtime.
On ARM that needs write-then-execute, which strict W^X (I-12) forbids — except
through **CAP_JIT (I-42)**, AS-BUILT + proven at CL-7k (`docs/reference/145-jit.md`,
`docs/LLVM-DESIGN.md` §8). The mechanism is **dual-mapping, not an RW→RX flip**: a
`BURROW_TYPE_CODE` Burrow maps one set of physical pages at two virtual addresses
in one Proc — **RW at `writer_va`, RX at `exec_va`**, each a separate fixed-prot
VMA. No PTE is ever W-and-X, so I-12 holds unchanged.

- `SYS_JIT_CREATE(len,out)` = 101 (CAP_JIT-gated) installs both aliases →
  `{writer_va, exec_va}`; `SYS_JIT_DESTROY` = 102; `SYS_ICACHE_SYNC(va,len)` = 103.
  `JIT_REGION_MAX` = 64 MiB. Wrapper: `libthyla_rs::jit` (`usr/lib/thylajit`).
- **Emit** = plain stores through `writer_va` (no syscall). **Publish** =
  `SYS_ICACHE_SYNC`. **Execute** = branch `exec_va+off`. Un-emitted pages are
  zero = `UDF #0` (trap, not residue).
- DOSBox-X is a clean fit needing **no kernel change**: one 64 MiB region (>> the
  code cache), bump-allocate blocks, one `SYS_ICACHE_SYNC` per committed block;
  re-publishing IS invalidation (self-modifying code + block-linking write via the
  writer alias then publish); emit-then-execute on the SAME thread, so the cross-PE
  ISB contract is covered by the calling-PE ISB; SMC is detected in DOSBox-X's
  software (its emulated MMU), so it does not need the resumable-host-fault path.
- CAP_JIT is acquired at startup via the corvus `jit` clearance (elevation-only,
  stripped at fork). Audit-bearing (I-42 + I-12 — the `AUDIT-TRIGGERS.md`
  DOSBox-X dynrec row).

`core=normal` (the interpreter) needs none of this and is the floor; `core=dynamic_rec`
is required before the Win9x/Voodoo acts and is what the Duke3D showcase runs.

## The Duke3D shareware showcase (DX-5a)

`build_duke3d_fixture` fetches `3dduke13.zip` (Apogee official shareware,
sha256-pinned) at build time (NEVER committed — the quake idiom), extracts the
complete released file set from its PKLITE/ZIP `.SHR` payload via host `bsdtar`
(the quake DeIce trick — no new dependency), and verifies `DUKE3D.GRP`
(sha256-pinned, 11,035,779 B = the v1.3d shareware GRP). It stages to the pool at
`/duke3d`, ships a SETUP-generated `DUKE3D.CFG` (sound device None so the game does
not hang on audio init; keyboard+mouse controller) so the game boots straight to
its title, and the pool bake-verify asserts `DUKE3D.GRP` present.

**Run model.** `/duke3d` is a read-only SYSTEM-owned master; DOSBox-X opens the
group file READ-WRITE, which a non-owner cannot do on a SYSTEM-owned pool file
(the pool ownership "walls"), so the game is copied into the user's writable home
first (`cp -r /duke3d ~/duke3d`) — the normal DOS "install to a writable dir"
step, the quake per-user-copy shape.

**License.** 1996 3D Realms shareware — free individual sharing; condition [C]
requires all released files unmodified (the complete `.SHR` set is staged). A
shipped-v1.0-image (product) distribution may need 3D Realms' written permission —
a v1.0-packaging decision for the operator, orthogonal to this dev/gate fixture
(same posture as the already-shipped `/quake` shareware).

## The cycles=auto oscillation + the fixed-cycles fix (DX-5)

**Symptom.** Holding a turn key in Duke3D produced a ~500 ms speed oscillation
("spins a bit slower, then faster") -- windowed and fullscreen. It is NOT the
compositor idle-throttle: the frame-intent fix (`docs/reference/139-tapestryd.md`)
pins the compositor clock to 60 Hz for a visible DYNAMIC surface, and the clock
stayed pinned while the oscillation persisted. The cause is downstream -- the
*emulated CPU speed* hunting.

**Mechanism.** With no `cycles=` setting, DOSBox-X defaults to `cycles=auto`; for a
protected-mode game that enables `CPU_CycleAutoAdjust`, a feedback control loop in
`increaseticks()` (`src/dosbox.cpp`) that re-measures real-vs-emulated time and
re-scales `CPU_CycleMax`. Its evaluation window is **250 ms**
(`if (ticksScheduled >= 250 || ticksDone >= 250 ...)`), so an overshoot-then-
correct cycle spans ~500 ms -- the observed period. It is independent of the
compositor (which is why pinning the clock did nothing).

**Measured (this session, HVF, Duke3D attract demo).** Under `cycles=auto` the loop
is wildly unstable: over 45 s / 531 adjustments the applied `CPU_CycleMax` swung
**748 <-> 495,725** (peak-to-peak 506% of the mean; 21% of steps reverse
direction), with the raw `ticksDone`/`ticksScheduled` timing inputs themselves
swinging 500-1200% -- the loop is fed noisy measurements and hunts hard. The
median settled value was 62,851 (Pentium-100 class -- period-appropriate for
Duke3D). This is far worse than typical DOSBox `cycles=auto` hunting on a native
host; whether Thylacine's host-timer resolution/jitter is the amplifier is an
OPEN question (needs a native-Linux baseline of the same telemetry -- do NOT
assume without measuring).

**Fix (verified).** `cycles=fixed 60000` (Pentium-100 class) -> `CPU_CycleAutoAdjust`
stays off -> constant emulated speed -> oscillation eliminated by construction.
Verified by re-running the showcase gate with the fix: **0** cyclelog lines (the
auto-adjust never fires) and the game renders + takes input. Delivered as the
launch flag on the gate + the documented command (`cp -r /duke3d ~/duke3d` then
`dosbox-x -set "cpu core=dynamic_rec" -set "cpu cycles=fixed 60000" ...`); no pool
rebake -- the fix is a launch-time setting, not a binary/data change. `N` is a
single-number tweak if the emulated speed feels off.

**Telemetry (`0007` patch).** DOSBox-X's per-adjust `cyclelog` (`dosbox.cpp:679`)
was uncommented. It prints `current CPU_CycleMax`, the newly-computed target, the
control ratio, and the raw `ticksDone`/`ticksScheduled` timing every ~250 ms. It
sits after `if (!CPU_CycleAutoAdjust) return`, so it is **silent under a fixed
`cycles=` setting** (the shipped config) -- a zero-overhead opt-in instrument: run
with `cycles=auto` to observe the hunt, ship with `cycles=fixed` to silence it.
Analyzer: `scratchpad/dx5-cyclelog-analyze.py` (not committed).

## DX-7: software Voodoo/Glide -- the Tomb Raider showcase

The 3dfx Voodoo + Glide stack is **already compiled into our DOSBox-X** (the
source-list generator walks `hardware/` + `builtin/`, and Voodoo is not gated by a
compile define -- it is runtime-selected by `voodoo_card=`). Because `C_OPENGL` is
**off** in `config.h`, `voodoo.cpp` maps `voodoo_card=auto` (the default) ->
`emulation_type=1` = the **CPU software rasteriser** (`voodoo_emu.cpp`) -- **no host
GL**. A DOS Glide game loads `GLIDE2X.OVL`, which DOSBox-X supplies from its
built-in blob (`builtin/glide2x.cpp`) and routes to the emulated card. So software
Voodoo is a **launch-config path on the existing, already-audited CAP_JIT dynrec
(I-42)** -- no kernel change, no new build.

**Verified with Tomb Raider (1996), the 3dfx DOS demo** (`TOMB.EXE` = DOS4GW, the
3dfx build; `/tombraider` = the "City of Vilcabamba" demo, archive.org `tomb3dem`),
launched with `-set "voodoo voodoo_card=software"`:
- the emulated Voodoo LFB maps (`VOODOO LFB now at ...` on serial);
- the 3dfx title renders on the scanout;
- **the in-game 3D renders** -- Lara in the Vilcabamba cave/cavern, textured Mayan
  architecture, correct perspective;
- **keyboard input drives Lara through the 3D world** (walk/turn -> the view
  updates). The first 3D-accelerated gameplay on Thylacine.

**Run model** (as Duke3D): `/tombraider` is a read-only SYSTEM master; copy to a
writable home then launch from it: `cp -r /tombraider ~/tombraider; cd ~/tombraider;
dosbox-x` (DX-8: the per-game `dosbox-x.conf` carries the dynrec, the fixed cycles,
`voodoo_card=software` and the `[autoexec]`). The explicit long form still works:
`dosbox-x -set "cpu core=dynamic_rec" -set "cpu cycles=fixed 60000" -set "voodoo voodoo_card=software" -c "mount c ~/tombraider" -c "c:" -c "TOMB.EXE"`.

**The fetch (DX-8, closes the DX-7 owed follow-up):** `build_tombraider_fixture`
fetches `tomb3dem.zip` from the archive.org item `tomb3dem` (2,310,970 B,
sha256 `4ffd686c…`; the item's published md5 `d0150cc4…` matched) at build time,
NEVER committed -- the quake/Duke3D posture. The zip is a single `tomb3dem/`
directory holding exactly the ten released files; `TOMB.EXE` (788,171 B, sha256
`6a333d2d…`) is the fixture's identity and the cache guard. Every staged file was
byte-compared (md5) against the operator's hand-staged demo before the pin was
written. The function was exercised end-to-end on thyla-pi (the networked host):
cold fetch 19 s, warm cache hit, a corrupted `TOMB.EXE` re-stages from the cached
zip, and a wrong zip fails loud on the sha. `THYLACINE_BAKE_TOMBRAIDER=0` opts out.

## DX-8: defaults, presets, per-game configs + the build inputs

**The problem.** Upstream DOSBox-X has no system-wide config location. A plain
launch searches the cwd, the executable's directory (empty on Thylacine: `whereami`
finds no `/proc/self/exe`), then the per-user XDG directory, loads the FIRST hit
exclusively -- and on a first launch GENERATES `~/.config/dosbox-x/dosbox-x-<ver>.conf`
from the built-in defaults with every key written explicitly. So a build-time
default had nowhere to live: whatever the image wanted, the user's generated file
pinned the built-in defaults (`autolock=false`, `core=auto`, `cycles=auto`) over it
forever after. Measured on the gate logs: every plain launch logged
`CONFIG: Loaded config file: /home/michael/.config/dosbox-x/dosbox-x-2026.08.31.conf`.

**The mechanism -- three layers, values-last-wins:**

1. **System base layer** `/lib/dosbox-x/dosbox-x.conf` (patch `0008`): parsed FIRST
   in `sdlmain.cpp`'s config resolution, then `configfiles.clear()` so the upstream
   flow runs byte-for-byte as before. Consequences: a first launch's generated
   per-user file INHERITS the system values (PrintConfig writes the current values);
   the per-user file, a cwd/exe-dir `dosbox-x.conf`, `-conf` files and `-set` all
   still override it; `-defaultconf` ignores it like every other file. It logs
   `CONFIG: Loaded system config: /lib/dosbox-x/dosbox-x.conf` (the gate witness).
   Rendered by `stage_dosbox_sysconf` from the build config's `DOSBOX_CPU_PRESET`
   (xt=500 / 286=3000 / 386=12000 / 486=45000 / pentium=60000 / pentium2=200000
   cycles per emulated ms; default pentium) -- four values: `autolock=true`,
   `core=dynamic_rec`, `cycles=fixed <N>`, and `quit warning=false` (Ctrl+F9 quits
   at once, upstream-DOSBox style: DOSBox-X's confirmation has no dialog on this
   port -- it asks y/n on the CONSOLE, behind a captured game window, and a
   backgrounded launch loops the prompt onto the console forever; the first
   version of the Duke3D gate's quit leg found that out by stacking two DOSBox
   windows). Baked SYSTEM-owned + readback-verified
   under `/lib` (the Plan 9 system-data idiom joey already uses for `/lib/ndb`,
   `/lib/aurora`, `/lib/halcyon`); the bake-verify lists it as `DBXCONF`.
2. **Per-user** `~/.config/dosbox-x/dosbox-x-<ver>.conf` -- upstream's own file,
   generated on first launch from the system values; the user edits it to override.
   The trap it inherits from upstream: it pins EVERY key, so a later build's changed
   preset does not propagate to an existing user file (delete it to re-inherit).
3. **Per-game** `dosbox-x.conf` in the game directory (shipped in both masters,
   copied with `cp -r`): autolock + dynrec + `cycles=fixed 60000` (both games are
   Pentium-class) + Tomb Raider's `voodoo_card=software` + an `[autoexec]` that does
   `mount c .` / `c:` / `<GAME>.EXE`. DOSBox-X's cwd search finds it, so
   `cd ~/duke3d; dosbox-x` is the whole launch -- the GOG/portable-DOS-game idiom,
   with no shell script and no `-set`. Self-contained on purpose: the copied
   directory carries what the game needs even if the system layer changes.

**Why a port patch and not a workaround.** The exe-dir slot would have shadowed the
user's own config (upstream's search is exclusive-first-hit); seeding the user's
file at login would need a skeleton mechanism Thylacine does not have; the base
layer is a 15-line hunk that leaves every upstream semantic intact and gives the
Unix layering (system defaults < user < per-game < per-launch).

**Mouse-look by default** is what the operator asked for and what the Duke3D gate
now proves end to end (WITNESS 4): the second launch warps into E1L1
(`-noautoexec` drops only the file's `[autoexec]`; `-c` still runs;
`DUKE3D.EXE /v1 /l1 /s2`), a tablet click captures (autolock from the FILES), and a
relative sweep yaws the view. The witness is `tools/interactive/gfx_shift.py`: the
per-column luma profile of the viewport band moves WHOLESALE under a yaw, while
animation or the shot's muzzle flash changes brightness locally without moving it.
The band lives INSIDE the surface's extent measured from the frame (per-row luma
variance across the tile, inset past its frame lines; 10..80% of that, below
DOSBox-X's menu bar and above the game's HUD) -- a fixed display-relative band went
blind on the gate's second run: the surface is top-aligned in its tile, so display
rows 30..65% straddled the HUD and the uniform dark-grey tile ground, both identical
between frames, and a 212 px keyboard yaw read as 0; `--extent` also hands the gate
the surface centre to click. Calibrated: click-only and no-input -> 0 px; a
keyboard turn (six arrow taps) 212-224 px; each 20x500 sweep +-296..320 px (320 is
the search window's cap), sign following the direction. The passing run measured
control 0 / keyboard 224 / right sweep 320 / left sweep -320. The gate runs the no-input CONTROL first
(two frames, ~0 shift required, else "cannot discriminate" rather than a pass) and
then requires |shift| >= 100 px for the sweep and the opposite sign for the sweep
back. A keyboard turn (the right arrow) sits between them as the POSITIVE control:
it proves the warp reached the level (a title/menu/loading screen is colour-rich
too) and that the witness can see a yaw in this layout, so a mouse-arm failure means
the mouse. A frame hash cannot carry this witness -- a live 3D frame differs on its
own (measured: the click-only pair already hashed differently). The quit between the
two launches is FENCED behind an echo marker: the first version matched a `retire
surface` line already in the buffer (DOSBox-X re-creates its window at start-up),
passed its "quit" step, and stacked two windows.

**The build inputs.** `tools/build-manifest.toml` gained `[network.duke3d]` +
`[network.tombraider]` (auto-at-build; url + sha256 + the GRP/EXE identity pins),
`forage.sh` the `duke3d` / `tombraider` targets (and accepts any literal
`class.name` section), and `test-forage.sh` a pin-drift control (A9): every hash and
url under `network.*` must appear verbatim in `build.sh` -- two copies of one truth,
and a bump in one place fails the test. The configurator gained `CHUNK_DOSBOX` /
`CHUNK_DUKE3D` / `CHUNK_TOMBRAIDER` (all default y) + `DOSBOX_CPU_PRESET` (choice),
and a second constraint kind: `CHUNK_DOSBOX=n` LOWERS the two games (raising the
emulator back would silently undo an explicit 17.6 MB opt-out). The emulator itself
is vendored in-repo and is deliberately NOT a manifest input.

## The gates (`tools/interactive/`)

| Gate | Proves |
|---|---|
| `ls-gfx-dosbox.exp` | DX-2: render (VGA text → Tapestry pane) + a DOS program (`DX2C.COM` writes `C:\OUT.TXT`); DX-8: `/lib/dosbox-x/dosbox-x.conf` baked with the preset + the base layer loads on a launch with no `-conf` |
| `ls-gfx-dosbox-input.exp` | DX-3a: QMP key → virtio-keyboard → tapestryd → SDL → DOS (`DX3K.COM` reads a key) |
| `ls-gfx-dosbox-conf.exp` | DX-3b: `-conf` loads settings + runs the `[autoexec]` (OUT.TXT with no `-c` flags) |
| `ls-gfx-dosbox-dynarec.exp` | DX-4: `core=dynamic_rec` runs a DOS program correctly (I-42 dual-map) |
| `ls-gfx-dosbox-duke3d.exp` | DX-5a + DX-8: Duke3D launched as `cd ~/duke3d; dosbox-x` (both config layers witnessed) under `core=dynamic_rec` — CAP_JIT acquired + a colour-rich title render (≥30 buckets; also proves the per-game `[autoexec]` ran) + ENTER advancing the frame; then Ctrl+F9 (surface retired) + a warp launch into E1L1 + the **mouse-look witness** (no-input control ~0 shift; a sweep yaws ≥100 px; the reverse sweep flips the sign) |
| `ls-gfx-dosbox-tombraider.exp` | DX-7 + DX-8: Tomb Raider launched as `cd ~/tombraider; dosbox-x` (both config layers witnessed; `voodoo_card=software` from the per-game file) — CAP_JIT + `VOODOO LFB` mapped + colour-rich title render + ENTER changes the frame (in-game 3D verified by hand); SKIPs (77) when not baked |

## Performance characteristics

Two dynrec-vs-interpreter attempts, both instructive:

- **Boot-to-title is the WRONG metric here.** The boot-to-title timer reported
  byte-identical `title_reached_ms=9738` for BOTH cores -- because a colour-bucket
  threshold fires on DOSBox-X's SDL-rendered *startup* screen (core-independent),
  not Duke3D's emulated title. The exact match is what caught it.

- **Achievable emulated-cycle rate (`cycles=auto`, attract demo, M2/HVF).** The
  auto loop drives each core toward ~90% realtime, so the achieved `CPU_CycleMax`
  reflects how many emulated x86 cycles each core sustains
  (mean / median / stable-plateau / peak):
  - `core=dynamic_rec`: 97,839 / 62,851 / ~187,000 / 495,725
  - `core=normal`:      49,204 / 44,985 / ~150,000 / 158,088

  The dynrec's advantage is ~1.4x (median) to ~3x (peak); the spread reflects the
  demo alternating CPU-bound and render-bound phases -- the Build engine's software
  rasteriser + the SDL blit are host-side, not emulated-CPU-bound -- and the
  `cycles=auto` loop is noisy on both cores. Both comfortably exceed Duke3D's
  ~60,000-cycle (Pentium-100) target on this host, so at the shipped
  `cycles=fixed 60000` the two deliver the same Duke3D experience; the dynrec's
  decisive advantage is HEADROOM for the CPU-heavy future workloads (Win9x/Voodoo,
  DX-6/7) and efficiency on slower silicon.

- **A rigorous single-number speedup needs a dedicated CPU-bound benchmark** (a
  compute-loop DOS program timed under each core at a fixed high cycle cap) --
  deferred; the attract demo is too render-mixed to isolate the recompiler.

## Known caveats / seams

- **Sound fully stubbed** (v1.0 non-goal): all DOSBox-X audio (PC speaker, SB16,
  AdLib/OPL, GUS, MIDI) compiles to a null mixer via the forced dummy driver
  (`0004`). A future audio server + virtio-sound (post-v1.0) lights it up.
- **Software Voodoo/Glide is AS-BUILT** (DX-7, verified with Tomb Raider — see
  above): CPU-rasterised, no host GL, on the CAP_JIT dynrec. It is CPU-heavy
  (fine for a 1996 game on M2, not blazing). **GL-accelerated Voodoo** (fast,
  high-res) is the unbuilt DX-7-proper path — it needs the host GL-accel arc
  (Warp/venus/Mesa/llvmpipe).
- **Win9x (DX-6) unbuilt** — resource + dynarec heavy; DX-4 is its prerequisite.
- **The run model copies the game to the user's home** — `/duke3d` is read-only
  SYSTEM-owned; DOSBox-X needs a writable drive, so the game is copied first
  (`cp -r /duke3d ~/duke3d`). Since DX-8 the copied directory carries its own
  `dosbox-x.conf` (`mount c .` + the game), so the launch is `cd ~/duke3d;
  dosbox-x`; the explicit `-set`/`-c` long form still works.
- **The generated per-user config pins every key** (upstream behaviour): the
  first launch writes `~/.config/dosbox-x/dosbox-x-<ver>.conf` from the system
  values, so a LATER change of the build's `DOSBOX_CPU_PRESET` does not reach an
  existing user file -- delete it to re-inherit, or set the value there.
- **`/lib/dosbox-x/dosbox-x.conf` is pool content**: a ramfs-only rebuild
  (`build.sh ramfs`, or `THYLACINE_MKFS_PRESERVE=1`) does not re-render it into
  the pool; a preset change needs a pool bake (the general PRESERVE trap).

## Status

DX-1..DX-4 + DX-5a AS-BUILT (`core=normal` floor, `core=dynamic_rec` via CAP_JIT,
Duke3D showcase). DX-5 (the cycles-auto oscillation fix + telemetry) landed.
DX-7 software Voodoo/Glide AS-BUILT (Tomb Raider). DX-8 AS-BUILT (2026-09-05, @1c054bdf): the
system base-layer config (patch 0008) + `DOSBOX_CPU_PRESET` + the per-game configs
(mouse-look on by default, gate-proven) + `build_tombraider_fixture` + the
manifest/forage/configurator registration. DX-6 (Win9x) + DX-7 GL-accelerated
Voodoo unbuilt. Design + exit criteria: `docs/DOSBOX.md`.
