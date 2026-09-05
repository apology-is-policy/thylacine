# 150 -- The build configurator + input manifest

**Status:** as-built at the build-configurator arc close (aux-2, 2026-08-24).
Landing commits: `b671c2cb` (scripture) / `4e271a85` (schema core) / `81eca60e`
(build.sh wiring) / `6726ac68` + `d982ee62` (account decouple) / `0d694b55`
(the wizard) / `3c1a9cb7` + `ec4c1ccc` (manifest + forage + detect-and-instruct).

Design: `docs/BUILD-CONFIG-DESIGN.md`. This reference is the as-built maintainer
view; the design doc is the intent. User-facing usage lives in
`docs/BUILD-HARNESS.md`.

---

## Purpose

Before this arc the build's configuration was overlapping argv flag-bundles
(`--production`, `--dev`) plus ~17 scattered `THYLACINE_*` environment variables,
with no single artifact answering "what is this image?". The configurator
replaces that with a **Buildroot/Kconfig-lite** model: one typed config artifact,
orthogonal axes, presets-as-files, and a fragment-merge -- resolved once, then
threaded onto build.sh's existing knobs at a single translation point. The
**input manifest** is the sibling half: the pinned record of every build input
that does not travel in the repo, plus a collector (`forage`) that gathers it and
a build.sh **detect-and-instruct** that names the remedy when an input is absent.

Four host-side bash tools + one TOML data file, all bash 3.2-safe (macOS
`/usr/bin/env bash` is 3.2.57): no associative arrays, no `mapfile`, no
`${var,,}`. None is soundness-bearing (design section 6) -- pure build tooling,
no runtime kernel effect -- **except** the account decouple, which touches A-5
identity provisioning and carried its own prosecutor round (see below).

---

## The config model

### Axes (the schema)

`tools/build-config.sh` holds the schema as **parallel indexed arrays**
(`BC_GROUP` / `BC_NAME` / `BC_TYPE` / `BC_DEFAULT` / `BC_MAP` / `BC_DESC` /
`BC_HELP`), one entry per symbol, built by `bc_def`:

```
bc_def GROUP NAME TYPE DEFAULT MAP DESC HELP
```

- `GROUP` -- one of `compile` (what the kernel IS), `bake` (what ships in the
  image), `pool` (disk/pool control).
- `TYPE` -- `bool` (`y`/`n`), `choice:a,b,...`, or `string`.
- `MAP` -- how `bc_export` threads the symbol onto build.sh's heterogeneous
  knobs: `var:<name>` (y/n -> ON/OFF), `varinv:<name>` (inverted), `buildtype:`,
  `sanitize:`, `def:<NAME>` (a `-DNAME=` CMake define), `env:<NAME>` (y/n ->
  1/0), `want:<NAME>` (an exported want-flag).
- `DESC` / `HELP` -- the one-line description and the long "what it enables /
  costs" text. **The wizard reads HELP aloud**, so a thin HELP is a UX defect,
  not just a doc gap.

Config values live in per-symbol `CFG_<NAME>` shell variables (`printf -v` +
`${!indirect}` indirection -- the bash-3.2 stand-in for an associative array).

### The typed artifact

`bc_emit_config PATH` writes the resolved config: one grouped, commented
`KEY = value` line per symbol. `build.sh` emits it to `build/.config` every run;
the wizard writes it to `configs/<name>.config`. `bc_load_file` reads it back,
tolerating unknown keys / bad values with a warning (Kconfig-style forward-compat
for a superset `.config`).

### Presets + fragments + precedence

Precedence is **call order** (last writer wins), which build.sh drives as:

```
bc_reset               # built-in schema defaults
bc_apply_preset X      # < a configs/X.config preset file
bc_apply_fragment Y    # < configs/fragments/Y.config overlays, in order
bc_set K=V             # < explicit --set on the CLI
bc_resolve             # implies-constraints + final validation
bc_export              # map resolved symbols onto build.sh's knobs
```

