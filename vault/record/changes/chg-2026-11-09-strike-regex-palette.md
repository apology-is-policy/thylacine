---
id: chg-2026-11-09-strike-regex-palette
type: chg
title: "The shell's =~, palette builtin and prompt function are struck (vote 78)"
date: 2026-11-09
arc: arc-halcyon-interaction
commits: []
touched: [sub-utopia-parser, sub-utopia-interactive]
established: []
closed: []
opened: []
depth: skeletal
created: 2026-11-09
---
Operator vote 78 struck what UTOPIA-SHELL-DESIGN described and `ut` lacked: `=~` (7.3), the `palette` builtin (9.4) and the user `prompt` function (11.1), with their copies in UTOPIA-VISUAL 3.3 and 4 and ROADMAP. Plan 9's rc matches with `~` and globs and has no regex; the REPL's own prompt is what ships and is tested.

`=~` was more than prose: the lexer scanned `/pattern/` after it through a one-shot flag (`TokenKind::EqualTilde`, `TokenKind::Regex`, `ParseErrorKind::UnterminatedRegex`), the parser built `MatchOp::Regex` and `ExprKind::Regex`, the evaluator refused it as NotImplemented, and completion and the line editor skipped a `/regex/`. All of it is gone. `=` and `~` now lex as themselves and a `/` after them begins an ordinary word, so `$x =~ /re/` is a parse error in a condition; `cond_equal_tilde_is_refused` and u-test's probes 6 and 15 assert that, each beside the glob match one variable away, which still parses and matches. The palette builtin and prompt function had no code.
