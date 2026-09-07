# 150 — halcyond: the Halcyon environment client [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-07-halcyond-doc-absorb`).
`usr/halcyond` — the Halcyon environment client (H-2, the CPU floor): the
transcript renderer that ingests untrusted OSC-1936 Beacon frames and paints them
through the compositor, and — in its `--session` variant — the per-user session
compositor itself (tiles, chrome, menus, the declared display handoff). Its
content lives, code-verified and current, in one audit:hard dossier:

    vault/system/userspace/shell-tui/sub-halcyond.md   (audit: hard)

The dossier carries, as-built, every one of this doc's load-bearing invariants
(the audit anchors), verified present:

- **The streaming property** — feeding a byte stream in any chunking yields the
  identical transcript structure; `safe_cut`'s holdback protects the escape OPEN
  at buffer end (the last-ESC heuristic was a real bug: an OSC's ST terminator is
  itself a later ESC, so cutting there strands the opener), pinned by the
  byte-by-byte fingerprint regression.
- **The robustness contract** — never panic, never buffer unboundedly (FRAME_MAX
  + 16 flushes an over-long partial), every malformed reference skips fail-safe.
- **Span hygiene** — Beacon em/obj/hdr spans die at block boundaries while the SGR
  pen persists (a program dying mid-`em` must not restyle the next prompt).
- **The §13.5 metrics rule**, the **budgets** (deque eviction + stored cost + the
  per-block CONTINUATION line cap, so no input grows memory unboundedly), and the
  **`raw_vt_intent`** latch (alt-screen / row-addressed control latch it; nothing
  paints; the pane-class flip that consumes it is H-3).

Plus the whole surface the doc walks through: the lib-is-the-brain / bin-is-the-body
split, the `--session` per-user compositor bootstrap and the post-KT-1-audit
compositor, the live-grid + scrollback tile model, the per-leaf tag-bar chrome
(H-3b), the obj menu / verb table / summon (H-3c), the `halcyon.rc`-or-default
session init (H-4c), the unified poll/Loom ingest, and the caveats.

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Clean redirect, zero fold.** The dossier is fresh (2026-09-06), audit:hard,
  claims all 19 halcyond source files, and covers all six audit anchors verified
  atom-by-atom. Nothing load-bearing in this 884-line reference is absent from it,
  and the dossier is the current source of truth (this doc froze at H-4c; the
  dossier tracks the post-KT-1-audit and H-4d/beacon-tier state). Zero code
  change.
