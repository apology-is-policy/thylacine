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
- **The blend is lane-safe (#35).** `render.rs::blend`'s packed R|B trick
  originally divided the PACKED word by 255 — integer division does not
  distribute over lanes (65536 ≡ 1 mod 255), so the B output absorbed
  `R_sum*257`'s low byte: glyph INTERIORS (the a=0/255 short-circuits)
  stayed exact — which is precisely why the `-c` gate (near-grey bg/fg
  interiors) never saw it — while every antialiased EDGE pixel got a
  garbage B correlated with R. Thin glyphs (the `⊢` prompt) are nearly
  all edge and read wholesale violet; warm colors fringed violet/gold
  (the user's "something with the oranges/yellows" against the Ghostty
  serial view). Diagnosed by pixel-sampling the user's screenshot + a
  live screendump (framebuffer-side, cocoa exonerated) and closed
  numerically: the buggy form reproduces the exact sampled wild pixels
  ((120,116,6)@a=127, (174,168,239)@a=191). The fix is the standard
  lane-safe form (`na = 256-a`, `>>8`), ideal-tracking within 1; a
  dormant `#[cfg(test)]` regression pins it (the host-harness seam).
  The gate blindness was closed at G-5: `screendump.sh -c` now runs a
  blend-integrity pass over exact-fg-adjacent edge pixels (each channel
  must sit in the `[bg,fg]` envelope ±6; >5% outside fails — junctions
  measure ~2%, the #35 formula ~15%), and
  `tools/test-screendump-edge.sh` keeps it non-vacuous offline (a
  synthesized pre-#35-buggy frame must fail on exactly that arm).
- **A failed present is a DROPPED FRAME, never death (#31).** The
  pre-#31 loop exited on any `present()` error, so one transient
  compositor GPU hiccup (the controlq desync's client-visible face)
  killed the console permanently. Now: the dirty rows + `prev_cursor`
  stay set (the retry MUST re-render — slots rotate per present, so
  re-presenting without re-rendering would ship a stale slot), the
  failure is logged with decay (first 3 + every 64th), and only
  `PRESENT_FAILS_FATAL` (240) CONSECUTIVE failures exit — the
  live-stream-but-presents-never-succeed wedge. Real compositor death
  still exits promptly via the event-stream-EOF path (`wait_event`
  erroring), and the FIRST present stays fatal (startup must prove the
  pipe).

joey spawns `/bin/aurora` with the perm in the G-3 block (still
console-attached), replacing tapestry-demo as the resident boot
presenter (the demo stays baked for manual runs — first-present-wins
scanout would race two residents). Aurora's `say!` diagnostics go
uart-direct (SYS_PUTS) — never into its own drain, so no feedback loop.

**TEV_CONFIGURE (G-6a/b)**: aurora is an accumulator client — the row
renderer paints only dirty rows into the current slot, so every weave
slot is a patchwork and only the compositor-side accumulator (the host
resource in direct mode, the screen buffer in composed mode) holds a
complete frame. So EVERY CONFIGURE marks the whole grid dirty and the
next pass repaints it — a same-size CONFIGURE is the compositor's
explicit full-repaint request (structural repaints blank pane content),
and a size CHANGE also forces a full repaint into the cropped viewport.
Aurora deliberately does NOT ack a size change (`Surface::reweave` — the
G-6b generation fence — exists, but the fbcon's cell grid is bound to
the console history at startup): it keeps its grid and the compositor
crops the top-left (the ignore/crop client posture). A reweaving fbcon
(re-derive rows/cols on resize) is a follow-up. No diagnostic is printed
on a CONFIGURE — aurora shares `/dev/cons` with whatever it renders, so
a chatty line interleaves byte-for-byte with a concurrent writer's
output (`t_putstr` is not cross-Proc atomic; the G-6b battery run
measured exactly that mangling). **TEV_FOCUS (G-6c)** falls to the
default-ignore arm: the fbcon renders no focus state (the compositor's
focus ring/strip highlight is chrome, outside aurora's pane).

## The F10 settings OSD (`usr/aurora/src/osd.rs`)

The built-in system dialog (AURORA-CONFIG.md §3.6, chunk 1 as-built) —
deliberately **Turbo-Vision raw** (EGA gray field, double-line frame, cyan
focus bar, drop shadow) so it reads as "system dialog, not the session",
contrasty against both Bonfire and Kaua's fine style. It is not a program:
it lives inside the renderer, so nothing (a future Halcyon included) can
invoke or spoof it.

- **Trigger**: bare **F10** (evdev 68) from aurora's own event stream — a
  key the tapestryd keymap resolves no rune for and `key_bytes` always
  dropped, so no app ever saw it (interception is regression-free). Press
  (`value == 1`) only: the opening key's autorepeat cannot bounce the
  panel shut. **Modal**: while open, every key routes to the OSD and
  nothing feeds `/dev/consfeed`; serial input is unaffected (the OSD is
  aurora-local). Known cosmetic edge: holding Esc past the close leaks
  its autorepeats to the terminal (a tap does not).
- **Sections**: *Appearance* (live: theme cycler + cursor blink) and
  *Display* (live since cfg-3: the MODE row — Left/Right cycles a
  PENDING preset, **Enter applies** through the gated compositor ctl
  (the monitor-OSD semantic: a whole-display reconfigure never fires on
  mere navigation); Resolution + zoom policy stay info rows. The
  pending choice re-seeds from the APPLIED settings at every open —
  `Osd::open_at`). Keys: Up/Dn select, Left/Right/Enter cycle, Tab
  section, Esc/F10 close.
- **Theme = runtime palette** (`vt.rs::Palette` + `THEMES` + `Vt::set_theme`):
  cells bake resolved colors at write time, so a switch retints existing
  content by **exact old→new color match** across both screens + the live
  SGR state; truecolor passes through untouched by design. Slot aliasing
  (Bonfire `ansi[15] == fg`) resolves fg/bg-first — benign while a theme
  keeps `ansi[15] ≈ fg`. Themes: `bonfire` (scripture), plus the
  **proposed names** `parchment` (light) and `spinifex` (green phosphor —
  the Tasmanian-bushland word; held-proposal per the thematic-naming
  discipline, trivially renameable data).
- **Compositing**: the panel draws OVER the grid after `render_rows`
  (`render.rs::draw_run` — explicit-color cell runs — + `darken_rect`, the
  shadow), through the **full-frame present branch only**: slot rotation
  means a partial rect could transfer stale panel pixels from an older
  slot, so an open OSD routes every damaged pass through fill + all rows +
  panel + `present(None)`, sharing the `full_fill` retry discipline
  (`ui.dirty` stays set on a failed present). The terminal keeps updating
  UNDER the panel (the drain still feeds the Vt). Close sets `full_fill`
  (margins refill — the theme may have changed `pal.bg`). Panel geometry
  derives from the current grid each draw, so a reweave needs no
  notification (sub-floor grids clamp).
- **Persistence (cfg-2a — the system tier)**: `/lib/aurora/config` is the
  DEVICE's memory (AURORA-CONFIG §3.2 "the writer defines the tier":
  aurora is a pre-login SYSTEM process and can never touch a per-user
  encrypted home, so the OSD persists to the tier it owns — the
  monitor-OSD semantic). `usr/aurora/src/config.rs`: `parse`/`render`
  (pure, fail-soft — unknown keys/malformed lines ignored, config can
  never break the fbcon) + `load` (bounded 4-KiB read at startup, applied
  BEFORE the first present → the pre-login screen wears the persisted
  theme) + `save` (write-tmp + fsync + rename + a **STRICT post-rename
  fsync on the SAME OWRITE fd** — the A-1.6 swap with its metadata
  barrier. The barrier shape is load-bearing and was earned the hard way,
  three iterations under the persist E2E's hard kill: `SYS_FSYNC` gates
  on RIGHT_WRITE, so any OREAD-opened fd — a parent dir or a re-opened
  file — fails with -1 before any 9P is sent; the fd you WROTE carries
  the right rights and stays valid across the rename (9P fids follow the
  file), and stratumd's `h_fsync` is a whole-pool `stm_fs_commit`, making
  it a complete barrier. A crash mid-save leaves the old config, never a
  torn one; a failed rename leaks a tmp the next save truncates; a failed
  barrier fails the save LOUDLY — best-effort hid the first attempt. See
  the corvus `persist_keypair_wrap` discipline it now mirrors). The OSD
  writes through on EVERY change (per-keystroke when cycling — immediate
  commit, monitor-style; the MODE row is the exception: apply-on-Enter,
  and it persists ONLY on an accepted ctl write). Baked default:
  `usr/aurora/config.default` → the pool populate (the `/lib/ndb/local`
  pattern, readback-verified). Keys: `theme <name>`, `cursor-blink on|off`,
  `mode auto | <W> <H>` (cfg-3).
- **The compositor tier + push-on-start (cfg-3)**: `Settings.mode`
  (`osd::Mode::{Auto, Fixed}`) is the one value aurora pushes to the
  SHARED compositor — through the GATED global ctl (AURORA-CONFIG.md
  §3.3; aurora holds the console-renderer role the gate admits). At
  startup, `config::load` runs BEFORE the tapestry connect and a
  `Fixed(w,h)` mode pushes on a THROWAWAY conn
  (`tapestry::global_ctl_once`, bounded retry) ahead of
  `Surface::fullscreen()` — so the console surface is BORN at the
  configured geometry (the Boot-scanout `set_mode` arm; no boot-time
  reweave). `Auto` never pushes (it IS the boot default). The OSD's
  Display section is live: the Mode row cycles a PENDING preset
  (`MODE_PRESETS`: auto + six common rasters), Enter applies via
  `surf.global_ctl("mode W H")` on aurora's own conn, and settings +
  config::save commit only when the compositor ACCEPTED the write (a
  refused apply must not seed the startup push). The resulting
  CONFIGURE rides the existing resize arm — grid realloc, present,
  winsize re-report (#55) — so the whole session learns the new
  geometry through `tty:winch`.
- **The per-user push (cfg-2b)**: `$home/lib/aurora` is the SESSION's
  file, pushed in-band over the console wire as
  `OSC 7770;aurora;<key>;<value>` (BEL or ST) — the xterm dynamic-colors
  shape; the drain already carries every console byte, so the channel adds
  zero kernel surface. The VT parser buffers OSC payloads (cap 256, queue
  cap 16, fail-soft on malformed/oversize; titles swallowed as before) and
  lands each as a `key value` line in `Vt.settings_req`; the main loop
  applies via the SAME `config::parse` the file uses — **deliberately
  without `config::save`** (session-scoped by scripture) — and, since
  cfg-3, ONLY through the authority-key ALLOWLIST: `theme` and
  `cursor-blink` pass, everything else (`mode` above all) is refused
  with a logged `aurora: OSC settings key ... refused`. Without the
  allowlist a session-injected `mode` would sit in `settings` and ride
  the NEXT OSD `config::save` into the gated startup push — session
  authority laundered through aurora's renderer role. **cfg-3 F1 (the
  audit's P1): the allowlist reads only the first token, but
  `config::parse` re-splits its VALUE on `.lines()`, so an embedded
  NEWLINE (`theme;spinifex\nmode 640 480`) laundered a second statement
  past the single-token check — so `vt.rs::osc_end` now REJECTS any OSC
  whose key or value carries a control byte (`b < 0x20`), the
  receiving-end twin of `aurora-push`'s own sender-side `b < 0x20`
  filter (the PARSER is the trust boundary for a raw byte channel;
  `aurora-push` itself cannot produce the attack — it splits its file on
  newlines into clean single-line OSCs, which is exactly why a
  documented-tool test missed it; the in-guest witness is the baked
  `/lib/aurora/osc-newline-attack` fixture that `ls-gfx-mode` cats,
  asserting no laundered retint).** The `reset system` arm is exempt by
  construction (it re-reads aurora's OWN system file — values the
  session cannot choose — and re-pushes nothing). The
  `aurora-push` coreutil reads `$HOME/lib/aurora` (from the login-seeded
  `/env`) and ALWAYS emits `reset system` first — the reset re-seeds from
  the system file, so every session start is *system defaults ⊕ user
  overrides* and a stale prior-session push dies at the next login (aurora
  is boot-long; without the reset it would linger). login runs the push at
  session start (post `/env` seed, pre shell, AS THE USER — the 0700 home
  denies SYSTEM per A-2d — best-effort + reaped). Trust posture = xterm
  dynamic colors: any console writer can emit the OSC; it is cosmetic,
  session-scoped, non-persisting, aurora-local ONLY, and must never gain a
  persisting or authority-bearing key.

## The gates

- **The per-boot console gate** (`tools/test.sh`, every ci-smp-gate
  boot): `screendump -c` asserts the console signature — the exact
  Bonfire bg DOMINANT (≥40%) + exact default-fg text pixels (≥200; AA
  glyph cores are pure fg) + the G-5 blend-integrity edge pass (above)
  — then a bounded retry-compare proves liveness (the cursor blink /
  prompt arrival must eventually change a dump). Content-independent,
  deterministic, never dropped.
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

- **Kaua vocabulary complete (#37).** Two VT gaps against kaua's emitted
  set, both user-found driving nora: `?7` DECAWM was ignored (kaua paints
  the bottom-right cell under `?7l`; with wrap still on each status
  repaint line-fed at the last row → a whole-screen scroll leaving stale
  modeline fragments — the artifact cascade) and `[6n` CPR was unanswered
  (kaua's size handshake got no report → nora ran the 80x24 fallback
  inside the 128x36 grid). Fixed: the `wrap` flag + the stick-at-margin
  overprint rule, and the CPR reply pushed through `Vt.reply` into the
  consfeed fd — the terminal answering on the keyboard wire. Dormant
  regressions pin both; the in-guest nora drive (type + arrows) renders
  fullscreen with zero artifacts.
