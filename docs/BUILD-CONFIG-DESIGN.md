# BUILD-CONFIG-DESIGN.md — the build configurator + the input manifest

**Status: RATIFIED, 2026-08-24 (user signoff in-session).** Scripture-first per
CLAUDE.md ("Design conversation -> scripture commit"): this doc lands as a scripture
commit BEFORE any code; the implementation commits reference its SHA.

**Ratified decisions (2026-08-24, AskUserQuestion vote + follow-up):**
- Scope = **MVP configurator + the "collect everything" input manifest**.
- The finding-#1 account fix (decouple dev-accounts from the boot-test flag) is
  **folded into the configurator**, not a standalone quick-win first.
- D-a keep `--production`/`--dev` as preset-alias sugar: **yes**.
- D-b honor legacy `THYLACINE_*` env vars as a low-precedence transition shim: **yes**.
- D-c manifest format `tools/build-manifest.toml`: **yes**.
- D-d the collect helper auto-fetches (forks + cache + pull/trigger remote): **yes**.
- D-e audit-bearing at impl for the account-gate move only: **yes**.
- D-f the collect helper is named **`forage`** (thematic; user chose thematic over
  the plain `collect`).
- **NEW (user-added to MVP): a minimal guided WIZARD** (`tools/configure.sh`) that
  interactively walks a newcomer through the profile one symbol at a time, explaining
  what each is and what it enables. See section 4.6.

---

## 1. Thesis

Thylacine's build harness is, structurally, **Buildroot**: one `tools/build.sh`
compiles a kernel + userspace from in-tree source, bakes optional "chunks" into a
persistent image, and assembles a bootable artifact. Its *configuration*, however,
is a pile of overlapping argv flag-bundles plus ~17 scattered `THYLACINE_*` env
vars, with no single artifact describing "what this image is." That is the Gentoo
USE-flag / FreeBSD `src.conf` model **without** the dependency resolver and profiles
that make those tolerable.

The fix is the model Buildroot already proves: **a single typed config artifact,
orthogonal axes, presets stored as files, composed by fragment-merge**, with **a
minimal guided wizard** for newcomers on top. The MVP deliberately excludes the
*full-screen `menuconfig`-style TUI* (a navigable curses grid with live dependency
display) — that remains a deferred nice-to-have. A minimal *linear* wizard (section
4.6) is a far smaller thing and IS in the MVP.

This doc covers the **build-time** layer only. The **install-time** layer (laying a
chosen system onto a target disk, minting the root-of-trust, per-user secrets) is a
*separate layer with a separate artifact* — it already exists as ratified scripture
in `INSTALLER.md`. Mature systems keep these two apart everywhere except Nix (which
fuses them only because its content-addressed store makes build == install, an
architecture Thylacine does not share). We keep them apart.

---

## 2. The problem (as-built; grounded in build.sh + joey.c)

1. **Non-orthogonal flag bundles.** `--production` sets `kernel_tests=OFF` **and**
   `boot_probes=OFF` (two axes, one flag; build.sh:127-133); `--dev` sets
   `kernel_tests=OFF` **and** `boot_probes=ON` (build.sh:136-139).

2. **Login accounts are welded to the test ladder** (the weekend pain, confirmed at
   file:line). `do_corvus_bringup()` is *called* unconditionally (joey.c:9200), but
   every account-creating `USER_CREATE` inside it — michael (1755), susan (1962),
   cora, `GROUP_CREATE wheel` (1843) — sits inside `#if THYLA_BOOT_PROBES`
   (joey.c:1728-2480). So `--production` (`-DTHYLA_BOOT_PROBES` off, build.sh:920)
   compiles account provisioning *out*: corvus comes up, the getty runs, and there
   is no one to log in as. There is no orthogonal "accounts, no tests."

3. **Two ways to set one axis.** hardening / kaslr / sanitize / tickless are each
   settable by BOTH a `--flag` AND a `THYLACINE_*` env var (build.sh:263-267) —
   drift-prone, ambiguous precedence.

