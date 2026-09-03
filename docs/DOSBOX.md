# DOSBox-X on Thylacine -- the DOS + Win9x emulation arc (design)

> Status: DESIGN (scoping, 2026-09-03). Arc prefix **DX**. Variant decided:
> **DOSBox-X** (operator, 2026-09-03) for its Win9x-guest + 3dfx-Glide reach.
> Design-first: this scripture lands before code; each sub-chunk implements
> against it, audited where invariant-bearing. Owner: aux track.

## 1. Goal + why it belongs

Port **DOSBox-X** so Thylacine runs the vast library of DOS applications and
games -- AND, in a second act, boots **Windows 9x guests** and runs **3dfx
Voodoo / Glide** titles. This is a flagship of Thylacine's **emulation strength**
-- the same conviction behind VIVARIUM (unmodified Linux binaries), the planned
x86 translation layer, and the planned Wine path. VIVARIUM answers "run Linux
binaries"; DOSBox-X answers "run DOS software" and then "boot Windows 98 + a 3dfx
game" -- a whole era otherwise extinct on modern hardware. The end-state demo --
Thylacine booting Win98 and running a Voodoo title -- is a spectacular showcase of
the CAP_JIT + compositor + input stack end to end.

Positioning: DOSBox-X is a **port** (a GPL app running on Thylacine), not a new
kernel mechanism -- EXCEPT its dynamic recompiler, which becomes a genuine
demonstration of **JIT-as-a-capability (I-42 / CAP_JIT)**: an x86->ARM64 JIT
riding Thylacine's capability-gated, W^X-preserving code emission. With DOSBox-X
that dynarec is not a nicety -- Win9x and Voodoo emulation REQUIRE it for usable
speed -- so DX-4 is load-bearing and moves early (see 6, 8).

## 2. Licensing -- CLEAR

- DOSBox-X (like original DOSBox + Staging) is **GPL-2.0-or-later**.
- Thylacine is **GPL v3**. GPL-2.0-**or-later** is COMPATIBLE with GPLv3 (the
  "or later" clause lets the code be taken under v3). GPL-2.0-**only** would be
  incompatible -- DOSBox-X is not that.
- A standalone app (separate process) = mere aggregation. A Pouch build linking
  Thylacine GPLv3 libs = combined work under GPLv3, fine (musl is MIT; only the
  Thylacine boundary-line patches are GPLv3). SDL2 is zlib.
- Vendor-time check: grep for any VENDORED GPL-2.0-**only** / GPLv3-incompatible
  component. DOSBox-X's feature sprawl pulls more bundled deps than Staging
  (OpenGlide for Glide, PC-98 fonts, etc.) -- audit each bundled dep's license.

## 3. Source target -- DOSBox-X (decided)

- **DOSBox-X** (SDL2 build path; GPLv2-or-later). Chosen over DOSBox Staging for
  its exclusive reach:
  - **Win9x guests** -- Staging deliberately DROPPED Win9x/XP; DOSBox-X supports
    Win 3.x/9x/ME (its `dynamic_x86` core was reworked for NON-recursive page
    faults specifically to run Win9x reliably -- the core we wire to CAP_JIT).
  - **Glide passthrough** -- both do low-level 3dfx Voodoo 1 emulation now;
    DOSBox-X additionally does high-level Glide passthrough (glide2x.dll -> a
    host provider like OpenGlide).
- The tradeoff accepted: DOSBox-X is the bigger, more legacy, "harder to work
  with" tree (Staging IS the modernized fork). That is a one-time DX-1 porting
  cost (see 5, 10), paid for the Win9x + Glide payoff.
- **Build the SDL2 target** (DOSBox-X supports SDL1 + SDL2; Thylacine's backend
  is SDL2). Verify the SDL2 build path early in DX-1.

## 4. Build path -- Pouch (native port), NOT libt-native, NOT viv

- **Pouch (musl + boundary-line) is the route.** Ported foreign POSIX C++ ->
  Pouch is scripture (ARCH 3.5), and the PROVEN path: SDL2 + the C++ runtime
  already exist on Pouch (see 5). DOSBox-X's larger dep surface (more to vendor)
  is the main delta vs a Staging port.
