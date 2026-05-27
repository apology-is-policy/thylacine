# 91-utopia — `usr/utopia/` workspace + `ut` shell skeleton (U-3)

Per CLAUDE.md "Reference documentation discipline (load-bearing)" — the
technical reference for the Utopia workspace skeleton landed at Phase 7
U-3. Binding designs: `docs/UTOPIA.md` + `docs/UTOPIA-SHELL-DESIGN.md`
+ `docs/UTOPIA-VISUAL.md`.

## Purpose

U-3 is the **first chunk in the shell half of Phase 7** — the chunk
that creates `usr/utopia/` and the `ut` binary skeleton. The library
half of Phase 7 (the libthyla-rs uplift U-2..U-2-test) is COMPLETE at
the tip preceding U-3 (`4dd21f2`). U-3 opens the shell half:

- A new `usr/utopia/` directory hierarchy for the Utopia surface.
- The `libutopia` Rust crate (palette + ansi + path helpers).
- The `utopia-shell` Rust crate that produces the `/ut` binary.
- Boot-path orchestration so joey spawns `/ut` and verifies its exit.

v1.0-skeleton scope at U-3: `ut` prints a Pale Fire version banner
via libutopia and exits cleanly. The line editor (U-4), parser (U-5),
evaluator (U-6), job control (U-7), Thylacine builtins (U-8),
coreutils (U-9..N), and PTY support (U-PTY) land in subsequent chunks.

## Public API

### `libutopia::palette`

```rust
/// One semantic colour role in the Pale Fire palette.
pub enum Role { Background, Foreground, Path, Glyph }

/// RGB triple in 0..=255.
pub struct Rgb { pub r: u8, pub g: u8, pub b: u8 }
impl Rgb { pub const fn new(r: u8, g: u8, b: u8) -> Self; }

/// Four canonical Pale Fire constants (UTOPIA-VISUAL.md section 1).
pub const BG:    Rgb;  // #0e1018 — cold near-black
pub const FG:    Rgb;  // #d8e4f4 — moonlight (default text)
pub const PATH:  Rgb;  // #8898b4 — receded steel (prompt path)
pub const GLYPH: Rgb;  // #e07840 — ember orange (the `⊢` only)

/// Resolve a Role to its RGB. const fn; zero runtime cost.
pub const fn rgb_of(role: Role) -> Rgb;
```

### `libutopia::ansi`

