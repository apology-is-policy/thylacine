---
id: chg-2026-09-06-coreutils-presenters-h1c2-destale
type: chg
title: "sub-coreutils-presenters de-stale: H-1c-2 built the console probe -- --color=auto now means auto across all sixteen, ps joins the set, and four tools gain a Beacon Rich realization"
date: 2026-09-06
arc: arc-vault
commits: ["b28a2154"]
touched:
  - sub-coreutils-presenters
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
[[sub-coreutils-presenters]] (updated 2026-08-04), `audit: none`, was the
larger of the two remaining BIG de-stales and the most sharply superseded: its
central caveat and both halves of its title stated the exact thing H-1c-2
(`8922ccd7`, "the Beacon emitters + the --color=auto unification") fixed. All
15 dossier files changed in that one commit, plus a new sixteenth presenter.
Ground-truthed against source; nothing incremented.

What the commit did, and what the dossier now says:

- **The `{ true }` stub is gone.** Every one of the fifteen
  `fn stdout_is_console() -> bool { true }` stubs became a one-line wrapper
  delegating to `libthyla_rs::stdout_is_terminal()` (over the newly-built
  `SYS_FD_DEVCLASS`; the console's Dev class is `'c'`). So `--color=auto` now
  resolves colour-off in a pipe/file/closed-fd -- the thing the old caveat
  said it did NOT do. The caveat is rewritten as RESOLVED, keeping the history
  because it predicted the fix precisely (the shell already ran this probe; the
  syscall was "reserved and never built" -> it shipped as `SYS_FD_DEVCLASS`).
- **Every one of the sixteen now defaults `ColorMode::Auto`** (measured across
  all bins). The dossier's old asymmetry -- introspection tools default ON,
  `grep` defaults OFF -- is gone: with a working gate, `auto` is what each tool
  wanted, so the Contract + the `grep`-default prosecution bullet were rewritten.
  `grep`'s default-OFF was a workaround for a gate that always said "yes".
- **`ps` added to `code:`** (the sixteenth presenter, `usr/coreutils/src/bin/ps.rs`,
  297 lines, new in the same commit). One atomic `/ctl/procs` slurp (kernel
  renders under `g_proc_table_lock`), three realizations (verbatim pass-through /
  boxed-coloured with humanized CPU + STATE colours / Beacon Rich table with
  `obj type=pid`), and a parse-failure -> verbatim degrade. Documented in
  Mechanism + Error paths.
- **Four tools gained a Beacon Rich realization** (`ls`, `stat`, `grep`, `ps`;
  measured -- the twelve network/other tools emit SGR only). SGR and Rich are
  mutually exclusive (the tool forces colour off at Rich). The gate itself
  (`beacon_gate`) is owned by [[sub-coreutils-lib]] (peer-updated 2026-09-05); a
  Mechanism paragraph documents the presenter-side behaviour and cross-refs it
  rather than duplicating the wire/gate internals.
- **Title changed** -- "fifteen tools, and fifteen copies of one stub" ->
  "sixteen tools, one console probe". Both halves of the old title stated the
  now-fixed state (sixteen, not fifteen; a shared probe, not fifteen stubs), so
  it was updated rather than left contradicting its own body. Id unchanged.
- **Prosecution/task #156** ("hand-written probes should never reach sixteen")
  closed the RIGHT way: the shared probe was built and delegated to, so there
  is exactly one probe body; `ps` calls it directly rather than adding a stub.
- **Coverage note**: still no unit tests (the bins link the runtime
  unconditionally), but `ls-halcyon.exp` now witnesses `ps`'s Beacon framing on
  a live console ("ps framed, ls never did"); the colour flag matrices remain
  unpinned.

Verified before quoting: the sixteen bins present; the delegation body of all
fifteen wrappers; `ps`'s three realizations; the four Beacon adopters; the
per-tool default modes; the landing commit per-file. `updated:` -> 2026-09-06.
Stale backlog 25 -> 24.