- **NOT native (POSIX->libt):** no native C++ runtime; DOSBox-X is even larger
  C++ than Staging. Wrong fit.
- **NOT viv (prebuilt Linux binary):** the vivarium-graphics arc (W4) is unbuilt
  and its Wayland/AF_UNIX stage is deferred post-v1.0. A stock Linux DOSBox-X
  cannot reach the display under viv today.

## 5. Architecture -- how it maps (the hard parts already exist)

DOSBox-X = **"TyrQuake, upgraded C -> C++, at larger scale."** Every load-bearing
dependency is built and gate-passing in-tree:

- **SDL2 + `SDL_thylacine`** (`third_party/SDL2` 2.32.10; backend at
  `usr/ports/sdl2/thylacine/`): video renders zero-copy to a **Tapestry weave**
  (`thyla_tap.c`, plain C over 9P to `/srv/tapestry`); present is one blocking
  `tpresent` write, tear-free. Input: a pthread parks on the tapestry event fid,
  evdev keycodes -> SDL scancodes, relative + absolute mouse. PROVEN by TyrQuake
  (969 frames to the scanout, CI green). Win9x's 2D desktop (GDI -> emulated SVGA
  framebuffer -> SDL surface -> Tapestry) rides this SAME software path.
- **Pouch C++ runtime**: static libc++/libc++abi/libunwind over musl (Clade CL-2),
  prover-passing. GATE: requires the LLVM fork clang present.
- **The port idiom** (TyrQuake template, `docs/reference/143-tyrquake.md`): vendor
  pruned-pristine + a boundary-line patch series + a curated object-list build in
  `tools/build.sh` + null-sound + stack/heap sizing. DOSBox-X is a BIGGER tree
  than TyrQuake/Staging -> expect a larger patch series + more object-build labor.
- **Placement**: DOSBox-X mints a tapestry surface; Halcyon/tapestryd place it as
  a **pane**. Software renderer -> the proven software weave path. (Glide
  PASSTHROUGH is the one exception -- it needs the GL path; see 6/8/10.)

## 6. The CAP_JIT dynarec (I-42) -- CENTRAL, not optional

DOSBox-X's `dynamic_x86` core translates x86 basic blocks to ARM64 at runtime,
executes them immediately, and re-emits on self-modifying code. **For Win9x and
Voodoo it is mandatory** -- `core=normal` (the interpreter) is fine for a DOS
text program but painfully slow for Win98 or a 3dfx title. On ARM the dynarec
needs write-then-execute, which strict W^X (I-12) forbids -- except through
**CAP_JIT (I-42)**, which is AS-BUILT + proven (CL-7k).

**The mechanism -- dual-mapping, not an RW->RX flip.** A code Burrow
(`BURROW_TYPE_CODE`) maps one set of physical pages at TWO virtual addresses in
one Proc: **RW at `writer_va`, RX at `exec_va`**, each a separate VMA with fixed
prot. No PTE is ever W-and-X, so I-12 holds unchanged. `SYS_JIT_CREATE(len,out)`
installs both aliases -> `{writer_va, exec_va}`. Emit = plain stores through
`writer_va` (NOT a syscall). Publish = `SYS_ICACHE_SYNC(va,len)`. Execute =
branch `exec_va+off`. Un-emitted pages are zero = `UDF #0` (trap, not residue).
Syscalls: `SYS_JIT_CREATE`=101 (CAP_JIT-gated), `SYS_JIT_DESTROY`=102,
`SYS_ICACHE_SYNC`=103; `JIT_REGION_MAX`=64 MiB; wrapper `libthyla_rs::jit`.

**Why DOSBox-X is a CLEAN fit -- no kernel change:**
- One big region (64 MiB >> the code cache), bump-allocate blocks, one
  `SYS_ICACHE_SYNC` per committed block. Emit is free (plain stores).
- Re-publishing IS invalidation (the `jit-prover` re-emit leg proves it) ->
  self-modifying-code + block-linking = write via the writer alias + `publish_range`.
- DOSBox-X emits-then-executes on the SAME thread -> the cross-PE ISB contract
  (F2) is covered by the calling-PE ISB.