Presets (`configs/*.config`): `production` (lean, hardened, release, loginnable),
`dev` (debug, loginnable, Go runtime, no tests), `everything` (every bake chunk),
`default` (the historical dev/CI shape a bare `build.sh` produces: tests + boot
probes ON), `ci`. Fragments (`configs/fragments/*.config`): composable overlays
(`ubsan`, `kaslr`, `hardening-full`, `release`, `chunk-clade`, `chunk-go`).

### Constraints

Two kinds since DX-8 (2026-09-05): **implies** raises the dependency
(`BOOT_PROBES=y` raises `DEV_ACCOUNTS`), **needs** lowers the dependent
(`CHUNK_DOSBOX=n` lowers `CHUNK_DUKE3D` + `CHUNK_TOMBRAIDER`, which default ON --
raising the emulator back would silently undo an explicit 17.6 MB opt-out).
`tools/test-build-config.sh` T-needs covers the lowering + its control.

`bc_resolve` enforces the implies-constraints. The MVP has one: **`BOOT_PROBES=y`
implies `DEV_ACCOUNTS=y`** (the boot-test ladder authenticates the dev accounts,
so it cannot run without them). It auto-raises + warns rather than ever producing
an image whose CI probes would deadlock on a missing login.

### build.sh integration

`tools/build.sh` sources `build-config.sh`, parses the configurator flags
(`--config` / `--with` / `--set` / `--show-config`) plus the legacy sugar
(`--production` / `--dev` / `--release` / `--kaslr` / `--hardening-full` /
`--sanitize` / `--no-tickless`, preserved per design 4.3), applies the `default`
preset when no config-selecting flag ran, then `bc_resolve` + `bc_export`. This
is the ONE translation point from the clean config model to the as-built knobs.

---

## The account decouple (DEV_ACCOUNTS) -- the one audit-bearing piece

**Finding #1:** login accounts were created by `USER_CREATE` calls inside
`#if THYLA_BOOT_PROBES` in `usr/joey/joey.c`, so `--production` (probes off)
compiled account provisioning OUT and the image had no login. The fix split a new
`DEV_ACCOUNTS` axis (`THYLA_DEV_ACCOUNTS`) out of `BOOT_PROBES`.

**As-built (option F):** rather than relocate provisioning through the interleaved
BOOT_PROBES ladder, `joey.c` carries a **self-contained** `provision_dev_accounts`
under `#if defined(THYLA_DEV_ACCOUNTS) && !defined(THYLA_BOOT_PROBES)`, with its
own corvus-framing helpers (`pda_connect` / `pda_write_all` / `pda_read_exact` /
`pda_exchange` / `pda_scrub`). It provisions **michael + cora** at first boot via
the sanctioned elevation spine -- once michael exists corvus admin-gates
`USER_CREATE` (the caller must hold `CAP_HOSTOWNER`), so cora cannot be a second
bootstrap create:

```
USER_CREATE michael (bootstrap)  ->  AUTH michael  ->
ADMIN_ELEVATE(system passphrase) ->  t_cap_use(CAP_HOSTOWNER)  ->  USER_CREATE cora
```

The elevation-enabling grant caps (`T_CAP_GRANT_HOSTOWNER | T_CAP_GRANT_CLEARANCE`)
are stamped at `t_spawn_with_perms` BEFORE the `#if THYLA_BOOT_PROBES` gate, so the
lean build has them. Only credential DATA is shared with the ladder (the `DEV_*`
`#define`s + `CORVUS_PROTOCOL_VERSION`, hoisted above both gates) so a cross-config
persistent pool cannot drift; the control flow stays separate. Idempotent on a
persistent pool (`st==2` = already-exists/admin-gated); any other status fails the
boot loudly rather than boot an unloginnable image.

**Audit:** two adversarial prosecutor rounds (Fable 5, max effort), 0 P0 / 0 P1.
See `memory/audit_*` and the `d982ee62` commit body for the full finding
disposition.

