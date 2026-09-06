# 91 — usr/utopia/ workspace + ut shell skeleton (U-3) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-07-utopia-skeleton-doc-absorb`).
This documented the **U-3 skeleton** — the first chunk of Phase 7's shell half:
the `libutopia` crate (palette + ansi + path helpers) and a `ut` binary that
printed a version banner and exited 0. Everything it described is **long
superseded** by the full interactive shell (the U-4 line editor, U-5 parser, U-6
evaluator, U-7 job control, U-8 builtins, U-9 coreutils all landed), and its
content — where still live — is code-verified and current, *ahead of this doc*, in:

- **the interactive shell + libutopia** — the line editor, REPL, completion,
  palette, ansi and path helpers (all of `usr/utopia/libutopia/src/*` + the `ut`
  binary):

      vault/system/userspace/shell-tui/sub-utopia-interactive.md   (owns all U-3 sources)

- **the parser + the evaluator** (the U-5/U-6 the doc listed as future):

      vault/system/userspace/shell-tui/sub-utopia-parser.md
      vault/system/userspace/shell-tui/sub-utopia-eval.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Nothing load-bearing — a superseded skeleton, and even its palette is
  renamed.** The doc's four Pale Fire roles (BG/FG/PATH/GLYPH) grew to **nineteen
  semantic roles**, and the palette itself was **renamed Pale Fire → Bonfire** — a
  migration `sub-utopia-interactive` not only carries but is *ahead* on, recording
  the finding that the rename "reached the definition and nothing else" (twelve
  stale "Pale Fire" descriptions survive across seven files, this doc among the
  sources it counts). The stable interface is the role *names*, not the hex; a
  retheme changes one file. The `⊢` turnstile / `⋮` continuation glyphs, the
  `abbreviate_home` `~`-abbreviation (partial-component-not-matched), and the
  banner-via-SYS_PUTS-not-fd1 constraint are all carried there. The U-3 workspace
  deviation (libutopia + shell as members of the existing `usr/Cargo.toml` rather
  than a separate workspace root) is as-built history, not a live decision. Zero
  code change.
