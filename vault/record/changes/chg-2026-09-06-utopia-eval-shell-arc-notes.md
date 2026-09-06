---
id: chg-2026-09-06-utopia-eval-shell-arc-notes
type: chg
title: "sub-utopia-eval de-stale: the && / || eval half, the six-entry $path, cd --, and the settled notes-mask changes (#237 pipe default, on-note unmask, mask tty:*)"
date: 2026-09-06
arc: arc-vault
commits: []
touched:
  - sub-utopia-eval
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
[[sub-utopia-eval]] (updated 2026-08-16) missed six genuine changes, split across
two arcs. The MAIN shell arc is the eval companion to [[chg-2026-09-06-utopia-parser-andor-eqliteral]];
the AUX notes/job-control changes landed 2026-08-17..19, are settled (aux is on
Nocturne now), and land as UPDATES to prose the dossier already scaffolds (it
already covers `wait_pids_interruptible`, the note-handler registry, the held-
note queue, and `note_mask`). Verified per-file in `eval/`.

MAIN (the shell arc):
- **`&&` / `||` short-circuit** (`ea93d8b7`, `eval_and_or`): the eval half of the
  parser's `AndOr` node -- first pipeline always runs, later ones gated by the
  running `$status`, a link's non-zero exit consumed by its connector so only the
  FINAL status reaches the implicit-fail check (`a || b` tolerates a's failure);
  a control-flow escape wins immediately; `should_propagate_failure` gained an
  AND-OR arm. New Mechanism subsection.
- **`$path` was stale** (`6eb0c7f7`/`1c571a62`/`1cfc9d27`): the dossier said a
  bare name "becomes `/bin/<name>`"; `resolve_command` now searches six dirs in
  order (`/bin`, `/`, `/goroot/bin`, `/clade/bin`, `/viv/bin`, `/viv/abin`),
  extras last so `/bin` stays authoritative, the two `/viv` dirs `MPHENO_LINUX`
  mounts. Corrected in place.
- **`cd --`** (`012d3645`): ends option processing -- the one way into a
  `-`-prefixed directory. Folded as a parenthetical.

AUX (settled notes/job-control, folded as UPDATES):
- **#237 pipe default** (`34809ab3`): `Env.note_mask` seeds to `just(Pipe)` (the
  process default is EPIPE-not-death) because `mask note` SWAPS the whole kernel
  mask; `on note <name>` now UNMASKS the registered note's class so an `on note
  'pipe'` handler can fire. New Mechanism subsection.
- **mask `tty:*`** (`3a7f50f1`): `note_class_for_name` maps any `tty:`-prefixed
  name to the single `NoteClass::Tty` bit (was a no-op that masked nothing).
- **item 10 pts poll bridge** (`6884f06c`): `JobControlState.poll_in_fd` -- the
  `/dev/pts/<n>ready` fd ut polls for fd-0 readiness (the pts slave is not
  directly pollable); degrades to polling fd 0. Folded into the pts wait-path.

Confirmed unchanged: `BUILTIN_NAMES` = 16 (`.` is one; the `cd --` change adds
no builtin); the `=`-literal eval rendering (`eval_value_token`) and the
comment-only `console.rs` TCSAFLUSH reword are below the dossier's threshold and
noted here rather than folded.

`updated:` -> 2026-09-06. Stale backlog 32 -> 31.