- DOSBox-X detects SMC in SOFTWARE (its emulated MMU) -> it does NOT need the
  resumable-host-fault path (the one designed-but-unbuilt JIT caveat). And Win9x
  specifically leans on DOSBox-X's NON-recursive page-fault dynarec core, which
  is a software-side property -- no kernel dependency.

**Integration work (DX-4):** adapt DOSBox-X's ARM64 dynarec backend to emit at
`writer_va+off` and use `exec_va+off` for the block entry + any absolute code
address (block-link targets, jump tables); intra-block PC-relative branches are
alias-agnostic. The ORC `DualMapMemoryMapper` (`usr/ports/llvm/patches/0007-*`)
is the writer->exec split template. Emit `bti c` at indirect-branch block entries;
PAC-aware on hardened silicon. Acquire CAP_JIT at startup via the corvus `jit`
clearance (elevation-only, stripped at fork). Audit-bearing (I-42 + I-12).

**Fallback: `core=normal`** needs none of this and is the DX-2 DOS first-light
target; DX-4 is required before the Win9x/Voodoo acts (DX-6/DX-7).

## 7. Sound -- fully stubbed (v1.0 non-goal)

Audio is a hard v1.0 non-goal (no virtio-sound; `VISION.md`). DOSBox-X is
even more sound-rich than Staging (PC speaker, SB16, AdLib/OPL, GUS, MIDI,
PC-98 sound) -- ALL of it compiles out to a null mixer, as TyrQuake shipped
`-nosound`. Biggest behavioral haircut; precedented + clean. A future audio
server + virtio-sound (post-v1.0) lights it up.

## 8. Arc structure (two acts)

**Act 1 -- DOS (the spine; core=normal then dynarec):**
- **DX-0** -- this scripture. Scripture commit, no code.
- **DX-1** -- vendor DOSBox-X pruned-pristine (`usr/ports/dosbox-x/`) + license
  grep of bundled deps; SDL2 build path; get it to COMPILE + LINK via Pouch
  (libc++ + libSDL2.a + SDL_thylacine), `core=normal`, sound stubbed. The
  heaviest labor chunk (big tree). Exit: a static ET_EXEC that links.
- **DX-2** -- FIRST LIGHT: stage into ramfs, boot in a tile, wire the file-I/O
  boundary-line (mount a host folder as a DOS drive), reach `Z:\>`, run a DOS
  program in a Tapestry pane. Exit: a DOS program runs + an `ls-gfx-dosbox` gate.
  - **DX-2a DONE** (`@c9c4cb40`): the ET_EXEC RUNS (`dosbox-x -version`).
  - **DX-2b DONE** (2026-09-03): the RENDER leg -- `dosbox-x` graphical paints its
    VGA text screen (blue welcome + `Z:\>`) to a Tapestry pane via output=surface
    -> the SDL_thylacine framebuffer path; `ls-gfx-dosbox` render leg gates it.
    Fixes: force SDL dummy audio (0004), a `SetWindowSize` tap-recreate hook, and a
    non-resizable window (0005) to end the compositor resize-war. THYLACINE_BAKE_DOSBOX
    still opt-in (flips default-on at DX-2 close).
  - **DX-2c DONE** (2026-09-03): mount a host dir as a DOS drive + run a real DOS
    program. `dosbox-x -c "mount c /home/michael" -c "c:" -c "DX2C.COM"` mounts
    michael's writable home as C:, runs DX2C.COM (a 49-byte DOS `.COM` emitted by
    `tools/dx2c-dosprog.py`, baked at the devramfs root, reached at `/bin/DX2C.COM`
    post-pivot via joey's /bin bind), which creates `C:\OUT.TXT` = "DX-2C-OK" via
    INT 21h -- read back through the Thylacine shell (the FILE is the only reliable
    signal; DOS output paints the pane, not serial). The gate backgrounds DOSBox-X
    (`&`) so the shell stays free for the readback. `ls-gfx-dosbox` now proves BOTH
    DX-2 halves in one leg (render screendump + OUT.TXT readback). With DX-2c the
    DX-2 exit criterion is met.
  - **DX-2 close DONE** (2026-09-03, operator-directed): `THYLACINE_BAKE_DOSBOX`
    is now DEFAULT-ON (`${THYLACINE_BAKE_DOSBOX:-1}=="1"`, mirroring
    `build_go_goroot`) -- the emulator + DX2C.COM ship in the default image;
    `THYLACINE_BAKE_DOSBOX=0` opts out for a fast iteration loop, and an absent
    LLVM C++ fork skips the emulator gracefully. **NEXT = DX-3** (sound stub/input).
