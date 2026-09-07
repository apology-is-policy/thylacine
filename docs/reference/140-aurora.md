# 140 — Aurora: the console renderer (fbcon) + the /dev/cons drain/feed [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-07-aurora-doc-absorb`).
Tapestry G-4 — the screen-side of the terminal protocol: aurora interprets the EL0
console byte stream into a cell grid presented through tapestryd (the fbcon claim),
and the kernel's drain/feed pair mirrors console output into a ring the renderer
reads while its decoded input enters the existing LS-8 line discipline (the
swappable-backend thesis: the shell writes `/dev/cons`, the same bytes now paint a
monitor). Its content lives, code-verified and current — across two audit:hard
dossiers — in:

- **the renderer** (`usr/aurora`) — the VT/ANSI state machine, the atlas
  alpha-blend + procedural box arms, the `wait_event`-blocking loop (a non-SQPOLL
  Loom ring's completions are pumped by the blocked thread), the held-input queue
  (#129) + its bounded-wait pacing (#135, so a tab-hidden aurora's held keystrokes
  don't land in the wrong context), the **lane-safe blend** (#35 — the packed-word
  divide corrupted only antialiased *edge* pixels, which is why the near-grey gate
  never saw it), the dropped-frame-not-death loop (#31), the F10 settings OSD, the
  cfg-2a strict-post-rename-fsync persistence, the cfg-3 compositor-tier mode push,
  and the cfg-2b OSC-7770 per-user channel with its **cfg-3 F1** control-byte reject
  (an embedded newline laundered a second statement past the single-token allowlist
  until `vt.rs::osc_end` refused it — the parser is the trust boundary):

      vault/system/userspace/shell-tui/sub-aurora.md   (audit: hard — cfg-3 F1 attribution sharpened)

- **the kernel backend** (`kernel/cons.c`) — the three console roles (ATTACH /
  OWNER / RENDERER; the renderer role confers no elevation, no interrupt target;
  the single-holder NULL-only claim race close), the drain (the drop-oldest mirror
  tap that never blocks a writer and never consults the role), the feed
  (`is_break` hardwired false — no feed byte can synthesize the SAK, I-27), and the
  tee (serial stays byte-identical):

      vault/system/kernel/console-gfx/sub-kernel-cons.md   (audit: hard, I-27)

- **the devdev leaves** (`/dev/consdrain` + `/dev/consfeed`, gated at open AND
  re-gated per op, the O_PATH/CWALKONLY discipline, POLLNVAL for a non-renderer):

      vault/system/kernel/console-gfx/sub-kernel-devdev.md

- **the shared VT** — the analogous control-byte reject for the shared VT library
  (the C-2d buffer-age union-repaint, for which aurora is the tree's Direct-scanout
  accumulator vehicle, is carried by sub-aurora itself):

      vault/system/userspace/shell-tui/sub-lib-vt.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Clean redirect — aurora is fully owned, with one attribution sharpened.** The
  doc names a "vt.rs" module, but **no `usr/aurora/src/vt.rs` file exists** — aurora
  has exactly four source files (main/render/osd/config), all claimed by sub-aurora,
  and the VT/OSC logic (`osc_end`, the palette) lives in `main.rs` + `render.rs`. So
  there is no ownership gap. sub-aurora had attributed the OSC control-byte reject to
  the shared `sub-lib-vt`; the receiving-end reject is actually aurora's *own*
  `osc_end` (in `main.rs`) — the cfg-3 F1 audit fix, sharpened
  (`chg-2026-09-07-aurora-doc-absorb`), with sub-lib-vt noted as the shared twin.
- **Everything else was covered** — the three console roles, the drain-drops-oldest
  and never-consults-the-role, the `is_break`-false SAK guard, the held-input
  queue + pacing, the #35 lane-safe blend, the cfg-2a persistence discipline. The
  tee-is-the-QEMU-era-posture, DECSTBM-ignored, one-weight, and Aurora-environment
  seams are live as-built seams the dossier holds. Zero code change.
