# 112 — Kaua, the native console TUI substrate [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-07-kaua-doc-absorb`).
`usr/lib/kaua` — the native (`no_std`+`alloc`, libthyla-rs) immediate-mode text-UI
library: an app redraws a whole cell `Buffer` each frame, the backend diffs it and
emits only the changed cells (the ratatui model over Thylacine's device model). The
"text weave" of the Loom family. Its content lives, code-verified and current, in:

      vault/system/userspace/shell-tui/sub-kaua.md   (audit-bearing: I-27 + the O(1) parser)

Everything the doc carried is there — the pure layers (style/rect/buffer + the diff,
event, the VT/ANSI `input` parser, encode, layout, widget), the fd-1 output
`Terminal` + the fd-0 `PollSource` (split so the Loom seam is real: a future
`LoomSource` swaps the input half via the `EventSource` trait), and the four
load-bearing properties:

- **the O(1) total parser** — `Parser::feed` accepts any byte sequence, holds O(1)
  state and never panics/loops/grows (a CSI param flood latches `csi_overflow` and
  yields no event; UTF-8 bounded at 4), plus the per-round drain cap against an
  unbounded writer;
- **I-27, consumed-not-introduced** — the backend touches **only fd 0 (read) + fd 1
  (write)**, never consctl, never console-attach; raw termios is `ut`'s job before
  the spawn — "a negative property", honest for a trusted or untrusted caller alike;
- **the crash backstop** — `Terminal::Drop` restores on a clean return, but
  `panic = abort` means Drop does not run on a crash, so `ut`'s post-reap restore is
  the authoritative (idempotent) backstop;
- **the #117 CPR size handshake** — the save/park/`ESC[6n` round-trip, F1-bounded by
  a total deadline (a dribbling peer cannot multiply the budget) and F2-lossless
  (stops at `R`, returns launch type-ahead as `pending` for `with_pending` to replay
  through the same parser in wire order; a late CPR is recognized as a resize).

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Nothing load-bearing — a clean zero-fold; the dossier is ahead of the doc's own
  stale Status table.** The doc's header records T-4 (the `ut` dance, `@77386f7`)
  landed and the focused I-27 audit **closed** (Opus-4.8-max, 0 P0 / 0 P1 / 0 P2 / 3
  P3, all closed at #106) — but its Status *table* still reads "T-4 ... not started"
  and "audit ... not started", an internal contradiction. `sub-kaua` reflects the
  landed+closed reality (the dance, the audit, the #106 P3 closures). The width-1
  cells, the query-at-launch-only winsize (no live-resize signal over UART), the
  richer-layout/word-wrap/indexed-color/`LoomSource` items are live v1.x seams the
  dossier holds. Zero code change.
