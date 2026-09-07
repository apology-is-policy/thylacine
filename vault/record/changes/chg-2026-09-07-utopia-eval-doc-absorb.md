---
id: chg-2026-09-07-utopia-eval-doc-absorb
type: chg
title: "absorb docs/reference/94-utopia-eval (the ut evaluator): fold the external-spawn chokepoint + shell-side #! shebang into sub-utopia-eval"
date: 2026-09-07
arc: arc-vault
commits: ["57d965e4"]
touched: [sub-utopia-eval]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-07
---
libutopia::eval (the ut evaluator, AST -> effects). quaestor owner:
usr/utopia/libutopia/src/eval/*.rs -> sub-utopia-eval (audit:light, guarded-by
I-19/I-20/I-27/I-28, fresh 2026-09-06). ALL 4 sampled paths OWNED. Verified
atom-by-atom.

ALREADY COVERED (verified, dossier deep + fresh + AHEAD of the doc): the unified-
list Value model; three-way command resolution + six-entry $path; implicit-fail-
is-a-mode; the &&/|| AND-OR short-circuit; the TWO foreground wait paths (console
Ctrl-C-to-owner forward with reap-is-ground-truth backstop + pts pgrp WAIT_UNTRACED
terminal-restore-on-every-outcome); the /dev/pts/<n>ready poll bridge; the closed
raw-mode allowlist; note-held-not-dropped scanners + mask-note-swaps-whole mirror
(#237 pipe-mask); the single EVAL_MAX_DEPTH counter (one counter two entry points,
deliberately unlike the parser's three); the #105/#106/#107/#108 caveats.

THE FOLD (genuine gap -> sub-utopia-eval, depth rich; updated 2026-09-06 -> 09-07):
the EXTERNAL-SPAWN CHOKEPOINT + shell-side #! shebang (doc section 13.1/13.2, in
eval/stmt.rs which the dossier owns but did not describe). Code-verified anti-
hollow (stmt.rs build_command:611, prepare_argv:632, peek_shebang:660,
parse_shebang_line:673): all five external-spawn sites route through
build_command; the kernel loads ELF only so #! is a shell-side Plan 9-lineage
convention -- a match rewrites argv to [resolve($path,interp), arg?, prog, args...]
(interp itself $path-resolved so #!ut works; at most ONE arg, Linux/BSD
convention); a non-match (\x7fELF/unreadable/non-#!) passes through, no recursion;
the Unix permission shape (R to peek + X to exec via OEXEC) falls out with no
shell-added gate; spawn failure = $status 127. Folded as a new mechanism
subsection.

NOTED: section 13.3 Repl::run_script (script mode) is in repl.rs ->
sub-utopia-interactive, redirected there in the stub.

Redirect stub (eval -> sub-utopia-eval; script mode -> sub-utopia-interactive).
Render + lint verified. Zero code change.
