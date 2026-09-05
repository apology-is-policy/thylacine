# 40 — DOSBox-X: running DOS (and 3dfx/Glide) games

**Audience:** a technically skilled user who is new to Thylacine. Assumes
Unix/Linux fluency; explains what Thylacine does differently.

DOSBox-X is a ported x86 PC emulator (the "Cryptid" port). It runs real DOS
protected-mode games and applications on Thylacine's ARM64 kernel by
JIT-compiling x86 to AArch64 through Thylacine's `CAP_JIT` capability — including
3dfx **Voodoo/Glide** titles via a software (CPU) rasteriser. This page documents
every launch setting and its effect.

---

## Overview

- **What it is:** a full DOS/Windows-3.x PC in a window. You mount a host
  directory as a DOS drive and run a `.EXE`/`.COM`.
- **When to use it:** to run a DOS-era game or application unchanged.
- **What makes it work on Thylacine:** `core=dynamic_rec` compiles x86 into a
  dual-mapped code buffer acquired through `CAP_JIT` (W^X-safe; see
  `docs/reference/152-dosbox.md`). Without the dynrec, DOS protected-mode games
  are too slow.

---

## Getting started

The game data lives read-only in the pool (SYSTEM-owned). DOSBox-X opens its
files **read-write**, so you must first copy the game into your own writable
home, then launch:

```
cp -r /duke3d /home/michael/duke3d
dosbox-x -set "cpu core=dynamic_rec" -set "cpu cycles=fixed 60000" \
         -set "sdl autolock=true" \
         -c "mount c /home/michael/duke3d" -c "c:" -c "DUKE3D.EXE"
```

That single command is the whole recipe: it selects the JIT core, pins the
emulated CPU speed, enables mouse capture, mounts the game as `C:`, and runs it.
Each flag is explained below.

> **Why the copy?** `/duke3d` is a read-only SYSTEM-owned master in the pool. A
> non-owner cannot open its files read-write even at mode 0666 (the pool
> ownership model — see the Stratum manual). Copying to your home is the same
> "install to a writable directory" step a real DOS game needs.

---

## Reference — every launch setting and its effect

DOSBox-X settings are passed as `-set "SECTION KEY=VALUE"`. The ones that matter
on Thylacine:

### `cpu core` — how x86 is executed

| Value | Effect | Use when |
|---|---|---|
| `dynamic_rec` | JIT-compile x86 → AArch64 via `CAP_JIT`. **Fast.** | Always, for games. |
| `normal` | Interpret x86 one instruction at a time. Slow. | Debugging; a game the JIT mis-runs. |

The JIT prints `dosbox-x: CAP_JIT acquired` on the serial when its code cache is
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

**Speed presets** — pick the class that matches the game's target hardware. These
are approximate; tune by feel (a game running too fast wants fewer cycles, too
slow wants more):

| Preset | Approx. PC | `cycles=fixed` |
|---|---|---|
| XT / 8088 | ~4.77 MHz 8088 | `500` |
| AT / 286 | ~12 MHz 286 | `3000` |
| 386 | ~25–40 MHz 386 | `12000` |
| 486 | ~66 MHz 486 | `45000` |
| Pentium | ~100–133 MHz Pentium | `60000` |
| Pentium II | ~233–450 MHz PII | `200000` |

Duke Nukem 3D and Tomb Raider both want roughly Pentium class; `60000` is a good
default and what the showcases use.

> A future release will let you pick a preset by name at build/launch time (part
> of the build configurator + per-game launchers); today you set the cycle count
> directly.

### `sdl autolock` — mouse capture (needed for mouse-look)

| Value | Effect |
|---|---|
| `true` | Clicking in the window **captures** the mouse — motion then drives the game (mouse-look/turn). Press **Ctrl+F10** to release the mouse back to Thylacine. |
| `false` (default) | The mouse is never captured; only mouse **buttons** reach the game, not motion. |

**Set `autolock=true` for any game that uses the mouse to look or turn** (e.g.
Duke Nukem 3D). Without capture, buttons (fire) still work, but moving the mouse
does nothing in-game. See "Mouse" below.

