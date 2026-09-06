# 108 — libutopia::repl + the ut REPL main loop (U-6g) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-utopia-repl-doc-absorb`).
The Utopia shell's read-parse-eval loop. Its content lives, code-verified and
current, in:

- the **REPL loop + the line editor + the prompt + background-job reaping** —
  the pure editor (`feed_byte`/`feed_bytes` → `EditorAction`, `render(prompt)`),
  the fd-agnostic `Repl::feed(bytes, out) → Some(exit_code)`, the `ut` main loop,
  the single/multi-line prompt rendering, and the U-7a/U-7b job reaping:

      vault/system/userspace/shell-tui/sub-utopia-interactive.md   (owns repl.rs)

- the **parse half** — tokenization + the grammar:

      vault/system/userspace/shell-tui/sub-utopia-parser.md

- the **eval half** — `run_line`, command execution, the error policy:

      vault/system/userspace/shell-tui/sub-utopia-eval.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Nothing load-bearing — a clean zero-fold.** The three-tier split (editor /
  REPL / eval) the doc describes is exactly the three utopia dossiers, all
  current; the REPL loop and the pure line editor are `sub-utopia-interactive`'s,
  the parse and eval halves their own dossiers'.