**SECURITY posture:** `DEV_ACCOUNTS=y` bakes accounts with passwords that are
PUBLIC in this repo (`usr/joey/joey.c` `DEV_*_PASS`), and the system passphrase is
public too. This is a development convenience, NOT a deployable posture -- build a
real deployment with `--set DEV_ACCOUNTS=n` and provision at first boot
(`docs/INSTALLER.md`). `configs/production.config` carries this disclosure inline.

---

## The wizard (`tools/configure.sh`)

A linear, interactive Q&A for a newcomer -- NOT the full `menuconfig` TUI
(deferred). Pure ergonomics over the schema; it introduces no config semantics.

Flow: (1) pick a base preset [production | dev | everything | custom]; (2) walk
the schema grouped, each symbol's name + description + long HELP, default in
brackets (Enter accepts, `?` reprints the help); (3) honor the constraint live
(`BOOT_PROBES=y` announces + pins `DEV_ACCOUNTS`); (4) flag any selected bake
chunk whose input is absent on this host + name the `forage` remedy; (5)
summarize + confirm, write `configs/<name>.config`, print `tools/build.sh
--config <name>`.

Non-interactive fallbacks: `--from <preset>` (seed then walk), `--defaults`
(accept the seed as-is; requires a name), `--edit <name>` (revisit an existing
profile). `BC_DIR_CONFIGS` is overridable so the test isolates reads AND writes to
a temp dir (its default is the real `configs/`). Named the plain `configure` per
the naming discipline (design 4.6): the standard "set up my build" verb wins for
an audience that does not know the project's identity.

---

## The input manifest (`tools/build-manifest.toml`)

A `[section.name]` TOML table per input, each with a `forageable` verb telling
`forage` what it can DO:

| verb | meaning |
|---|---|
| `clone` | git clone/fetch the repo + checkout `commit` into `path` |
| `download` | curl `url` into `dir`/`file`, then verify `sha256` |
| `remote-pull` | run `pull` (delegates to the Clade builder; needs it reachable) |
| `remote-source` | a source pin for a remotely-built artifact; not fetched locally |
| `manual` | no public source; the operator supplies it at `path` |
| `auto-at-build` | build.sh fetches it itself; no manual action |

Sections: `[fork.*]` (the 6 sibling forks -- go/ambush/stratum clone from
`apology-is-policy`; gopls is `manual`; llvm/mesa are `remote-source` clade-build
sources), `[cache.*]` (the 3 manual-drop inputs, sha256-pinned), `[network.*]`
(auto-at-build: `quake`, and since DX-8 `duke3d` + `tombraider` -- the two DOSBox
showcase games; the DOSBox-X emulator itself is vendored in-repo and deliberately
NOT an input), `[remote.clade_*]` (the thyla-keep-built artifacts), and
`[pairing.stratum_pool]` (the crypto-paired pool/key/ramfs policy -- a constraint,
not an input). Hashes are byte-exact from build.sh -- and `test-forage.sh` A9
enforces it: every hash/url under `network.*` must appear verbatim in `build.sh`
(two copies of one truth; a pin bumped on one side fails the test). Commit pins
are verified against the local forks.

`forage` reads a **controlled TOML subset**: `[section]` tables + `key = "value"`
/ `key = bareword` + `#` comments. Not a full TOML parser -- no arrays, inline
tables, or multi-line strings. The manifest header pins the subset so nobody adds
a construct the reader silently drops.

---

## The collector (`tools/forage.sh`)

```
forage.sh              report present/ABSENT + the action for every input
forage.sh <target>     gather one: go|ambush|stratum|gopls|llvm|mesa|
                       alpine|busybox|quake|duke3d|tombraider|clade|clade-gl
                       (or any literal manifest section, e.g. network.duke3d)
forage.sh all          gather everything automatable
FORAGE_DRY=1 forage.sh … preview; touch nothing (git/net/gcp)
```