### `voodoo voodoo_card` — 3dfx / Glide

| Value | Effect |
|---|---|
| `software` | Emulate a 3dfx Voodoo in software (CPU rasteriser). Runs Glide games with **no host GPU** needed. Correct but modest speed. |
| `auto` / `false` | No/auto Voodoo. |

Use `voodoo_card=software` for Glide titles (e.g. the 3dfx Tomb Raider). The
Glide runtime (`GLIDE2X.OVL`) is built in. When the emulated Voodoo initialises
you'll see `VOODOO LFB now at ...` on the serial. (GPU-accelerated Voodoo is a
future path.)

### The `-c` commands and `-conf`

- `-c "mount c <dir>"` mounts a host directory as DOS drive `C:`.
- `-c "c:"` switches to it; `-c "PROG.EXE"` runs the program.
- `-conf <file>` loads a `dosbox-x.conf` (sections + an `[autoexec]`) instead of
  inline `-c`. A sample is baked at `/bin/dosbox-x.conf`.

---

## Mouse

Two independent things, worth stating plainly because they behave differently:

- **Buttons (fire) always work** — a click reaches the game's mouse driver
  (INT 33h) whether or not the mouse is captured.
- **Motion (look/turn) needs capture** — set `sdl autolock=true`, then **click
  once inside the game** to capture. Now moving the mouse turns/looks. **Ctrl+F10**
  releases the mouse.

This is standard DOSBox behaviour; the only Thylacine-specific note is that
autolock is **off by default**, so you must enable it (as in the launch command
above) for mouse-look.

Tomb Raider (1996) is keyboard/joystick only and does **not** use the mouse in
gameplay — that is the game, not Thylacine. Duke Nukem 3D uses the mouse to turn
and fire.

---

## The showcase games

Both ship as pool masters; copy to your home and launch.

**Duke Nukem 3D (shareware)** — keyboard + mouse:

```
cp -r /duke3d ~/duke3d
dosbox-x -set "cpu core=dynamic_rec" -set "cpu cycles=fixed 60000" \
         -set "sdl autolock=true" \
         -c "mount c ~/duke3d" -c "c:" -c "DUKE3D.EXE"
```

**Tomb Raider (1996, 3dfx demo)** — keyboard, software Voodoo/Glide:

```
cp -r /tombraider ~/tombraider
dosbox-x -set "cpu core=dynamic_rec" -set "cpu cycles=fixed 60000" \
         -set "voodoo voodoo_card=software" \
         -c "mount c ~/tombraider" -c "c:" -c "TOMB.EXE"
```

---

## Differences from Linux

- **You copy the game to a writable dir first** (pool masters are read-only; see
  Getting started). On Linux you'd usually run in place.
- **autolock is off by default** — enable it for mouse-look (on many Linux setups
  it defaults on).
- **The JIT is a capability.** `core=dynamic_rec` needs `CAP_JIT`; it is granted
  to interactive launches. There is no ambient "make memory executable" — code
  emission is gated (W^X holds across the JIT publish).

---

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| Game speed oscillates (~½ s cycle) | `cycles=auto` | Use `cycles=fixed <N>`. |
| Moving the mouse does nothing in-game | Mouse not captured | `-set "sdl autolock=true"`, then click in the window. |
| Mouse stuck in the game | Captured | Press **Ctrl+F10** to release. |
| Game exits to a DOS prompt at start | Missing/`.GRP`/data or wrong dir | Check the `mount` path; ensure the game files copied. |
| `CAP_JIT acquisition failed` | No JIT clearance | Launch from an interactive session (the login shell has it). |
| Runs but very slow | Cycles too low, or `core=normal` | Raise `cycles`; use `core=dynamic_rec`. |

---

## See also

- `docs/reference/152-dosbox.md` — the as-built port internals (patch series,
  the CAP_JIT dynrec, the cycles=auto root cause, software Voodoo).
- `docs/manual/00-overview.md` — using Thylacine.
- The Stratum manual — pool ownership (why the copy-to-home step).
