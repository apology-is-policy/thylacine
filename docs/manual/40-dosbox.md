# 40 — DOSBox-X: running DOS (and 3dfx/Glide) games

**Audience:** a technically skilled user who is new to Thylacine. Assumes
Unix/Linux fluency; explains what Thylacine does differently.

DOSBox-X is a ported x86 PC emulator (the "Cryptid" port). It runs real DOS
protected-mode games and applications on Thylacine's ARM64 kernel by
JIT-compiling x86 to AArch64 through Thylacine's `CAP_JIT` capability — including
3dfx **Voodoo/Glide** titles via a software (CPU) rasteriser. This page documents
every setting that matters, where each setting comes from, and its effect.

---

## Overview

- **What it is:** a full DOS/Windows-3.x PC in a window. You mount a directory
  as a DOS drive and run a `.EXE`/`.COM`.
- **When to use it:** to run a DOS-era game or application unchanged.
- **What makes it work on Thylacine:** `core=dynamic_rec` compiles x86 into a
  dual-mapped code buffer acquired through `CAP_JIT` (W^X-safe; see
  `docs/reference/152-dosbox.md`). Without the dynrec, DOS protected-mode games
  are too slow.
- **What Thylacine sets up for you:** a system-wide default config
  (`/lib/dosbox-x/dosbox-x.conf`) turns on mouse capture, the JIT core and a
  fixed CPU speed for every launch, and the two shipped games carry a per-game
  config, so playing one is three commands.

---

## Getting started

The game data lives read-only in the pool (SYSTEM-owned). DOSBox-X opens its
files **read-write**, so first copy the game into your own writable home, then
start DOSBox-X *from inside the game directory*:

```
cp -r /duke3d ~/duke3d
cd ~/duke3d
dosbox-x
```

That is the whole launch. DOSBox-X finds the game's `dosbox-x.conf` in the
current directory; that file mounts the directory as `C:` and runs the game.
The same three commands with `/tombraider` start Tomb Raider.

> **Why the copy?** `/duke3d` is a read-only SYSTEM-owned master in the pool. A
> non-owner cannot open its files read-write even at mode 0666 (the pool
> ownership model — see the Stratum manual). Copying to your home is the same
> "install to a writable directory" step a real DOS game needs.

To run **your own** DOS program, mount its directory and run it:

```
dosbox-x -c "mount c ~/dos/mygame" -c "c:" -c "GAME.EXE"
```

`-c` commands run at startup in order. Anything you do not set comes from the
system defaults described next.

---

## Where the settings come from (the layers)

DOSBox-X reads its settings from up to four places. Later layers override
earlier ones, value by value:

| Layer | File | Who writes it | Effect |
|---|---|---|---|
| 1. System defaults | `/lib/dosbox-x/dosbox-x.conf` | The build (read-only) | The Thylacine baseline for every launch: `autolock=true`, `core=dynamic_rec`, `cycles=fixed <preset>`, `quit warning=false`. |
| 2. Your defaults | `~/.config/dosbox-x/dosbox-x-<version>.conf` | DOSBox-X, on your **first launch** — generated from layer 1 with every key written out | Edit it to change your personal defaults. **Note:** because it lists every key, a later change to the system file does not reach it — delete it to re-inherit. |
| 3. Per-game | `dosbox-x.conf` in the **current directory** | The game master (copied with `cp -r`), or you | Found automatically when you start `dosbox-x` in that directory; its `[autoexec]` section runs the game. Shipped for Duke Nukem 3D and Tomb Raider. |
| 4. Per launch | `-conf <file>` and `-set "SECTION KEY=VALUE"` on the command line | You | `-conf` loads a file (several allowed, later wins); `-set` overrides one value. Passing `-conf` skips the per-game search. |