- **DX-3** -- sound fully stubbed/hardened; input polish (keyboard + mouse for
  games); config/autoexec; larger real DOS programs.
  - **DX-3a DONE** (2026-09-03): the INPUT path proven end to end + the DX-2c
    foreground-exit open item RESOLVED. An injected keystroke travels QMP
    `send-key` -> QEMU virtio-keyboard-PCI -> tapestryd (the compositor owns the
    input device) -> the auto-focused DOSBox-X surface -> the SDL_thylacine event
    pump -> DOSBox-X's BIOS keyboard buffer -> INT 21h AH=08h in a guest program.
    The witness: `DX3K.COM` (`tools/dx3-keyprog.py`, a 67-byte .COM baked at the
    devramfs root) prints a prompt, reads ONE key with no echo, and writes
    "KEY=<c>" to `C:\OUT.TXT`, read back through the Thylacine shell. Gate:
    `tools/interactive/ls-gfx-dosbox-input.exp`, driving `tools/qmp-send-key.sh`
    (the "agentic fingers", companion to screendump.sh's "agentic eyes").
    RESOLVED (the DX-2c "wedge"): it was never an SDL-teardown hang -- a
    FOREGROUND dosbox that exits via an autoexec `-c "exit"` returns the shell
    cleanly (SDL_Quit -> the event-pump join -> process exit all complete; the
    first clean exit of an SDL app to the shell on Thylacine). The pitfall was
    typing `exit` on the SERIAL console: DOSBox-X reads its input from the PANE
    (SDL events), not serial, so a serial `exit` never reaches the DOS shell and
    the foreground dosbox runs on. The cure is an autoexec exit or backgrounding;
    the gate's foreground-exit leg is now HARD.
  - **DX-3b DONE** (2026-09-03): file-based config/autoexec. `dosbox-x -conf
    <file>` loads a dosbox-x.conf whose `[autoexec]` section runs at startup --
    the declarative equivalent of `-c` flags (a persistent config vs retyping
    flags). A sample dosbox-x.conf is baked at the devramfs root (build.sh,
    under THYLACINE_BAKE_DOSBOX): it sets `[sdl] output=surface` and an
    `[autoexec]` that mounts C: = the home and runs a program. Gate
    `ls-gfx-dosbox-conf.exp` runs `dosbox-x -conf <file>` with NO `-c` flags and
    verifies the autoexec ran (C:\OUT.TXT appears) -- and the transcript shows
    `CONFIG: Loaded config file`. (Gate-authoring note: `[word]` in a
    double-quoted Tcl/expect message is command substitution -- reword config
    section names in message strings, or the gate crashes when that arm runs.)
  - **Sound**: DONE by design -- the SDL dummy driver (patch 0004) + full sound
    device emulation with discarded output is the compat-correct stub (DOS
    software detects a sound card and runs its audio code silently rather than
    mishandling "no card"); disabling devices would HURT compatibility. Proven
    by every dosbox boot bringing the sound stack up without a crash.
  - **Larger real DOS program**: deferred to DX-5 (the DOS-game milestone),
    where sourcing a recognizable real program is the explicit task -- no
    assembler is vendored for a richer hand-written one, and DX-5 owns the
    fetch. **DX-3 is otherwise complete; NEXT = DX-4 (the CAP_JIT dynarec,
    AUDIT-BEARING, prereq for Act 2).**
- **DX-4** -- the **CAP_JIT dynarec** (I-42; central): wire `dynamic_x86` to emit
  through CAP_JIT (writer/exec split per the ORC template); SMC via re-publish.
  AUDIT-BEARING (own design pass + focused audit). Exit: `core=dynamic` correct +
  measurably faster; I-42/I-12 prosecuted clean. **Prerequisite for Act 2.**
- **DX-5** -- Act-1 close: a recognizable DOS GAME end-to-end; focused audit;
  reference doc + user-manual entry; AUDIT-TRIGGERS row for the DX-4 surface.

**Act 2 -- Win9x + 3dfx (the showcase; needs DX-4):**
- **DX-6** -- **Win9x guest bring-up**: boot Windows 98 in DOSBox-X on Thylacine
  (IDE/disk-image + SVGA + PCI all in DOSBox-X's C++; display via the software
  weave; needs more guest RAM + the dynarec). Its own milestone -- Win9x boot is
  finicky even on Linux. Exit: the Win98 desktop renders in a pane; a Win9x app runs.
- **DX-7** -- **3dfx Voodoo / Glide**: low-level Voodoo 1 emulation first (CPU via
  CAP_JIT, no GL). Glide PASSTHROUGH (OpenGlide -> host GL) is GATED ON the
  GL-accel path (Warp/venus/Mesa/llvmpipe) being lit in the bake -- a cross-arc
  dependency (see 10); sequence low-level first, passthrough when GL lands. Exit:
  a Voodoo title renders.

## 9. Invariant / audit surface

- **I-42 (JIT-as-a-capability)** + **I-12 (W^X)**: DX-4 is the surface (kernel
  unchanged; the audit prosecutes DOSBox-X's correct USE -- writer->exec
  translation, per-block publish, cache lifecycle). Adds an AUDIT-TRIGGERS row.
- DX-1..DX-3, DX-6 are userspace port work (no new invariant); the audit floor is
  the suite + the LS-CI gate + the pouch boundary-line audit discipline.
- DX-7 Glide passthrough, if built, rides the GL-accel path -> its invariant
  surface is that arc's (Warp/I-45 + the JIT for llvmpipe), not new here.

## 10. Risks

- **DOSBox-X codebase size + legacy** (the main port cost): bigger, older-style,
  "harder to work with" than Staging. DX-1 is the heaviest chunk -- more source,
  more POSIX/SDL surface to patch, more bundled deps to vendor + license-check.
- **Glide passthrough depends on the GL-accel arc.** OpenGlide needs host GL; the
  Warp/venus/Mesa/llvmpipe path exists in source but is NOT baked in the current
  tree. So DX-7 passthrough is gated on that arc; low-level Voodoo (CPU/CAP_JIT)
  is the un-gated fallback. Do NOT promise passthrough before GL is lit.
- **Win9x is resource + dynarec heavy**: makes DX-4 a hard prerequisite for DX-6;
  needs more guest RAM (mind Thylacine's guest memory envelope). Win9x boot is
  finicky (a real bring-up milestone, not a flip).
- **CAP_JIT fit -- RESOLVED (clean).** The as-built dual-map surface covers a
  same-thread, software-SMC dynarec entirely; no kernel extension. One
  `SYS_ICACHE_SYNC` per emitted block is the only irreducible cost.
- **C++ build friction**: the LLVM-fork gate; curated object build for a big tree.
- **File-I/O + SDL usage**: heavy path-based I/O (disk images, drives) + DOSBox-X's
  varied SDL usage (8bpp/palette, mode changes) -> new musl + backend patches.

## 11. Naming (thematic -- RATIFIED)

Keep the **DOSBox-X** name (a foreign port keeps its identity). The Thylacine-side
DOS/Win9x-emulation capability / the emulated-machine tile is named **Cryptid**
(operator-ratified 2026-09-03) -- the cryptozoology / Lazarus-species angle:
software long thought dead, sighted alive on Thylacine. Chunk prefix **DX**.

## 12. Exit criteria ("done")

DOSBox-X runs on Thylacine as a Tapestry pane; mounts host folders as DOS drives;
runs DOS programs AND games (text + VGA) with `core=dynamic` via CAP_JIT (and
`core=normal` as the floor); **boots a Windows 9x guest** whose desktop renders in
a pane; runs a **3dfx Voodoo** title (low-level; Glide passthrough when the
GL-accel path is lit); audio cleanly stubbed; DX-4 audited; reference + manual
docs landed. The DOS AND Win9x libraries are open to the user.
