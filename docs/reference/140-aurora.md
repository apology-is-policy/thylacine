# 140 — Aurora: the console renderer (the fbcon) + the `/dev/cons` drain/feed backend

As-built reference for Tapestry G-4: the kernel drain/feed console backend
(`kernel/cons.c` + `kernel/devdev.c` + the `SPAWN_PERM_CONSOLE_RENDERER`
role), the Cornucopia bake (`tools/bake-cornucopia.py` +
`usr/lib/cornucopia`), the renderer (`usr/aurora`), and the evolved
per-boot console gate. Binding design: `docs/AURORA.md` §3/§4 +
`docs/TAPESTRY.md` §18.7 / §18.11-F8 / §18.12-R2-F6; the compositor it
rides is `docs/reference/139-tapestryd.md`.

## Purpose

Aurora-the-renderer is the screen-side of the terminal protocol: it
interprets the EL0 console byte stream into a cell grid and presents it
through tapestryd — the fbcon claim. The kernel's half is the drain/feed
pair: console output MIRRORS into a ring the renderer reads; the
renderer's decoded keyboard input enters the EXISTING LS-8 line
discipline. The shell/login/ut are unchanged and unaware (AURORA §4's
swappable-backend thesis): they write `/dev/cons`, and the same bytes now
paint a monitor.

## The three console roles (I-27)

| Role | Bit / holder | Conveys |
|---|---|---|
| console-ATTACH | `PROC_FLAG_CONSOLE_ATTACHED` (joey pre-relinquish; corvus post-SAK) | the elevation gate (SAK target, hostowner redeem) |
| console-OWNER | `SPAWN_PERM_CONSOLE_OWNER` → `g_console_owner` (the session shell) | receives Ctrl-C `interrupt` |
| console-RENDERER | `SPAWN_PERM_CONSOLE_RENDERER` → `g_console_renderer` (aurora) | may open `/dev/consdrain` + `/dev/consfeed` — nothing else |

The renderer role (G-4, the §18.12 R2-F6 three-role split) confers NO
elevation authority and NO interrupt-target authority. Grant gate:
console-attach-only (the CONSOLE_TRUSTED shape — the pair reads all
console output and injects input, so only the boot trust anchor
designates it) AND single-holder (`spawn_perm_grant_check` refuses while
a live renderer exists; `proc_set_console_renderer`'s NULL-only claim
under `g_proc_table_lock` closes the concurrent-grant race — the loser
child lacks the flag and the open gate refuses it, fail-closed).
`proc_become_zombie_locked` releases the role on every death path.

## The kernel backend (`kernel/cons.c`)

**The drain** is a mirror tap in `cons_emit` — the one chokepoint both
program output (`cons_output_write`) and line-discipline echo already
cross, so the renderer sees exactly the byte stream a terminal displays:

- Bounded 8 KiB drop-OLDEST ring (`CONS_DRAIN_RING_SIZE`): console
  writers NEVER block on the renderer; on overflow the newest output (the
  prompt) survives and `overflow` counts the loss. The tap's disarmed
  fast path is one RELAXED load per byte.