4. **No config artifact.** The full build shape is spread across argv flags + ~11
   `THYLACINE_BAKE_*`/control env vars. You cannot diff two build configs, reproduce
   one from a file, or name a preset except as flag-code — `build-everything.sh` *is*
   a hand-coded preset.

5. **Gitignored inputs don't travel** (`.gitignore:2` is a bare `build/`). A fresh
   clone/worktree starts with an empty `build/`; the real inputs are scattered across
   six sibling forks + manual-drop cache files + GCP-built artifacts, and the build
   **silently skips a chunk when its input is absent** (build.sh:590/645/684/...).
   This is the class that cost the weekend (aux was missing `build/clade`, and also —
   found in the inventory — `build/clade/gl/` and all of `build/cache/`).

---

## 3. Prior art -> the model (verified against primary sources)

| System | Model | Artifact | Presets | Layer |
|---|---|---|---|---|
| **Kconfig / Buildroot** | typed symbols (bool/tristate/string/int/choice) + `depends on`/`select` | `.config` | `<name>_defconfig` + fragment-merge | build-time |
| **Nix/NixOS** | declarative typed modules | `configuration.nix` | imported modules | fuses build+install (store-specific) |
| **Gentoo / FreeBSD** | flat per-feature knobs | `make.conf` / `src.conf` | profiles / none | build-time |

**Conclusion:** adopt **Kconfig-as-Buildroot-uses-it**. Steal from Nix only the
*typing discipline* (typed options, defaults, defined override precedence), not its
architecture. The current env-var model is the cautionary row, not the target.

---

## 4. The design — the config artifact

### 4.1 Format + location

