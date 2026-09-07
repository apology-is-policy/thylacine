---
id: chg-2026-09-07-utopia-line-editor-doc-absorb
type: chg
title: "absorb docs/reference/92-utopia-line-editor: clean redirect + fold Repl::run_script (script mode) into sub-utopia-interactive"
date: 2026-09-07
arc: arc-vault
commits: ["e9fda668"]
touched: [sub-utopia-interactive]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-07
---
libutopia::line_editor (the ut line-editor engine, U-4). quaestor owner:
usr/utopia/libutopia/src/{line_editor,repl,completion,ansi}.rs ->
sub-utopia-interactive (audit:light, guarded-by I-9/I-19/I-20/I-27, fresh
2026-09-05). ALL 4 paths OWNED. Verified atom-by-atom.

ALREADY COVERED (verified, dossier deep + fresh): the four-state ParserState byte
pipeline; emacs C0 bindings; balance() (per-type bracket/quote/escape/comment,
signed depths, negative-is-balanced); single + multi-line render through
ansi::visible_width (CSI zero-width); command-line validity colouring; Tab
completion (LCP + zsh cycling menu); the command-index-built-once shared by
completion+colouring; UTF-8 buffer invariant; Ctrl-R search; smart Up/Down; the
per-buffer cap; and every caveat (RW-9 R3-F2 multi-line-shrink stale lines,
256-cap-before-sort, completion-vs-resolver-disagree-by-/, pollable-/dev/cons
header denial, Bonfire rename incomplete, ansi non-CSI).

THE FOLD (genuine gap -> sub-utopia-interactive, depth rich; updated 2026-09-05
-> 09-07): Repl::run_script (SCRIPT MODE). Owed here from the 94-utopia-eval
absorption, which redirected its section 13.3 (script mode) to this dossier -- the
dossier owns repl.rs but did not describe it (census: 0). Code-verified anti-hollow
(repl.rs:805): ut SCRIPT [args] runs a file non-interactively -- binds positional
params (0/1/2/*) at global scope, sets interactive=false so a non-zero $status
fail-fast-propagates (scripture 8.9, opposite of the interactive REPL), evaluates
via eval_source (one multi-statement parse), returns the exit code (exit N wins,
else last $status), NO line editor/prompt/notes loop/banner, reads no fd 0. The
ut binary's parse_script picks the first non-flag operand; a #!/bin/ut spawn
arrives the same way, so this composes with the shell-side #! shebang
([[sub-utopia-eval]]) into a working ./s.ut. Folded as a mechanism note.

Redirect stub (line editor -> sub-utopia-interactive). Render + lint verified.
Zero code change.
