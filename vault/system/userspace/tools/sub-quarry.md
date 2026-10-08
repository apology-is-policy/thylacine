---
id: sub-quarry
type: sub
title: "quarry — one menu over every renderer the box carries, and a bench that reads the engine's own log"
parent: moc-userspace-tools
code:
  - usr/quarry/src/main.rs
  - usr/quarry/Cargo.toml
  - tools/interactive/quarry.exp
audit: none
guarded-by: []
validated-by: [prose, gate-interactive]
locks: []
hazards: []
abis: []
design: []
created: 2026-10-08
updated: 2026-10-08
---
## Purpose

quarry is the GPU demo-bench launcher: one place to run Quake with each
renderer the machine carries — software tyr-quake, GL tyr-glquake on llvmpipe or
on virgl (the `/srv/warp` hardware seam), and two Vulkan rows that wait for a VK
engine. It has three faces: `quarry`, a kaua menu; `quarry <key> [args...]`, a
launch on the inherited console; and `quarry list` / `quarry bench`, the
agent-facing probe table and timedemo comparison. It is the instrument the GL
performance work reads its numbers from, so most of its care goes into making a
number mean what its label says.

## Contract

**The renderer matrix is data.** `RENDERERS` holds one row per renderer — key,
label, binary, the `GALLIUM_DRIVER` value it pins, and its kind — so a new
renderer is one row.

**A row says only what it has proved.** `probe` marks a row missing when its
binary is absent, and the `hw-gl` row also reads `/srv/warp`'s ctl and needs
`virgl 1`: a 2D device or an absent service downgrades the row rather than let
the GL build fall back to llvmpipe under a hardware label. Each GL row pins its
driver even where the build default would match, because the mesa fork falls
back loudly and a bench row that relied on fallback order would mislabel a
fallen-back run.

**The driver rides `/env`, not the spawn ABI.** quarry writes `GALLIUM_DRIVER`
into its own environment, the child inherits a deep copy at spawn
(`env_clone_into`), and the old value is put back after. No spawn-ABI
environment argument exists (#151); this is the sanctioned inheritance route.

**The console is borrowed, then lent.** quarry owns the screen on fd 1 and reads
keys on fd 0 through kaua ([[sub-kaua]]); it never touches consctl, and `ut`
sets and restores raw mode around it (`is_raw_command`). A played game takes the
console, so the menu leaves the screen for the game's run and redraws on reap.
quarry stops reading at `q` and at Enter (play): kaua reads one byte at a time,
so what was typed behind `q` goes to the shell and what was typed behind Enter
goes to the game (`quarry.exp`).

## Mechanism

**Probing `/srv/warp` is two opens.** Opening `/srv/warp` is a connect, and a
single-shot walk of `/srv/warp/ctl` does not compose through it, so `probe`
opens the service root and then `ctl` relative to it — the shape joey's probe
and `warp_client` use.

**A bench leg reads the engine's console log, never its stdout.** `+timedemo`
does not quit the engine when the demo ends, so a piped stdout would hold the
fps line in the child's stdio buffer until an exit that never comes. The engine
runs with `-condebug`, which writes every console line to
`/quake/id1/qconsole.log` with a bare open/append/write/close, and quarry reads
that file every 500 ms. A leg ends on the fps line, on the engine's own exit, or
at `BENCH_DEADLINE_MS` (600 s, above the slowest honest run); every leg then
kills the engine and reaps it within a bounded wait. The log is deleted before
each leg so a leg reads its own lines.

**A leg witnesses its own conditions.** `+vid_describecurrentmode` puts the mode
the engine actually chose into the same log, and quarry prints it beside the
request, naming a MISMATCH — an unhonoured `-width` and a per-submit-bound
renderer both draw a flat fps curve, so a sweep that recorded only its request
could not tell the measurement from the bug. Each leg also prints whether it
ran paced, and records the engine's `GL_RENDERER` line and its count of
`GL_OUT_OF_MEMORY` and mesa errors.

**Legs are `key[@WxH][:paced]`.** An explicit list selects, orders and sizes the
legs: order because a within-boot drift makes a late leg read low (#168, #232),
size so one boot sweeps a renderer across modes. A bench leg is UNPACED by
default (`SDL_THYLACINE_NOPACE=1`, set around the spawn like the driver),
because a paced present waits on the compositor's frame tick and the number
stops being the renderer's; `:paced` measures the delivered rate. Play keeps
sound on; a bench runs `-nosound` so the audio path cannot move the figure.

**The menu.** A header, one row per renderer with its status or last bench
result, and a footer of keys: Enter play, `d` timedemo of the selection, `b`
bench every ready row, `r` re-probe, `q` or Escape quit.

## Data structures

`Renderer` (the static row) and `Kind` (software, GL, Vulkan). `Status` is
`Ready` or `Missing(reason)`. `Leg` is a row plus an optional resolution and the
paced flag; its label carries both, so a pasted result keeps the variable it
varied. `Bench` keeps the engine's own strings for frames, seconds and fps —
re-formatting a measurement invites drift — with the renderer string, the error
count, the exit status and a note. `App` holds the menu rows as (row index,
status, last bench), the selection and a status line.

## Concurrency

Single-threaded, one child at a time: play waits for the game, and a bench leg
polls the log, then kills and reaps. Keys typed during a bench are not read
until it ends — they wait in the console — and the menu handles them after.

## Invariants enforced

No §28 invariant is on this line: quarry is an unprivileged program with no
capability of its own. It writes its own `/env` (which its children inherit)
and the engine's console log.

## Error paths

A spawn failure reaches the menu's status line or the CLI's output. A leg that
ends without an fps line says why (the engine exited first, or the deadline
killed it) and prints what it read of the log — present or not, its size, its
last line — because an absent log, an empty one and an unparsed one accuse
different things. The kill is bracketed by printed markers, so a wedge in it
leaves its step behind. `quarry bench` exits 1 when any leg lacks an fps figure.

## Performance

quarry's own cost is a 500 ms log poll during a leg; the figures are the
engine's.

## Prosecution

- **Does a row still say only what it proved?** The `hw-gl` probe and the
  explicit driver pin are what keep a llvmpipe fallback from carrying a
  hardware label.
- **Is the environment restored on every path?** The driver and the pace flag
  are put back after the spawn whether it succeeded or not, so quarry's own
  environment ends as it began.
- **Is every leg bounded?** By the deadline and the bounded reap; the one
  unbounded step is the `/proc/<pid>/ctl` kill write, which the markers expose.
- **Does the menu stop reading at Quit and Play?** It returns from the burst on
  `q` and ends it on Enter, so the bytes behind either stay in the kernel.

## Seams

- **The Vulkan rows** probe missing until a VK engine is staged; the ICD
  selector (lavapipe or venus) lands with that arc (Warp-6). The rows exist now
  so the menu and the CLI keys are stable.

## Caveats

**The bench trusts a shared file.** `/quake/id1/qconsole.log` sits in a gamedir
`build.sh` makes world-writable so the session user can write it, and every
quarry reads and deletes the same path: two benches at once, from two sessions,
would read each other's lines, and another user could write an fps line a bench
would report. A benchmark tool with no authority, so a wrong number rather than
a breach, but a number is the whole product (owned as an open bug, 2026-10-08).

**No host tests.** `parse_leg`, `fps_line` and `mode_line` are pure functions
over strings, but the crate is a `no_std` bin with no lib target, so nothing runs
them; the prowl caveat describes the same shape and its one-manifest-line fix.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