Reader: `manifest_get "section" "key"` (an awk one-pass, quotes stripped) and
`manifest_sections "prefix."` (enumerate). `present` probes per class (a fork's
`probe` marker, a cache file's existence, a remote artifact's `probe` path).
Actions: `do_clone` / `do_download` (sha256-verified) / `do_remote_pull`
(delegates to `tools/clade-keep-build.sh fetch`) / `do_instruct` (the
non-automatable classes -- never a silent no-op). The `alpine` target gathers
both cache inputs; `clade` -> the LLVM toolchain, `clade-gl` -> the GL stack.

Test seams: the dispatch is `BASH_SOURCE`-guarded (the test sources forage.sh and
calls the parser directly); `FORAGE_ROOT` overrides the repo root the
REPO_ROOT-relative inputs resolve under; `MANIFEST` overrides the manifest;
`FORAGE_DRY` previews.

---

## Detect-and-instruct (build.sh)

`forage_hint <target> <name> <path>` prints a one-line remedy naming
`tools/forage.sh <target>`. Wired at the three load-bearing skip sites: the
`/goroot` bake skip (`forage go`), the two Alpine bundle skips (`forage alpine`),
and -- NEW -- `THYLACINE_BAKE_CLADE=1` with no toolchain staged (`forage clade`).
That last closed a real silent gap: #101 warned only the INVERSE
(staged-but-flag-unset); the "requested but not staged" case minted a pool WITHOUT
`/clade` with no message at all.

---

## Tests

All four are host-only bash, no QEMU, ~1 s each, and every check is a
discrimination control proven to fail without its behavior (sabotaged-copy
verified):

| Test | Covers |
|---|---|
| `tools/test-build-config.sh` | the schema core: `bc_*` reset/preset/fragment/set/resolve/export, the constraint, precedence (28 asserts) |
| `tools/test-configure.sh` | the wizard: seed fidelity, interactive input, the live constraint (+ negative), `?`-help reprint, chunk-flagging (+ negative), `--edit`, usage contracts, and an isolation guard that the real `configs/` is untouched (21 asserts) |
| `tools/test-forage.sh` | the manifest reader (incl. section-scoping) + the collector: present/ABSENT, dry-run clone/download/remote-pull, the `alpine` alias, instruct paths, no side effects (19 asserts) |
| `tools/test-detect-instruct.sh` | the tooling->forage CONTRACT: every forage target named by build.sh OR the wizard is a real target (anti-rot for a renamed target) (6 asserts) |

Runtime coverage for the account decouple: `tools/check-production.sh --all`
(builds config A/B/C, size-discrimination, boots the lean image, asserts the
`dev-accounts.exp` login + the spine's completion line); `tools/interactive/dev-accounts.exp`
is a permanent LS-CI scenario (image-agnostic cora + michael login).

---

## Known caveats / footguns

- **The TOML reader is a subset**, not a parser. Adding an array or inline table
  to the manifest silently drops it. Extend `manifest_get` first.
- **`build.sh kernel` re-mints the pool from the AMBIENT environment** (it pulls
  the whole `all` chain). A stale `THYLACINE_*` env var or a staged-but-unflagged
  clade tree therefore changes the image; the #101 + detect-and-instruct warnings
  exist for exactly this.
- **`DEV_ACCOUNTS` passwords are public.** See the security posture above.
- **forage's clade pull needs the builder reachable** (`thyla-keep` running); it
  instructs on failure rather than pretending.
- **The pool/key/ramfs triple is one atomic unit** (`[pairing.stratum_pool]`): a
  fresh random key per pool bake means a pool + a ramfs from different bakes
  mismatch (`STM_EBADTAG`). Never promise a reproducible pool unless
  `THYLACINE_MKFS_SEED` is pinned; re-bake both paired.

---

## Vault

`tools/build-config.sh` / `configure.sh` / `forage.sh` / `build-manifest.toml`
were UNOWNED by the vault at the arc close (all four returned no dossier); this
reference section is the owed prose. A vault sweep to carry these surfaces is
filed against the vault's backlog.
