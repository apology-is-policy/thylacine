# 92 — libutopia::line_editor: the `ut` line-editor engine [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-07-utopia-line-editor-doc-absorb`).
`usr/utopia/libutopia/src/line_editor.rs` (U-4 arc) — the pure state machine that
turns a terminal byte stream into an editable line and decides when it is
finished: bytes in, `EditorAction` out, no syscall anywhere. It is the innermost
of the interactive layer's three tiers (line editor / REPL / `ut` binary). Its
content lives, code-verified and current, in:

    vault/system/userspace/shell-tui/sub-utopia-interactive.md   (audit: light,
        guarded-by I-9 wake / I-19 notes / I-20 pts-stop / I-27 trusted-path)

The dossier carries the whole line editor, as-built: the four-state `ParserState`
byte pipeline (Ground/Escape/Csi/Utf8, so a byte-at-a-time paste inserts one
`char` not four broken ones), the emacs C0 bindings, `balance()` (the per-type
bracket/quote/escape/comment walk that decides "more input?" without being a
tokenizer — signed depths, negative-is-balanced so a stray `}` submits to a real
parser error), single- and multi-line rendering through `ansi::visible_width`
(CSI treated as zero-width so colour cannot disturb the cursor), the command-line
validity colouring, Tab completion (the LCP extension + the zsh-style cycling
menu), the command index built once and shared by completion + colouring, the
UTF-8 buffer invariant, Ctrl-R search, smart Up/Down, the per-buffer cap, and the
caveats (the RW-9 R3-F2 multi-line-shrink stale-lines defect, the 256-cap-before-
sort, the completion-index-vs-resolver-disagree-by-`/`, the pollable-`/dev/cons`
header denial, the Bonfire-rename-incomplete, the ansi non-CSI escape).

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Clean redirect for the line editor itself — one adjacent fold.** The line
  editor is fully covered. The fold is `Repl::run_script` (script mode), owed here
  from the eval doc's §13.3 redirect: `ut SCRIPT [args…]` runs a file
  non-interactively (positional-param binding, `interactive = false` so a non-zero
  `$status` fail-fast-propagates per scripture §8.9, `exit N` wins the exit code,
  no line editor / prompt / notes loop / banner, reads no fd 0). Verified in
  repl.rs:805. Folded into sub-utopia-interactive as a mechanism note
  (`chg-2026-09-07-utopia-line-editor-doc-absorb`; `updated:` bumped to
  2026-09-07) — it composes with the shell-side `#!` shebang so `./s.ut` works.
- **Everything else was covered** — every U-4a..d atom (keybindings, the ANSI
  parser, the balance tracker, multi-line render, completion, validity colouring)
  is as-built in the dossier, which is deeper and fresher than this doc. Zero code
  change.
