# Phase 7 — status and pickup guide

Authoritative pickup guide for **Phase 7: Utopia (textual milestone)** — execution Phase 7 per `ROADMAP.md §2.1`; ROADMAP section `§8`. Binding designs: **`docs/UTOPIA.md`** + **`docs/UTOPIA-SHELL-DESIGN.md`** + **`docs/UTOPIA-VISUAL.md`**.

## TL;DR

**Phase 7 is OPEN — scripture landed; implementation underway.** The U-1 scripture commit (this commit's predecessor at the time this status doc lands) materializes the design conversation as durable scripture: the `ut` shell, the `libutopia` shared library, the native Rust coreutils, the `hx` (Helix) editor port, and the Pale Fire visual identity. The U-* chunk arc unfolds from here.

The Phase 7 entry decision (taken under the U-1 scripture conversation):

- **Shell**: `ut`, a new from-scratch design — rc-shaped with refinements per twelve resolved design axes. Native Rust on `libthyla-rs`. NOT a port of rc; NOT bash.
- **Coreutils**: native Rust on libthyla-rs, 9base-shaped. NOT uutils-coreutils.
- **Editor**: `hx` (Helix), ported via Pouch. THE only Pouch consumer in Utopia.
- **Visual identity**: Pale Fire — four colours (`bg`, `fg`, `path`, `glyph`), one glyph (`⊢`), one prompt format. Disciplined in Utopia's programs; user programs colour freely.
- **Runtime**: native libthyla-rs (the Plan 9 split — see `docs/ARCHITECTURE.md §3.5` + `CLAUDE.md` "Native vs ported userspace programs").
- **Workspace**: Cargo workspace at `usr/utopia/`; Helix vendored separately at `usr/helix/`.

## Landed chunks

| Sub-chunk | What | Commit | Tests |
|---|---|---|---|
| U-2b hash fixup | Update U-2b row with the allocator impl's hash. | *(pending)* | — |
| U-2b | `t::alloc` — `ThylaAlloc` `#[global_allocator]` backed by `linked_list_allocator::LockedHeap` over a single SYS_BURROW_ATTACH region (4 MiB initial). Lazy init via atomic state machine; multi-thread-safe. Adds `t_burrow_attach` + `t_burrow_detach` SVC wrappers + T_SYS_BURROW_ATTACH/DETACH (37/38) constants. New `usr/alloc-smoke/` workspace member: native Rust binary exercising Box/Vec/String/small-alloc loop. Wired into `tools/build.sh` + spawned by joey at boot. | `d8e95d3` | usr workspace cargo build clean; `tools/test.sh` boot pass with new joey lines: "alloc-smoke: Box + Vec + String + small-alloc loop OK" + "joey: /alloc-smoke reaped status=0; libthyla-rs::alloc verified" |
| U-2a hash fixup | Update U-2a row with the foundation impl's hash. | *(pending)* | — |
| U-2a | `t::err` (`Error` enum + `Result<T>` + `From<i32>` + `from_syscall_return` + `Display`) + `t::handle` (`Handle` RAII + `Rights` bitflags-newtype + RAII close via SYS_CLOSE on Drop). Foundational types for the libthyla-rs uplift; no_std + no_alloc; backwards-compatible (existing T_RIGHT_* constants + bare wrappers preserved). | `e99bb43` | usr workspace cargo build clean (no warnings); full boot test pass (`tools/test.sh` green; "Thylacine boot OK" reproducible) |
| U-2 hash fixup | Update U-2 row with the scripture amendment's hash. | *(pending)* | — |
| U-2 | Scripture amendment: §15 reframed as "the libthyla-rs uplift to the library Thylacine deserves" — lead-by-example framing, complete module structure, sub-chunk decomposition (U-2a..U-2-test, ~9-12 sessions). §19 + phase7-status.md + UTOPIA.md + ROADMAP §8.1/§8.7 updated. NO code. | `2fbfad3` | — |
| U-1 hash fixup | Update U-1 row with the scripture commit's hash. | *(pending)* | — |
| U-1 | Scripture: UTOPIA.md + UTOPIA-SHELL-DESIGN.md + UTOPIA-VISUAL.md + ARCH/ROADMAP/CLAUDE updates + this doc. NO code. | `c4e57f2` | — |

## Remaining work — the U-* arc

Per `docs/UTOPIA-SHELL-DESIGN.md §19`. Sequenced for dependencies.

The U-2 work was reframed in U-2 scripture amendment from a single "libthyla-rs extensions" chunk into a multi-chunk **libthyla-rs uplift** — the library Thylacine deserves. ~9-12 sessions of foundation work before U-3 begins; investment paying back across every subsequent native Rust program.

| Sub-chunk | Scope | Depends on |
|---|---|---|
| **U-2** | Scripture amendment: §15 (libthyla-rs uplift framing) + §19 update + phase7-status.md U-* arc refresh. NO code. | U-1 |
| **U-2a** | `t::err` (Error + Result + From<i64>) + `t::handle` (Handle RAII + Rights bitflags). Foundational. | U-2 |
| **U-2b** | `t::alloc` (`#[global_allocator]` via burrow_attach). Enables `alloc::*`. Smoke binary with Box/Vec/String. | U-2a |
| **U-2c** | `t::fs::{Path, PathBuf, File, OpenOptions, Metadata, ReadDir}` + `t::io::{Read, Write, Seek, BufRead}`. | U-2b |
| **U-2d** | `t::process::{Command, Child, ExitStatus, Stdio}` + `t::process::pipe()`. | U-2c |
| **U-2e** | `t::notes::{Notes, Note, MaskGuard}` + `t::poll::{PollSet, PollEvents}`. | U-2c |
| **U-2f** | `t::territory::{bind, mount, unmount, pivot_root, rfork}` + `t::cap::{Caps, current, drop}`. | U-2a |
| **U-2g** | `t::thread` + `t::torpor` + `t::time` + `t::rand` + `t::tty`. | U-2c |
| **U-2h** | `t::ninep` (lift 9P client from corvus) + `t::hardware::{Mmio, Irq, Dma}`. Migrates corvus + virtio-* callers in same commit. | U-2c, U-2d |
| **U-2-test** | Cross-module smoke binary on Thylacine. Validates the uplift. | U-2a..U-2h |
| **U-3** | Utopia workspace skeleton: `usr/utopia/{Cargo.toml,shell,libutopia,coreutils}`; libutopia palette + ansi + path modules; `ut` skeleton (version-print + exit); `tools/build.sh utopia` Rust cross-compile wiring; host-bake `/bin/ut`. | U-2-test |
| **U-4** | Line editor in libutopia: raw mode + emacs keybindings + line buffer + multi-line + tab hook + Ctrl-R history hook. Hand-rolled (~1500-2500 LOC); NOT reedline. | U-3 |
| **U-5** | Parser + AST for rc-shape syntax. Pure logic; unit-testable on host. | U-3 |
| **U-6** | Evaluator core + main loop: poll() main loop; built-ins (cd, exit, set, source, fn, alias, eval, type, etc.); external command spawn; pipes; redirection; `?`/try-catch; pipefail. | U-4, U-5 |
| **U-7** | fd-notes job control: Ctrl-C / Ctrl-Z handling; `&`; jobs/fg/bg; on note / mask note. | U-6 |
| **U-8** | Thylacine builtins: bind / mount / unmount / pivot_root / rfork / cap / note. | U-6 |
| **U-9..N** | Coreutils, one or two per chunk: cat, ls, echo, grep, sed, awk, cp, mv, rm, mkdir, find, wc. | U-3 (each independent thereafter) |
| **U-Helix** | Helix port via Pouch. Parallel arc; not in shell critical path. | U-3 (for host-bake) |
| **U-PTY** | PTY infrastructure if not landed by then: `/dev/ptmx`, `/dev/pts/<n>`, `termios` via `/dev/consctl`. | Independent of shell impl until U-Z |
| **U-Z** | The Utopia bring-up integration test (`docs/UTOPIA-SHELL-DESIGN.md §18`). Multiple full-suite passes; perf measurements; doc final pass. | All above |

Rough scale: 27-37 sessions across the arc. The libthyla-rs uplift (U-2..U-2-test) is the heaviest sub-arc — ~9-12 sessions — and is investment in the library every subsequent chunk builds on.

## Exit criteria status

Per `docs/UTOPIA-SHELL-DESIGN.md §18` / `docs/ROADMAP.md §8.2`. Twelve headline checks.

- [ ] Boot a fresh Thylacine VM; reach the Pale Fire `ut` prompt via UART.
- [ ] Multi-stage shell pipeline: `cat /etc/passwd | grep root | cut -d: -f1` produces correct output.
- [ ] Job control via fd-notes: `sleep 100 &` appears in `jobs`; Ctrl-Z + `fg` resume; Ctrl-C terminates.
- [ ] Error model: function with `cmd1?; cmd2?; cmd3?` short-circuits on `cmd2` failure.
- [ ] Namespace builtin: `bind /srv/stratum-ctl /n/stratum`; `ls /n/stratum` shows the Stratum admin surface.
- [ ] Notes builtin: `note send $$ snare:user1` triggers a registered `on note 'snare:user1' { ... }` handler.
- [ ] `hx /etc/hosts` opens Helix; edit + save observable.
- [ ] rc-shape script: `for (f in *.md) { wc -l $f }` runs.
- [ ] Pale Fire prompt renders with the canonical three-segment colour scheme (path `#8898b4`, glyph `⊢` `#e07840`, command `#d8e4f4`).
- [ ] No kernel extinctions, no driver crashes, no zombie processes.
- [ ] No P0/P1 audit findings on the Utopia surface.
- [ ] All planned U-* chunks landed.

## Build + verify commands

To be filled in as `tools/build.sh utopia` wiring lands under U-3. Anticipated shape:

```bash
# Build the Utopia workspace via aarch64-thylacine target (no_std on libthyla-rs)
tools/build.sh utopia

# Build the Helix port via Pouch
tools/build.sh helix

# Build everything (kernel + Utopia + Helix + sysroot + disk)
tools/build.sh all

# Boot + integration tests
tools/test.sh
```

## Trip hazards

- **Native libthyla-rs requires no_std + alloc.** Reaching for a std-dependent Rust crate is the most common mistake. The decision rule (`CLAUDE.md` "Native vs ported"): authored = native, ported = Pouch. Helix is the only Pouch consumer in Utopia.
- **The line editor is hand-rolled.** Reedline assumes std/Pouch; we have a libutopia line editor. Bug surface is real — see `docs/UTOPIA-SHELL-DESIGN.md §11.2`.
- **The fd-notes main loop is the core innovation.** Replaces classical signal handlers. Every shell operation routes through `poll()` (`docs/UTOPIA-SHELL-DESIGN.md §10`).
- **`ut` is NOT bash, NOT rc-as-shipped.** Rc-shaped with twelve refinements (`docs/UTOPIA-SHELL-DESIGN.md §4-§15`). Scripts written for POSIX `sh` do not run unchanged.
- **Pale Fire palette discipline.** Four colours; the `⊢` glyph; coloured by role not by personal preference. See `docs/UTOPIA-VISUAL.md`.
- **The integration test gates Phase 7 exit.** All twelve headline checks must pass.

## References

- `docs/UTOPIA.md` — the experience.
- `docs/UTOPIA-SHELL-DESIGN.md` — the binding design (12 axes + impl roadmap).
- `docs/UTOPIA-VISUAL.md` — Pale Fire palette + glyph + prompt format.
- `docs/ARCHITECTURE.md §3` — language and toolchain (extended in U-1 with native vs ported split).
- `docs/ARCHITECTURE.md §3.5` — the Plan 9 split scripture.
- `docs/ARCHITECTURE.md §23` — POSIX surfaces and the Utopia milestone (updated in U-1).
- `docs/ROADMAP.md §8` — phase definition (rewritten in U-1).
- `CLAUDE.md` "Native vs ported userspace programs" — operational decision rule.
- `docs/POUCH-DESIGN.md` — the ported-code substrate Helix consumes.
- `usr/lib/libthyla-rs/src/lib.rs` — the native runtime crate Utopia extends.

## Predecessors

- Phase 6 (Pouch; ROADMAP `§7A`) — `docs/phase6-status.md` — CLOSED at `218feb0`. Delivered the cross-compilation environment + the boundary-line for ported code.
- Phase 5 (9P + Stratum integration + corvus; ROADMAP `§7`) — `docs/phase5-status.md` — CLOSED. Delivered the 9P client + Spoor + Territory + the corvus precedent for native Rust on libthyla-rs.
