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
  SETUP-generated game config). The cycles fix is a launch flag, not a shipped
  emulator config (see the cycles section).
- `tools/build.sh::build_dosbox_x` — the curated object-list build (static
  ET_EXEC, links libSDL2.a + libc++.a + libz.a). Staleness-cached against the
  vendored tree + port dir + extractor + thylajit + the linked archives; a newer
  patch auto-triggers a clean rebuild (`rm -rf` the copy + obj, re-extract,
  re-patch). `DBX_FORCE=1` forces it.
- `tools/build.sh::build_duke3d_fixture` — fetches + stages the Duke3D shareware
  (see the showcase).
- `THYLACINE_BAKE_DOSBOX` (default-on) bakes the emulator; `THYLACINE_BAKE_DUKE3D`
  (default-on with the emulator) bakes the game data. Either `=0` opts out for a
  fast iteration loop; an absent LLVM C++ fork skips the emulator gracefully.

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
writable home (`cp -r /tombraider ~/tombraider`) then
`dosbox-x -set "cpu core=dynamic_rec" -set "cpu cycles=fixed 60000" -set "voodoo voodoo_card=software" -c "mount c ~/tombraider" -c "c:" -c "TOMB.EXE"`.

**Owed follow-up:** `build_tombraider_fixture` (a build-time archive.org fetch,
sha256-pinned, mirroring `build_duke3d_fixture`) is not yet written -- the pool bake
is currently conditional on a locally-staged `build/tombraider/stage` (the demo is
game data, never committed -- the quake/Duke3D posture). Pinning the fetch needs a
networked build context.

## The gates (`tools/interactive/`)

| Gate | Proves |
|---|---|
| `ls-gfx-dosbox.exp` | DX-2: render (VGA text → Tapestry pane) + a DOS program (`DX2C.COM` writes `C:\OUT.TXT`) |
| `ls-gfx-dosbox-input.exp` | DX-3a: QMP key → virtio-keyboard → tapestryd → SDL → DOS (`DX3K.COM` reads a key) |
| `ls-gfx-dosbox-conf.exp` | DX-3b: `-conf` loads settings + runs the `[autoexec]` (OUT.TXT with no `-c` flags) |
| `ls-gfx-dosbox-dynarec.exp` | DX-4: `core=dynamic_rec` runs a DOS program correctly (I-42 dual-map) |
| `ls-gfx-dosbox-duke3d.exp` | DX-5a: Duke3D under `core=dynamic_rec` — CAP_JIT acquired on serial + a colour-rich title render (≥30 quantized buckets) + a keystroke advancing the frame |
| `ls-gfx-dosbox-tombraider.exp` | DX-7: Tomb Raider 3dfx under software Voodoo — CAP_JIT + `VOODOO LFB` mapped + colour-rich title render + ENTER changes the frame (in-game 3D verified by hand) |

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
  (`cp -r /duke3d ~/duke3d`) and the launch mounts `~/duke3d` as C:. The
  cycles=fixed fix is a launch flag on that command; a one-command `-conf`
  wrapper (with `mount c .`) is a possible convenience follow-up, not shipped.

## Status

DX-1..DX-4 + DX-5a AS-BUILT (`core=normal` floor, `core=dynamic_rec` via CAP_JIT,
Duke3D showcase). DX-5 (the cycles-auto oscillation fix + telemetry) landed this
session. DX-7 software Voodoo/Glide AS-BUILT (Tomb Raider). DX-6 (Win9x) + DX-7 GL-accelerated Voodoo unbuilt. Design + exit
criteria: `docs/DOSBOX.md`.