```rust
/// `ESC[0m` — the canonical reset sequence.
pub const RESET: &str;

/// Compose 24-bit FG / BG SGR escape strings. ECMA-48 format
/// `ESC[38;2;R;G;Bm` and `ESC[48;2;R;G;Bm`.
pub fn fg_seq(rgb: Rgb) -> String;
pub fn bg_seq(rgb: Rgb) -> String;

/// Wrap `text` with role's FG/BG escape + RESET, returning a fresh
/// String. The common-case helpers; programs rarely call fg_seq
/// directly.
pub fn fg(role: Role, text: &str) -> String;
pub fn bg(role: Role, text: &str) -> String;
```

### `libutopia::path`

```rust
/// Abbreviate `path` against `home`. Cases per UTOPIA-VISUAL.md
/// section 3.1:
///
///   path == home          -> "~"
///   path == home + "/..." -> "~/..."
///   otherwise             -> path unchanged
///
/// Both inputs are byte-precise (UTF-8 by convention). A
/// partial-component prefix (e.g. "/home/joey-other" vs
/// "/home/joey") does NOT match.
pub fn abbreviate_home(path: &str, home: &str) -> String;
```

### `libutopia` crate root re-exports

```rust
/// The Pale Fire turnstile glyph -- `⊢` U+22A2 RIGHT TACK
/// (UTOPIA-VISUAL.md section 2). Pinned spelling so every emission
/// site agrees on bytes (`E2 8A A2`).
pub const GLYPH: &str = "⊢";

/// The multi-line continuation glyph -- `⋮` U+22EE VERTICAL ELLIPSIS
/// (UTOPIA-VISUAL.md section 3.2). Rendered in palette::Role::Path
/// colour by the line editor (U-4).
pub const CONTINUATION_GLYPH: &str = "⋮";
```

### `ut` (the `utopia-shell` binary)

```
usage: ut

  At U-3 (v0.0.1-skeleton): prints the Pale Fire version banner
  and exits 0. There is no REPL, no line editor, no parser, no
  evaluator at this chunk.
```

The banner format:

```
<ESC[38;2;224;120;64m>⊢<ESC[0m> ut v0.0.1-skeleton -- <ESC[38;2;136;152;180m>Thylacine textual shell (U-3 skeleton)<ESC[0m>
```

Glyph in ember orange (`#e07840`); plain middle text in terminal
default; tagline in receded steel (`#8898b4`); each coloured segment
closes with the canonical `ESC[0m` reset.

## Implementation

### File layout

```
usr/utopia/
├── libutopia/
│   ├── Cargo.toml              member of usr/ workspace
│   └── src/
│       ├── lib.rs              crate root; module decls + glyph consts
│       ├── palette.rs          Role enum + RGB constants + rgb_of
│       ├── ansi.rs             RESET + fg/bg/fg_seq/bg_seq
│       └── path.rs             abbreviate_home
└── shell/
    ├── Cargo.toml              member of usr/ workspace; produces `ut`
    └── src/
        └── main.rs             rs_main: emits Pale Fire banner + exits 0
```

### Workspace placement (deviation from scripture sketch)

Per `UTOPIA-SHELL-DESIGN.md` section 14.1 the diagram shows
`usr/utopia/` as a Cargo "workspace root" — a separate workspace from
`usr/`'s existing one. **U-3 instead places `utopia/libutopia` and
`utopia/shell` as members of the existing `usr/Cargo.toml`
workspace.** Rationale:

- The directory grouping under `usr/utopia/` (the scripture's
  structural intent) is preserved.
- Single Cargo workspace = single `Cargo.lock` = single `target/` dir
  = single `cargo build --release` from `tools/build.sh::build_userspace`.
  No new `build_utopia` shell function required.
- The existing workspace already follows the same pattern with
  `lib/libthyla-rs` (a sub-path workspace member).
- If a future Utopia chunk needs the separation (e.g., a different
  Cargo profile, a different target spec), splitting B -> A is
  mechanical: add `exclude = ["utopia"]` to `usr/Cargo.toml`, drop
  `utopia/libutopia` and `utopia/shell` from `members`, add a
  `usr/utopia/Cargo.toml` workspace root.

### `usr/utopia/libutopia/Cargo.toml`

```toml
[package]
name = "libutopia"
version = "0.1.0"
edition.workspace = true
publish.workspace = true
license.workspace = true

[lib]
name = "libutopia"
path = "src/lib.rs"

[dependencies]
libthyla-rs = { path = "../../lib/libthyla-rs" }
```

### `usr/utopia/shell/Cargo.toml`

```toml
[package]
name = "utopia-shell"
version = "0.1.0"
edition.workspace = true
publish.workspace = true
license.workspace = true

[[bin]]
name = "ut"
path = "src/main.rs"

[dependencies]
libthyla-rs = { path = "../../lib/libthyla-rs" }
libutopia   = { path = "../libutopia" }
```

### `ut` binary shape

`utopia-shell/src/main.rs` follows the established native-Rust binary
convention (matches `alloc-smoke`, `u-test`, the virtio-* drivers):

- `#![no_std] #![no_main]` at the crate root.
- `extern crate alloc;` for `String` / `format!`.
- `#[global_allocator] static GLOBAL_ALLOCATOR: ThylaAlloc = ThylaAlloc;`
  per the `libthyla-rs` convention (every consumer declares its own
  global allocator).
- `#[no_mangle] pub extern "C" fn rs_main() -> i64` — the entry the
  libthyla-rs `_start` invokes; the return value tail-calls
  `SYS_EXITS`.

`libthyla-rs` provides `_start` (kept alive by
`usr/scripts/aarch64-userspace.ld::ENTRY(_start)`) and
`#[panic_handler]`; `ut` does not redeclare either.

### Build wiring

`usr/Cargo.toml::members` extended:

```toml
members = [..., "u-test", "utopia/libutopia", "utopia/shell"]
```

`tools/build.sh::build_ramfs` extended:

```bash
local usr_rs_bins=( ... "u-test" "ut" )
```

The existing `build_userspace` builds the entire workspace via
`cargo build --release`; the `aarch64-unknown-none` target +
`scripts/aarch64-userspace.ld` linker script + hardening flags come
from `usr/.cargo/config.toml`. No new shell function needed.

### Joey orchestration

`usr/joey/joey.c::main` extended with a 7-line block AFTER the
`/u-test` block. Pattern matches every other Rust binary joey spawns
(hello, alloc-smoke, u-test, thread-probe):

```c
const char ut_name[] = "ut";
long ut_shell_pid = t_spawn(ut_name, sizeof(ut_name) - 1);
if (ut_shell_pid <= 0) { t_putstr("joey: t_spawn(\"ut\") FAILED\n"); return 1; }
int ut_shell_status = -1;
long ut_shell_reaped = t_wait_pid(&ut_shell_status);
if (ut_shell_reaped != ut_shell_pid || ut_shell_status != 0) {
    t_putstr("joey: /ut orchestration FAILED\n"); return 1;
}
t_putstr("joey: /ut reaped status=0; Utopia shell skeleton verified\n");
```

Variable names disambiguate from the `/u-test` block (which uses
`ut_pid` / `ut_status` / `ut_reaped`): the shell block uses
`ut_shell_pid` / `ut_shell_status` / `ut_shell_reaped`.

## Data structures

### `palette::Role`

```rust
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum Role {
    Background,  // #0e1018 -- cold near-black
    Foreground,  // #d8e4f4 -- moonlight; default text colour
    Path,        // #8898b4 -- receded steel; prompt path only
    Glyph,       // #e07840 -- ember orange; `⊢` only
}
```

### `palette::Rgb`

```rust
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct Rgb { pub r: u8, pub g: u8, pub b: u8 }
```

3 bytes; no alignment requirement. `const fn` constructor.

## Tests

### Skeleton smoke at boot

Joey spawns `/ut`, expects `exit_status == 0`, prints:

```
joey: /ut reaped status=0; Utopia shell skeleton verified
```

The Pale Fire banner itself emits to the boot UART via SYS_PUTS
(console-direct, NOT through fd 1) — verified by `cat -v` on the
boot log:

```
^[[38;2;224;120;64m⊢^[[0m ut v0.0.1-skeleton -- ^[[38;2;136;152;180mThylacine textual shell (U-3 skeleton)^[[0m
```

Five emit-points verified end-to-end:

1. Glyph FG escape: `ESC[38;2;224;120;64m` (`#e07840` = `palette::GLYPH`) ✓
2. `⊢` character (UTF-8 `E2 8A A2`) ✓
3. First reset: `ESC[0m` ✓
4. Path FG escape: `ESC[38;2;136;152;180m` (`#8898b4` = `palette::PATH`) ✓
5. Final reset: `ESC[0m` ✓ (no colour bleed into joey's next line)

### libutopia unit tests (host-only)

The library carries `#[cfg(test)] mod tests` in each module. Cargo
test on a host target would exercise:

- `palette`: hex values match scripture, `rgb_of` resolves each role.
- `ansi`: `fg_seq` / `bg_seq` produce the ECMA-48 24-bit format,
  `fg` / `bg` wrap with RESET, RESET is canonical.
- `path`: exact home → `~`, prefix → `~/...`, non-home unchanged,
  empty home unchanged, trailing slash handled, partial component
  not matched.

The tests are `#[cfg(test)]`-gated so they compile away under
`cargo build --release --target aarch64-unknown-none` (the production
path); they live in the source as inline documentation + would catch
regressions if `cargo test` against the host target is ever wired up.

## Error paths

`ut` has no fallible operations at U-3. The skeleton:

1. Constructs a heap String (cannot fail; `ThylaAlloc` panics on
   OOM via the `#[panic_handler]` from libthyla-rs, which tail-calls
   `SYS_EXITS(1)`).
2. Calls `t_putstr` (returns `-1` on validation failure; v1.0 boot
   path treats t_putstr return as ignored — the boot log is best-effort).
3. Returns `0` from `rs_main`; `_start` tail-calls `SYS_EXITS(0)`.

`libutopia::palette` and `libutopia::path` are pure logic; no fallible
paths. `libutopia::ansi::fg_seq`/`bg_seq` allocate via `format!`;
panic-on-OOM via the consumer's `#[global_allocator]`.

## Performance characteristics

The `ut` ELF is ~53 KB stripped (vs 32 KB for `hello-rs`). The
overhead is `libutopia` + the bumped String allocation paths from
`alloc::format!`. ut runs in ~10ms wallclock from spawn to reap (boot
log entry to the next entry).

`libutopia::palette` is all `const fn` + `Copy` types; zero runtime
cost when the role is a literal.

`libutopia::ansi::fg_seq` / `bg_seq` allocate one short String each
(15-20 bytes); `fg`/`bg` allocate three pushes (escape + text + reset)
into a single String. The Pale Fire banner allocates three Strings
total per emission (two `fg(...)` calls + one outer compose) — well
within the ThylaAlloc's 4 MiB heap (sub-100 byte total).

`libutopia::path::abbreviate_home` allocates at most one String the
length of `path`. Worst case is a HOME-prefix match where the output
is `~/` + the suffix.

## Status

- **U-3 LANDED** at this chunk.
- libutopia v0.1.0: palette + ansi + path modules.
- utopia-shell v0.1.0: `ut` binary (Pale Fire banner + exit).
- usr/ workspace extended with 2 new members (`utopia/libutopia` +
  `utopia/shell`).
- cpio extended with `ut`; 40 entries (was 39 at the U-2-test tip).
- joey extended with a 7-line spawn-and-verify block.

## Known caveats / footguns

- **Two-workspace future**: if a future chunk needs a separate
  `usr/utopia/Cargo.toml` workspace root (e.g., for a different Cargo
  profile or a coreutils sub-workspace per `UTOPIA-SHELL-DESIGN.md`
  section 14.1's "sub-workspace OR cargo group" parenthetical),
  the lift is mechanical — drop the two `utopia/*` lines from
  `usr/Cargo.toml::members`, add a new `usr/utopia/Cargo.toml`
  workspace root, add `exclude = ["utopia"]` to the outer
  `[workspace]` table. No code under `usr/utopia/` needs to change.
- **Banner emits via SYS_PUTS, not fd 1**: same constraint as
  `hello-rs`. A future Utopia builtin that wants to capture and
  validate stdout (e.g., the test path for `echo`) needs a binary
  that writes via fd 1 (libthyla-rs::io::Write on Stdio). The
  v1.0-skeleton banner does not require this; the constraint is
  documented here for the U-4..U-Z arc.
- **`#[cfg(test)]` blocks reference std-only deps**: the unit tests
  in `palette`/`ansi`/`path` use `assert_eq!` and `String`. They're
  cfg(test)-gated so they don't affect the `aarch64-unknown-none`
  production build; if someone wires up `cargo test --target=<host>`
  they'll need to declare `extern crate std;` or thread the alloc
  imports differently.
- **Pale Fire palette discipline is voluntary**: nothing in the
  Rust type system prevents a future Utopia binary from emitting a
  non-palette colour. The discipline is at the program-author level
  (per UTOPIA-VISUAL.md section 1's "discipline applies to Utopia's
  own programs"). Linter enforcement could be a v1.x consideration.

## References

- `docs/UTOPIA.md` — top-level experience doc.
- `docs/UTOPIA-SHELL-DESIGN.md` — the binding shell design.
- `docs/UTOPIA-VISUAL.md` — Pale Fire palette + glyph + prompt format.
- `docs/ARCHITECTURE.md` section 3.5 — the Plan 9 native-vs-ported split.
- `docs/ROADMAP.md` section 8 — Phase 7 (Utopia).
- `docs/phase7-status.md` — landed chunks; U-* arc.
- `docs/reference/90-u-test.md` — predecessor (U-2-test, closes U-2 arc).
- `usr/utopia/libutopia/src/` — the library source.
- `usr/utopia/shell/src/main.rs` — the `ut` binary source.
- `usr/lib/libthyla-rs/src/lib.rs` — the runtime crate `libutopia`
  depends on (its `_start` + `#[panic_handler]` + every typed
  syscall wrapper from the U-2 uplift).