`-defaultconf` ignores every file (upstream's built-in defaults), including layer 1.

You can see which files a launch used: DOSBox-X prints
`CONFIG: Loaded system config: /lib/dosbox-x/dosbox-x.conf` and
`CONFIG: Loaded config file: <file>` on the console.

---

## Reference — every setting and its effect

Settings are `KEY=VALUE` lines under a `[SECTION]` header in a config file, or
`-set "SECTION KEY=VALUE"` on the command line. The ones that matter on
Thylacine:

### `cpu core` — how x86 is executed

| Value | Effect | Use when |
|---|---|---|
| `dynamic_rec` | JIT-compile x86 → AArch64 via `CAP_JIT`. **Fast.** The Thylacine default. | Always, for games. |
| `normal` | Interpret x86 one instruction at a time. Slow. | Debugging; a game the JIT mis-runs. |
| `auto` | Interpret, switching to the JIT when a program enters protected mode. | Rarely; the default is already the JIT. |

The JIT prints `dosbox-x: CAP_JIT acquired` on the console when its code cache is
minted. If you see `CAP_JIT acquisition failed`, the launching context lacks the
JIT clearance.

### `cpu cycles` — the emulated CPU speed (the "tick rate")

This is the single most important tuning knob. It sets **how many x86
instructions DOSBox executes per emulated millisecond**, i.e. how fast the
virtual PC is.

**Always use `cycles=fixed <N>`. Do NOT use `cycles=auto`.** `cycles=auto` runs a
feedback loop that continuously re-scales the speed; on Thylacine it hunts wildly
(measured swings of 748 ↔ 495725, a ~500 ms speed oscillation you can *see* as
the game periodically speeding up and slowing down). A fixed count pins the speed
and the oscillation disappears. (Root cause + measurement:
`docs/reference/152-dosbox.md`.)

**Speed presets.** The system default is one of these, chosen when the image was
built (`DOSBOX_CPU_PRESET`, see "Building it in" below; the stock image uses
`pentium`). Pick the class that matches a game's target hardware; tune by feel (a
game running too fast wants fewer cycles, too slow wants more):

| Preset | Approx. PC | `cycles=fixed` |
|---|---|---|
| `xt` | ~4.77 MHz 8088 | `500` |
| `286` | ~12 MHz 286 | `3000` |
| `386` | ~25–40 MHz 386 | `12000` |
| `486` | ~66 MHz 486 | `45000` |
| `pentium` | ~100–133 MHz Pentium | `60000` |
| `pentium2` | ~233–450 MHz PII | `200000` |

Duke Nukem 3D and Tomb Raider both want Pentium class; their per-game configs
pin `60000` regardless of the system preset. For one launch at another speed:

```
dosbox-x -set "cpu cycles=fixed 45000" -c "mount c ~/dos/mygame" -c "c:" -c "GAME.EXE"
```

### `sdl autolock` — mouse capture (needed for mouse-look)

