---
id: chg-2026-09-06-coreutils-filters-destale
type: chg
title: "coreutils-filters de-stale: the ps-driven partition recount (51->52, 15->16), the which drift narrowed to the single / entry, realpath's shared path::normalize, and mkdir -p's race-tolerant re-check"
date: 2026-09-06
arc: arc-vault
commits: []
touched:
  - sub-coreutils-filters
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
[[sub-coreutils-filters]] (updated 2026-08-04) missed three self-contained
landings, each verified in the current source; the stale tool flagged
`mkdir.rs`/`realpath.rs`/`which.rs`, and the ground-truth (`<dossier>..HEAD`,
per-file) confirmed exactly three source commits, none of them aux-active.

- **The partition recount** (H-1c-2 `8922ccd7` built `ps`, a new colour-linking
  presenter). MEASURED at HEAD: 52 coreutils bins total = 36 filters (none
  reference `coreutils::palette`/`color`) + 16 that do (`ps` is the sixteenth,
  via `palette::GREEN/EMBER/GOLD` on the kernel state vocabulary). The
  dossier's own numbers were stale by ps: "fifty-one" -> **fifty-two** total,
  "all fifteen of the others" -> **sixteen**. The partition-is-exact claim is
  preserved (36 + 16 = 52); the "36" filter count itself was and stays correct.
- **The `which` drift caveat narrowed to one entry** (`1c571a62` +
  X-2 `1cfc9d27`). The mechanism is unchanged and still true, but the specifics
  had rotted. Shell `resolve_command` now iterates SIX
  (`/bin/`,`/`,`/goroot/bin/`,`/clade/bin/`,`/viv/bin/`,`/viv/abin/`); login
  seeds FIVE (`/bin:/goroot/bin:/clade/bin:/viv/bin:/viv/abin`) -- the env
  drops exactly the namespace root `/`. So the residual #159 drift is now the
  single `/` entry (a `/`-resident boot-test binary still runs-but-reports-not-
  found). The `/viv/bin` instance -- the "git ran while `which git` failed"
  symptom -- was CLOSED at X-2 (W1-b) by seeding both surfaces; `/clade/bin` was
  added to both at once. (which.rs's own header still lists only three of the
  five env entries -- minor code-comment drift, left to the code.)
- **realpath's shared normaliser** (H-1c-2 `8922ccd7`). `realpath.rs` no longer
  defines `normalize` inline; it is `coreutils::path::normalize`, shared with the
  colour presenters (`ls`/`ps`/`stat`) that emit cleaned-absolute obj refs. It
  is the first piece of real *behaviour* (not just `--help`/usage plumbing) to
  cross the filter/presenter partition -- folded as a Mechanism note.
- **mkdir -p race-tolerant re-check** (X-2 `1cfc9d27`, the X-7 fix). `mkdir_p`'s
  benign case changed from "the create said EEXIST" to "the component is a
  directory afterwards" (re-checked after the failed create), because an
  ancestor that exists inside an unwritable parent answers permission-denied,
  not exists -- which broke `-p` essentially everywhere outside `/`. Folded into
  the per-tool Mechanism cluster. (The residual `/home/<user>` mount-point
  refusal is a separate namespace matter, not re-verified here, so not asserted.)

`updated:` -> 2026-09-06. Stale backlog 35 -> 34.