- `cons_drain_open()` arms (single-open; a fresh open discards the prior
  epoch's bytes); `cons_drain_close()` disarms + wakes a parked reader to
  EOF + the registered pollers (process-context only — the handle-close
  paths).
- `cons_drain_read()` mirrors `cons_input_read`: single-reader
  busy-guard, blocking (death-interruptible per #811), 0 = EOF when
  disarmed-and-empty. The reader gets the RW-11 INTERACTIVE promotion
  (structurally only the bound renderer reaches it) so a keystroke's echo
  paints promptly.
- `cons_drain_poll()`: POLLIN iff bytes buffered OR disarmed (EOF is
  readable); read-only leaf, never POLLOUT. The IRQ-context POLLIN edge
  (echo pushes run in IRQ context) rides the LS-8a deferred-wake relay,
  second instance: `console_mgr` drains `poll_wake_pending` under the
  drain's own leaf lock and walks the hook list in process context.

**The feed** (`cons_feed_write`) injects bytes into `cons_rx_input`
exactly as UART RX bytes: ICANON/ECHO/ISIG/ICRNL cooking unchanged and
backend-independent; the echo lands in BOTH the UART and the drain (the
renderer paints its own echo); a graphical Ctrl-C rides ISIG to the
console OWNER via the LS-5 path. **`is_break` is hardwired false** — no
feed byte sequence can synthesize the SAK line condition (I-27: the
serial BREAK stays the one unforgeable trigger; the graphical SAK is the
board-era kernel-scanned trusted-tier combo, MENAGERIE §7).

**The tee, deliberately.** On serial-bearing media the UART path
continues byte-identical — the tooling ABI, the host terminal, and the
serial trusted path all keep working; on a serial-less board the uart
layer is inert and the ring is the only sink. The exclusive switch
(suppressing EL0 serial output, bound from the DTB medium fact per
TRUSTED-PATH §7) is the recorded board-era seam — it will gate
`uart_putc`, composing with the tap. Kernel diagnostics (SYS_PUTS,
extinction, Halls) are uart-direct and NEVER in the drain: the crash
path must not depend on a userspace renderer.

## The devdev leaves (`kernel/devdev.c`)

`/dev/consdrain` (kind 10, RO) + `/dev/consfeed` (kind 11, WO). Gated at
OPEN **and re-gated at every read/write/poll** on
`proc_is_console_renderer` (the cons data-leaf O_PATH discipline — a
walk-open skips `dev->open`, and #81 CWALKONLY plus the re-gate close
the bypass; a non-renderer poll gets POLLNVAL so no hook can learn
console-output timing). The drain open arms the tap and unwinds on a
failed mint; the opened drain Spoor's close — including the #926/#68
close-at-exit of a dead renderer — disarms via the COPEN-checked hook
(exactly one opened drain Spoor exists at a time; walk intermediates and
O_PATH Spoors never carry COPEN).

## The Cornucopia bake (`tools/bake-cornucopia.py` + `usr/lib/cornucopia`)

AURORA §3's two-rasterization design, build-time half: the TTF is
rasterized ONCE (flatten outlines → nonzero-winding scanline fill at 4×
supersample → 8-bit alpha) into a committed atlas
(`usr/lib/cornucopia/src/atlas.bin`, 47 KiB — magic `CATL` v1) compiled
in via the no_std `cornucopia` crate. Geometry from the font's own
metrics (upm 1000, uniform advance 500 — verified monospace): advance
10 px → em 20 → **cell 10×22, baseline 18** → a 128×36 console at
1280×800. 207 glyphs (ASCII + Latin-1 + the ut prompt glyphs U+22A2 /
U+22EE + extras). Box-drawing/blocks (U+2500–259F) are deliberately
absent — the renderer draws them procedurally for pixel-perfect cell
joins. Re-bake only on a font/geometry change (fonttools in a disposable
venv; see the tool header).

## The renderer (`usr/aurora`)

An ordinary tapestryd client (libtapestry — the demo's template):
private session → fullscreen surface → weave map → Loom presents.

- `vt.rs`: UTF-8 assembly; CSI/OSC state machine (CUP/CUU..CUB/CHA/VPA,
  ED/EL, IL/DL/ICH/DCH/ECH, save/restore, DECTCEM, a real alt-screen
  buffer swap for 1049/1047/47); SGR 0/1/4/7/22/24/27, 16-color,
  bright, 38;2 truecolor (libutopia emits it), 38;5 xterm-256. Unknown
  sequences parse-and-drop. DECSTBM accepted-and-ignored (full-screen
  scroll) — the recorded MVP seam. Colors: the Bonfire palette
  (UTOPIA-VISUAL §1) — exact default bg `#0e0c0c` / fg `#e4ddd8` + the
  §1.4 role-derived ANSI-16 map (bright tier = Aurora's documented
  derivation; scripture pins no brights).
- `render.rs`: atlas alpha-blend; procedural box arms (an exact arm mask
  per codepoint; heavy/double render as light at this cell size — joins
  stay exact); block fills + shade blends; notdef hollow box; underline;
  the inverted block cursor.
- `main.rs`: **the loop blocks on `wait_event`** — load-bearing: a
  non-SQPOLL Loom ring's completions are pumped by the thread blocked in
  enter (the Loom-4 CQ-wait drives the elected 9P reader), so a
  never-blocking reap loop starves its own event stream (measured — the
  frame clock went silent under a poll-only first cut). The 60 Hz FRAME
  clock is the heartbeat: each wake feeds KEY runes / CSI arrow
  sequences to the feed (press + autorepeat; the keymap already folded
  shift/ctrl; Enter is CR), services the drain non-blockingly (bounded
  reads per pass; a larger burst rides the kernel ring's drop-oldest —
  skip-ahead scrolling), toggles the 1 Hz cursor blink, renders the
  CONTIGUOUS dirty row span into the CURRENT slot, and presents exactly
  that rect (slots rotate per present — presenting rows a pass did not
  just render would transfer stale slot content).

joey spawns `/bin/aurora` with the perm in the G-3 block (still
console-attached), replacing tapestry-demo as the resident boot
presenter (the demo stays baked for manual runs — first-present-wins
scanout would race two residents). Aurora's `say!` diagnostics go
uart-direct (SYS_PUTS) — never into its own drain, so no feedback loop.

## The gates

- **The per-boot console gate** (`tools/test.sh`, every ci-smp-gate
  boot): `screendump -c` asserts the console signature — the exact
  Bonfire bg DOMINANT (≥40%) + exact default-fg text pixels (≥200; AA
  glyph cores are pure fg) — then a bounded retry-compare proves
  liveness (the cursor blink / prompt arrival must eventually change a
  dump). Content-independent, deterministic, never dropped.
- **`tools/interactive/ls-gfx.exp`** — the fbcon claim end to end:
  serial login + `ls /`; `-c` + differing dumps before/after; then
  `tools/qmp-sendtext.sh` types `whoami` on kbd-pci0 (display-bound to
  gpu0 in run-vm.sh, which is what makes device-targeted
  `input-send-event` legal) and the serial TEE asserts the command's
  OUTPUT — keyboard → tapestryd → aurora → consfeed → line discipline →
  ut, no pixel OCR.

## Known caveats / seams

- **The tee is the QEMU-era posture**: the exclusive Aurora-only switch
  (no EL0 serial) binds at board bring-up from the DTB medium fact.
- **DECSTBM / scroll regions** ignored (full-screen scroll); fine for
  ut/login/nora's full-redraw style.
- **Drain bursts beyond ~16 KiB/frame** drop-oldest (skip-ahead
  scrolling); a scrollback buffer is an Aurora-environment item
  (AURORA §5).
- **One weight baked** (Regular); SGR bold maps to the bright color
  tier, not a bold face.
- **The Aurora ENVIRONMENT half** (session multiplexing, status band)
  is its own post-G-4 arc (TAPESTRY §18.9).
- **Compositor-gone**: aurora exits on the session-dead error (the F4
  contract); no restarter at v1.0 (the shared netd/tapestryd posture) —
  the serial console is unaffected (the tee).