| Value | Effect |
|---|---|
| `true` (**Thylacine default**) | Clicking in the window **captures** the mouse — motion then drives the game (mouse-look/turn). Press **Ctrl+F10** to release the mouse back to Thylacine. |
| `false` (upstream's default) | The mouse is never captured; only mouse **buttons** reach the game, not motion. |

Thylacine turns autolock **on** in the system defaults, so mouse-look works out
of the box. To turn it off for one launch: `-set "sdl autolock=false"`; for good:
set it in your `~/.config/dosbox-x/` file.

### `voodoo voodoo_card` — 3dfx / Glide

| Value | Effect |
|---|---|
| `software` | Emulate a 3dfx Voodoo in software (CPU rasteriser). Runs Glide games with **no host GPU** needed. Correct but modest speed. |
| `auto` / `false` | Auto-select / no Voodoo. |

Use `voodoo_card=software` for Glide titles (e.g. the 3dfx Tomb Raider; its
per-game config sets it). The Glide runtime (`GLIDE2X.OVL`) is built in. When
the emulated Voodoo initialises you'll see `VOODOO LFB now at ...` on the
console. (GPU-accelerated Voodoo is a future path.)

### `[autoexec]` — what runs at startup

The `[autoexec]` section of a config file is a list of DOS commands run at
startup — the file form of `-c`. The shipped per-game files use

```
[autoexec]
@echo off
mount c .
c:
DUKE3D.EXE
```

`mount c .` mounts the current directory, which is why the launch is `cd` then
`dosbox-x`. `-c` commands run *after* the file's section; `-noautoexec` skips
the file's section but still runs `-c` commands.

### The command line

- `-c "<DOS command>"` — run a command at startup (repeatable, in order).
- `-set "SECTION KEY=VALUE"` — override one setting for this launch.
- `-conf <file>` — load a config file instead of searching the current directory
  (repeatable; later files win). A sample with a commented `[autoexec]` is at
  `/bin/dosbox-x.conf`.
- `-noautoexec` — do not run the config file's `[autoexec]` section.
- `-defaultconf` — ignore every config file.
- `-version` — print the version and exit.

---

## Mouse

Two independent things, worth stating plainly because they behave differently:

- **Buttons (fire) always work** — a click reaches the game's mouse driver
  (INT 33h) whether or not the mouse is captured.
- **Motion (look/turn) needs capture** — with autolock on (the Thylacine
  default), **click once inside the game** to capture. Now moving the mouse
  turns/looks. **Ctrl+F10** releases the mouse.

Tomb Raider (1996) is keyboard/joystick only and does **not** use the mouse in
gameplay — that is the game, not Thylacine. Duke Nukem 3D uses the mouse to turn
and fire.

---

## Keys DOSBox-X itself reacts to

| Keys | Effect |
|---|---|
| **Ctrl+F10** | Release / capture the mouse. |
| **Ctrl+F9** | Quit DOSBox-X immediately. (Thylacine disables DOSBox-X's "are you sure?" step: it has no dialog here and would ask on the console pane behind the game.) |

---

## The showcase games

Both ship as pool masters with a per-game config; copy to your home and launch
from inside the copy.

**Duke Nukem 3D (shareware)** — keyboard + mouse:

```
cp -r /duke3d ~/duke3d
cd ~/duke3d
dosbox-x
```

**Tomb Raider (1996, 3dfx demo)** — keyboard, software Voodoo/Glide:

```
cp -r /tombraider ~/tombraider
cd ~/tombraider
dosbox-x
```

The explicit form still works and is what the per-game config does for you:

```
dosbox-x -set "cpu core=dynamic_rec" -set "cpu cycles=fixed 60000" \
         -set "voodoo voodoo_card=software" \
         -c "mount c ~/tombraider" -c "c:" -c "TOMB.EXE"
```

---

## Building it in (image builders)

The build configurator (`tools/configure.sh`, `configs/*.config`) exposes:

| Symbol | Default | Effect |
|---|---|---|
| `CHUNK_DOSBOX` | `y` | Bake the emulator (`/bin/dosbox-x`, ~17.6 MB) and the system default config. Off lowers the two games too. |
| `CHUNK_DUKE3D` | `y` | Fetch Apogee's v1.3d shareware at build time (sha256-pinned) and bake `/duke3d`. |
| `CHUNK_TOMBRAIDER` | `y` | Fetch the 1996 3dfx demo at build time (sha256-pinned) and bake `/tombraider`. |
| `DOSBOX_CPU_PRESET` | `pentium` | The system default speed: `xt`, `286`, `386`, `486`, `pentium`, `pentium2`. |

The game fetches are network inputs (`tools/build-manifest.toml`,
`tools/forage.sh`); the emulator source is vendored in the repo. The system
config lives in the pool, so a preset change needs a pool bake, not just a
ramfs rebuild.

---

## Differences from Linux

- **You copy the game to a writable dir first** (pool masters are read-only; see
  Getting started). On Linux you'd usually run in place.
- **autolock is on by default** and **the JIT core + a fixed speed are the
  defaults** — via the system config layer, which upstream DOSBox-X does not
  have (on Linux the first launch writes a per-user file with upstream's
  defaults: autolock off, `core=auto`, `cycles=auto`).
- **The JIT is a capability.** `core=dynamic_rec` needs `CAP_JIT`; it is granted
  to interactive launches. There is no ambient "make memory executable" — code
  emission is gated (W^X holds across the JIT publish).
- **Sound is silent.** The emulated sound cards exist (games detect them) but
  the output is discarded until Thylacine's audio system lands.

---

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| Game speed oscillates (~½ s cycle) | `cycles=auto` (set by you or an old per-user file) | Use `cycles=fixed <N>`; delete `~/.config/dosbox-x/*.conf` to re-inherit the system defaults. |
| Moving the mouse does nothing in-game | Mouse not captured, or autolock turned off | Click in the window; check `autolock=true` (system default) is not overridden in `~/.config/dosbox-x/`. |
| Mouse stuck in the game | Captured | Press **Ctrl+F10** to release, **Ctrl+F9** to quit. |
| `dosbox-x` in the game dir shows a `Z:\>` prompt instead of the game | The per-game `dosbox-x.conf` was not copied (an old copy) or you are not in the game directory | `cd` into the copied directory; re-copy from the master. |
| Game exits to a DOS prompt at start | Missing/`.GRP`/data or wrong dir | Check the `mount` path; ensure the game files copied. |
| `CAP_JIT acquisition failed` | No JIT clearance | Launch from an interactive session (the login shell has it). |
| Runs but very slow | Cycles too low, or `core=normal` | Raise `cycles`; use `core=dynamic_rec`. |
| A setting from `/lib/dosbox-x/dosbox-x.conf` is ignored | Your generated `~/.config/dosbox-x/` file pins the old value | Edit or delete that file. |

---

## See also

- `docs/reference/152-dosbox.md` — the as-built port internals (patch series,
  the CAP_JIT dynrec, the cycles=auto root cause, software Voodoo, the DX-8
  config layers).
- `docs/reference/150-build-config.md` — the build configurator.
- `docs/manual/00-overview.md` — using Thylacine.
- The Stratum manual — pool ownership (why the copy-to-home step).
