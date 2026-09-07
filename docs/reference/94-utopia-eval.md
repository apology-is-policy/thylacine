# 94 — libutopia::eval: the `ut` evaluator [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-07-utopia-eval-doc-absorb`).
`usr/utopia/libutopia/src/eval/` — the shell's whole consequence surface: AST to
effects. It spawns processes, opens files, wires pipes, forwards notes, forms
process groups, hands the terminal to a foreground job and takes it back, and
flips the console line discipline around a full-screen child. Not a privilege
boundary (the kernel gates every syscall), but within a session the blast radius
is the whole thing. Its content lives, code-verified and current, in:

    vault/system/userspace/shell-tui/sub-utopia-eval.md   (audit: light,
        guarded-by I-19 notes / I-20 pts-stop / I-27 trusted-path / I-28 resolution)

The dossier carries, as-built: the unified-list `Value` model, the three-way
command resolution (function -> builtin -> external) with the six-entry `$path`,
the implicit-fail-is-a-mode discipline, the `&&`/`||` short-circuit AND-OR lists,
the **two foreground wait paths** (console: Ctrl-C to the owner, forwarded to the
live pids via a note-queue poll with a reap-is-ground-truth backstop; pts: the
signal fans to the process group, `WAIT_UNTRACED`, terminal handed and restored on
every outcome), the `/dev/pts/<n>ready` poll bridge, the raw-mode allowlist
(closed, basename-matched, joining is a deliberate edit-plus-test), the
note-held-not-dropped scanners + the `mask note`-swaps-whole mask mirror (with the
#237 pipe-mask fix), the single `EVAL_MAX_DEPTH` counter (one counter, two entry
points — deliberately unlike the parser's three, because a shell mixes its
recursion shapes in one expression), and the caveats (#105 stranded job-table
tests, #106 eval_expr-pure-but-spawns + export-declined, #107/#108 stale comments).

The script-mode entry point lives one layer up:

- **`Repl::run_script`** (`usr/utopia/libutopia/src/repl.rs`) — `ut SCRIPT
  [args…]` non-interactive execution (positional-param binding, `interactive =
  false` so a non-zero `$status` fail-fast-propagates, `exit N` wins the exit
  code, no banner):

      vault/system/userspace/shell-tui/sub-utopia-interactive.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **One code-grounded fold — the external-spawn chokepoint + shell-side `#!`.**
  §13's `build_command` (the single chokepoint all five external-spawn sites route
  through) and the shebang handling (`prepare_argv`/`peek_shebang`/
  `parse_shebang_line` in `eval/stmt.rs`, owned by sub-utopia-eval) were
  uncovered. Folded (`chg-2026-09-07-utopia-eval-doc-absorb`; `updated:` bumped to
  2026-09-07): the kernel loads ELF only, so `#!` is a shell-side Plan 9-lineage
  convention — a match rewrites argv to `[resolve($path, interp), arg?, prog,
  args…]` (interpreter itself `$path`-resolved, at most one arg, Linux/BSD
  convention); a non-match passes through; the Unix permission shape (R to peek, X
  to exec via `OEXEC`) falls out of the mechanism with no shell-added gate; a
  spawn failure is `$status = 127`. Verified in stmt.rs (build_command:611,
  prepare_argv:632, peek_shebang:660, parse_shebang_line:673).
- **The dossier is otherwise AHEAD of this doc** (the AND-OR lists, the six-entry
  `$path`, the #237 pipe-mask, the pts poll bridge). Read the dossier.
- **Everything else was covered** — the value model, the wait paths, the note
  handling, the recursion cap, the glob matcher, and the error taxonomy are all
  as-built in the dossier. Zero code change.