A flat `KEY=value` file (Kconfig-*shaped*, not the Kconfig language — a shell-sourced
`KEY=value` reader is enough). Comments with `#`. Symbol names carry no prefix inside
the file (the file's context supplies it); build.sh maps each to its CMake define or
bake env var.

```
# configs/dev.config  (a preset -- see 4.3)
BUILD_TYPE=debug          # debug | release
TESTS=n                   # in-kernel test suite      -> -DKERNEL_TESTS
BOOT_PROBES=n             # joey boot-test ladder     -> -DTHYLA_BOOT_PROBES
DEV_ACCOUNTS=y            # bake dev login accounts   -> -DTHYLA_DEV_ACCOUNTS  (see 4.5)
HARDENING_FULL=y
KASLR=y
SANITIZE=none             # none | ubsan | asan
TICKLESS=y
CHUNK_GOROOT=y            # -> THYLACINE_BAKE_GOROOT
CHUNK_CLADE=y             # implies CHUNK_STORM       -> THYLACINE_BAKE_CLADE
CHUNK_CHASE_W2=n
CHUNK_ALPINE=y            # needs the cache inputs (section 5)
CHUNK_QUAKE=y
CHUNK_DOSBOX=y            # DX-8: the DOSBox-X emulator + /lib/dosbox-x/dosbox-x.conf
CHUNK_DUKE3D=y            # needs CHUNK_DOSBOX (lowered without it)
CHUNK_TOMBRAIDER=y        # needs CHUNK_DOSBOX (lowered without it)
DOSBOX_CPU_PRESET=pentium # xt|286|386|486|pentium|pentium2 -> the baked cycles=fixed N
CHUNK_AURORA_CFG=n
DISK_SIZE=16M
MKFS_SEED=                # empty = random
MKFS_PRESERVE=n
```

- **Presets** live in `configs/*.config` (committed).
- The **resolved active config** is written to `build/.config` (gitignored) for
  inspection + reproducibility — the one-line answer to "what is this image?"

### 4.2 The axis schema (three kinds; each symbol typed + documented)

The schema is the single source of truth for every symbol. Each entry carries:
**name, type, default, a one-line description, and a longer "what it enables / costs"
help string.** The help text is the substrate the wizard (4.6) reads aloud, the
config-file comments are generated from, and any future full TUI would reuse — write
it once, in the schema.

- **compile-shape** — `BUILD_TYPE`, `TESTS`, `BOOT_PROBES`, `DEV_ACCOUNTS`,
  `HARDENING_FULL`, `KASLR`, `SANITIZE`, `TICKLESS`.
- **bake-content** — `CHUNK_GOROOT`, `CHUNK_CLADE` (+`CHUNK_STORM`), `CHUNK_CHASE_W2`,
  `CHUNK_ALPINE`, `CHUNK_QUAKE`, `CHUNK_AURORA_CFG`; since DX-8 (2026-09-05)
  `CHUNK_DOSBOX`, `CHUNK_DUKE3D`, `CHUNK_TOMBRAIDER` and the choice
  `DOSBOX_CPU_PRESET` (the one non-bool bake symbol: the emulated CPU class the
  baked DOSBox-X system config pins).
- **pool-control** — `DISK_SIZE`, `MKFS_SEED`, `MKFS_PRESERVE`.

One symbol per independent decision — the orthogonalization that dissolves the
bundles.

### 4.3 Presets replace the bundles (migration)

The hand-coded bundles become data:

| Preset | TESTS | BOOT_PROBES | DEV_ACCOUNTS | chunks |
|---|---|---|---|---|
| `configs/production.config` | n | n | **y** | lean set |
| `configs/dev.config` | n | n | **y** | dev set |
| `configs/ci.config` | y | y | y (implied) | test set |
| `configs/everything.config` | n | n | y | all chunks on |

`build.sh --config <name>` loads a preset. **Backward-compat migration:**
`--production` becomes sugar for `--config production`, `--dev` for `--config dev`,
`--release` sets `BUILD_TYPE=release`; `build-everything.sh` becomes `--config
everything`. Legacy `THYLACINE_*` env vars are honored during transition as the
lowest explicit override, with a deprecation note; removed once presets cover the
cases. (Keeps CI + existing callers working — no flag-day break.)

The preset table already expresses the case that is **impossible today**: `dev` =
accounts **y**, boot-probes **n** ("accounts, no tests"). That is the finding-#1 fix,
delivered by orthogonalization.

### 4.4 Fragment composition + precedence

Overlays live in `configs/fragments/*.config` (e.g. `kaslr.config`,
`hardening-full.config`, `chunk-go.config`). `build.sh --config production --with
kaslr --with chunk-go` composes them.

**Precedence (last writer wins):**
```
built-in defaults  <  preset (--config)  <  fragments (--with, in order)  <
explicit CLI (--set KEY=VAL, and legacy sugar --kaslr/--hardening-full/...)
```

### 4.5 Account decoupling (folded in per the vote)

Introduce a **new compile symbol `THYLA_DEV_ACCOUNTS`**, split out of
`THYLA_BOOT_PROBES`. The account-*provisioning* block in `do_corvus_bringup` (the
USER_CREATE michael/susan/cora + GROUP_CREATE wheel sequence) moves from
`#if THYLA_BOOT_PROBES` to `#if THYLA_DEV_ACCOUNTS`. The E2E *test* helpers
(`do_login_e2e`, `do_recover_e2e`, the legate prover) STAY under `THYLA_BOOT_PROBES` —
they are tests, not provisioning.

**The dependency (a real Kconfig `implies`):** `BOOT_PROBES=y` implies
`DEV_ACCOUNTS=y` — the boot-test ladder authenticates the accounts, so it cannot run
without them. The resolver enforces this (MVP: if `BOOT_PROBES=y` and
`DEV_ACCOUNTS=n`, auto-raise `DEV_ACCOUNTS` + warn).

**AS-BUILT (option F, ratified 2026-08-24; supersedes the wholesale "move" above).**
The wholesale move proved infeasible: the provisioning in `do_corvus_bringup` is
*interleaved* with test-assertions (AUTH-wrong->BadAuth, pre-elevate
GROUP_CREATE->PermissionDenied, RECOVER, RESOLVE) and depends on ~8 corvus wire
primitives (`corvus_exchange`, `build_user_create`, `build_auth`,
`build_admin_elevate`, ...) that live in the probe-only helper block alongside ~25
probe-only functions. Widening that block to `DEV_ACCOUNTS` would strand those 25
under `-Werror=unused-function` (#229); relocating just the primitives would cut
through the audited identity ladder.

So the account gate did NOT move. Instead a **self-contained
`provision_dev_accounts()`** lands under
`#if defined(THYLA_DEV_ACCOUNTS) && !defined(THYLA_BOOT_PROBES)`, and the full ladder
stays under `THYLA_BOOT_PROBES` untouched. It provisions **michael + cora** (the two
daily-use accounts; susan/wheel + every assertion stay BOOT_PROBES test fixtures):
`USER_CREATE michael` (bootstrap, cap-free while the table is empty) -> `AUTH michael`
-> `ADMIN_ELEVATE(system passphrase)` -> `t_cap_use(HOSTOWNER)` -> `USER_CREATE cora`
(cora needs the elevation because corvus's admin gate requires `CAP_HOSTOWNER` once any
user exists). The elevation-enabling grant caps corvus is spawned with
(`T_CAP_GRANT_HOSTOWNER | T_CAP_GRANT_CLEARANCE`) are stamped *before* the BOOT_PROBES
gate, so the lean path has them. Idempotent on a persistent pool (each `USER_CREATE`
tolerates st==2 = already-exists / admin-gated); any other status fails the boot loudly.

This duplicates the trivial corvus framing (option F's cost) but touches the audited
ladder's *logic* not at all. Only the credential DATA is shared, via `DEV_*` #defines
hoisted above both gates (`DEV_MICHAEL_USER/PASS`, `DEV_CORA_USER/PASS`,
`DEV_SYSTEM_PASS`, `CORVUS_PROTOCOL_VERSION`) so a cross-config persistent pool cannot
drift; the provisioning CONTROL FLOW stays separate. cora carries a short memorable
password by design (the daily login; michael's long password is the admin identity).
(The eventual, cleaner home for account provisioning is first-boot per `INSTALLER.md` --
the install-disc arc, out of scope here -- which retires both copies.)

Landed: `6726ac68` (michael-only, self-contained) + the michael+cora expansion +
shared-#define + F2-F7 audit fixes (this close). Audit-bearing (the elevation spine +
the ladder's #define touch); prosecuted per D-e.

### 4.6 The wizard — guided profile creation (MVP; user-added)

`tools/configure.sh` — a host-side, interactive, **linear** wizard for a newcomer who
knows nothing about Thylacine. Not the full-screen `menuconfig` TUI (deferred); a
sequential Q&A that reads the schema (4.2) and writes a profile.

Flow:
1. **Start from a preset.** "Start from: [1] production  [2] dev  [3] everything
   [4] custom (walk me through everything)." A newcomer picks a sane base in one
   keystroke; `custom` starts from defaults and visits every symbol.
2. **Walk the schema, grouped** (compile-shape -> bake-content -> pool-control), with
   a section header per group. For each symbol print its **name, one-line
   description, and the "what it enables / costs" help** (especially the chunks — e.g.
   "CHUNK_CLADE: the on-device LLVM/Clang toolchain (~1.3 GB, slow first build) --
   lets you compile C/C++ *on* Thylacine"), then prompt with the default in brackets.
   Empty input accepts the default; `?` reprints the long help.
3. **Honor constraints live** (e.g. selecting `BOOT_PROBES=y` announces "-> enables
   DEV_ACCOUNTS (required by the boot-test ladder)").
4. **Flag inputs the chunk needs** (section 5): if a chosen chunk's input is absent,
   say so and name the `forage` remedy, so the newcomer learns before the build fails.
5. **Summarize + confirm**, then write `configs/<name>.config` (or `build/.config`)
   and print the exact next command (`tools/build.sh --config <name>`).

Non-interactive fallbacks: `--from <preset>` (seed), `--defaults` (accept all),
`--edit <config>` (revisit an existing profile). The wizard is pure ergonomics over
the schema + presets — it introduces no config semantics of its own.

**Naming (called, per the discipline):** the wizard stays the plain `configure` —
`./configure` is the single most universally recognized "set up my build" verb in
Unix, and this tool's whole purpose is a reader who does *not* know Thylacine's
identity, where the naming discipline says the standard name wins. (Contrast
`forage`, an internal collector where a thematic name is free — section 8.)

**AS-BUILT (lane 4):** `tools/configure.sh` implements all five flow steps + the
three non-interactive fallbacks, driving the schema core (bc_reset / bc_apply_preset
/ bc_set_one / bc_resolve / bc_emit_config) with no config semantics of its own.
Impl notes: `--defaults` requires a profile name (usage contract); the live
constraint announces on `BOOT_PROBES=y` *selection* (its pin on `DEV_ACCOUNTS`
reinforces it, bc_resolve remains authoritative); the chunk-input flag (step 4)
probes the real GOROOT/CLADE/ALPINE inputs today and names the `forage` remedy
(section 5, next lane) plus a working manual fallback; `BC_DIR_CONFIGS` is
overridable so the test isolates reads+writes to a temp dir. `tools/test-configure.sh`
is 21 discrimination checks (each proven to fail without its behavior via sabotaged
copies), incl. the isolation guard that the real `configs/` is never touched.

---

## 5. The input manifest ("collect everything")

### 5.1 What it pins (grounded in the inventory)

- **Six sibling forks** (in no Thylacine checkout), by exact commit: `go-thylacine`
  4bb69d2, `llvm-thylacine` 251b5b5 (branch `thylacine`, 6-patch series over
  llvmorg-22.1.8), `ambush` 563bae9, `gopls` f65d347, `mesa-thylacine` b7f9ed2,
  `stratum/v2` (`thylacine-pouch-arm`).
- **Two manual-drop cache inputs**: Alpine minirootfs (3.21.0-aarch64) +
  busybox-static (1.37.0-r14.apk) — URL + sha256 each.
- **Network inputs**: `quake106.zip`, and since DX-8 `3dduke13.zip` +
  `tomb3dem.zip` (each sha256-pinned in build.sh; the manifest mirrors the pins
  and `test-forage.sh` A9 fails on drift).
- **Remote-build artifacts** (cannot be cheaply rebuilt on the Mac):
  `build/clade/llvm-build/{bin/llvm,bin/clangd,lib/clang}` and `build/clade/gl/*` —
  each with builder host + source commit + content hash -> **pull-or-rebuild** (via
  `clade-keep-build.sh` / `clade-gcp-build.sh`).
- **The crypto-paired caveat as policy**: `pool.img` + `system.key` + `ramfs.cpio` are
  one atomic unit (fresh random key per pool bake -> `STM_EBADTAG` on mismatch); never
  promise a reproducible pool unless `MKFS_SEED` is pinned.

Committed inputs that already travel need no entry: `usr/https/ca-certificates.crt`,
`third_party/mesa-gl-headers/`, `usr/netd/ndb/local`, `usr/ports/**`,
`usr/lib/pouch/patches/**`, `tools/corvus-mint/`.

### 5.2 Format + location

A committed structured manifest, `tools/build-manifest.toml`, with a section per input
class (forks / cache / remote / pairing-policy). TOML because the entries are
structured (repo + commit + patch-series-path); the config artifact (section 4) stays
flat KEY=value because its entries are scalar.

### 5.3 Consumption: detect-and-instruct + `forage`

- **`build.sh` detects and INSTRUCTS** instead of silently skipping: when a chunk's
  input is absent, print the manifest's remedy ("chunk clade needs `build/clade/gl/` --
  absent; run `tools/forage.sh clade-gl` or `tools/clade-keep-build.sh`"). This alone
  would have saved the weekend.
- **`tools/forage.sh`** (the collector — an animal foraging for scattered provisions)
  reads the manifest and gathers what it can: clone/checkout forks at pinned commits,
  download + sha256-verify the cache inputs, and for the remote artifacts pull a cached
  copy or trigger the GCP builder. What it cannot do automatically it prints as an
  instruction.

This manifest is *also* the payload definition the install disc (INSTALLER.md) will
consume — which is why it is the shared foundation, not a side-quest.

---

## 6. Audit-bearing surfaces

- **Account-provisioning gate move** (4.5, joey.c): adjacent to the A-5 login/identity
  audit surface. The MVP change is "same provisioning code, different compile gate + an
  implies-constraint," so it does not change provisioning logic — but moving
  identity-provisioning code warrants a **self-audit**, and a **focused prosecutor
  round** at impl if the change grows beyond a gate move.
- **The config parser, fragment-merge, preset loader, wizard, and manifest/forage
  tooling are pure build tooling** — no runtime kernel effect, not soundness-bearing.
- **`MKFS_SEED` / `MKFS_PRESERVE`** already sit on an existing audit-trigger row
  (Thylacine mkfs RNG seed pinning); the configurator only *routes* them, but the row's
  discipline still applies to any change in how they are threaded.

Call (ratified D-e): **audit-bearing at impl for the account-gate move only**; the
rest is tooling.

---

## 7. Lane split / sequencing

**ALL LANES LANDED (aux-2, 2026-08-24); arc complete.** Commits noted per lane.

1. **This doc** as a scripture commit. -- DONE `b671c2cb`.
2. **The configurator core**: the KEY=value reader + the axis schema (with per-symbol
   help text, 4.2) + `configs/*.config` presets + fragment-merge +
   `--config`/`--with`/`--set` + the `--production`/`--dev`/`--release` migration +
   `build/.config` emission. -- DONE `4e271a85` (core) + `81eca60e` (build.sh wiring).
3. **The account decoupling** (`THYLA_DEV_ACCOUNTS` + the joey.c gate move + the
   `BOOT_PROBES implies DEV_ACCOUNTS` constraint) — folded into (2) per the vote;
   carries the self-audit / prosecutor round. -- DONE `6726ac68` (michael) + `d982ee62`
   (cora + 2-round audit close, 0 P0/0 P1); as-built option F (see 4.5).
4. **The wizard** (`tools/configure.sh`, 4.6) — builds on the schema + presets from (2).
   -- DONE `0d694b55` (+ `tools/test-configure.sh`, 21 discrimination checks).
5. **The manifest + forage** (`tools/build-manifest.toml` + `build.sh`
   detect-and-instruct + `tools/forage.sh`). -- DONE `3c1a9cb7` (manifest + forage +
   `test-forage.sh`) + `ec4c1ccc` (build.sh detect-and-instruct + `test-detect-instruct.sh`).
6. Docs per the per-PR discipline: `docs/reference/150-build-config.md` (technical) +
   the full `--config`/preset fold into `BUILD-HARNESS.md` (section 4.3-4.5). -- DONE
   this lane. The `docs/manual/` entry is intentionally SKIPPED: the configurator is
   host-side developer tooling, and `docs/manual/` (the OS user manual) stays a
   Phase-0 stub until v1.0-rc per the standing user-manual-deferred policy; the
   developer-facing walkthrough lives in `BUILD-HARNESS.md`, which IS the build
   harness's manual.

The install disc stays roadmapped behind its deps (driver framework + Aurora),
untouched by this arc. git-on-viv remains the active mission; this is the detour.

---

## 8. Thematic naming (ratified)

- **`forage`** — `tools/forage.sh`, the collector that gathers scattered build
  provisions (marsupial foraging). Thematic name adopted (user choice): an internal
  collector where color is free and does not obscure intent.
- **`configure`** — `tools/configure.sh`, the wizard, stays plain: `./configure` is the
  universal Unix "set up my build" verb, and the wizard exists precisely for readers
  who do not know Thylacine's identity, so the standard name wins (naming discipline).
- Load-bearing surfaces stay plain: `configs/*.config`, `build/.config`,
  `--config`/`--with`/`--set`, `build-manifest.toml`.

---

## 9. Status

- **2026-08-24**: scripture RATIFIED (this doc). All six sub-decisions signed off
  (D-a..D-e as recommended; D-f = `forage`), plus the wizard added to the MVP. No code
  yet. Next: the configurator core (lane 2), account decoupling folded in (lane 3),
  the wizard (lane 4), the manifest + forage (lane 5). The account-gate move is
  audit-bearing at impl.
