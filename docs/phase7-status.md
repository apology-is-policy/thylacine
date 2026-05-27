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
| U-2f hash fixup | Update U-2f row with the impl's hash. | *(pending)* | — |
| U-2f | `t::territory::{mount, bind_replace, bind_before, bind_after, unmount, chroot, pivot_root, MountFlags, PathId}` + `t::cap::{Caps, Stripes, grant, use_grant}`. Plan 9 namespace composition + the two-phase cap-elevation flow. mount/unmount key by an abstract u32 `path_id_t` at v1.0 -- string-path resolution is a v1.x kernel lift; libthyla-rs docs the future shape. Source handles via `&impl AsFd` (reuses U-2e's trait; File + future SrvConn compose for free; the kernel validates KOBJ kind + rights at the syscall boundary). chroot is the initial-bringup primitive; pivot_root is the long-running-Proc variant (audit-trackable, the joey-uses-this path from 16c). bind_before/bind_after/bind_replace are sugar that route through mount() with the matching MountFlags. New syscall constants: T_SYS_MOUNT=14 / UNMOUNT=15 / CHROOT=35 / PIVOT_ROOT=53; new SVC wrappers t_mount/t_unmount/t_chroot/t_pivot_root. T_MREPL/MBEFORE/MAFTER/MCREATE flag constants + TPathId alias. **No `current()` or `drop()` at v1.0**: the kernel exposes no cap-query or non-spawn cap-drop syscall. Programs that need to drop caps spawn a child via Command (U-2d) with reduced `cap_mask`; cap-querying is v1.x. CAP_HOSTOWNER ELEVATION-ONLY restriction documented (kernel-enforced; libthyla-rs comments the trap). alloc-smoke extended ~150 LOC: MountFlags + Caps bitops round-trip (compile-time + runtime against the kernel bit positions); positive mount/unmount round-trip on free path_id 0x7E5701 using /system.key as Spoor source; bind_before/bind_after/bind_replace variants; bogus-fd negative; cap::grant without CAP_GRANT_HOSTOWNER → PermissionDenied; cap::use_grant without console-attach + pending grant → PermissionDenied. | `0391383` | usr workspace cargo build clean (no warnings); `tools/test.sh` boot pass with new joey line: "alloc-smoke: Territory + Cap OK" — validates SYS_MOUNT + SYS_UNMOUNT + SYS_CAP_GRANT + SYS_CAP_USE end-to-end through the typed Rust surface |
| U-2e hash fixup | Update U-2e row with the impl's hash. | *(pending)* | — |
| U-2e | `t::notes::{Notes, Note, NoteClass, NoteMask, NoteTarget, MaskGuard}` + `t::poll::{PollSet, PollEvents, PollTimeout, PollEvent, PollResults, AsFd}`. Notes is RAII over SYS_NOTE_OPEN (fd-shaped path -- the canonical Thylacine surface per NOVEL.md §3.1); read() blocks, try_read() probes via poll(0). NoteMask is a per-Thread bitflag set of NoteClass (Interrupt/Kill/Pipe/ChildExit/Snare); set_mask swaps and returns prior; with_mask returns a MaskGuard that restores on Drop. notes::send accepts NoteTarget::{SelfProc, Pid(Pid)}; validates empty/oversize names AND rejects `snare:`-prefixed names at the boundary (kernel-synthetic reservation). PollSet is a reusable Vec<TPollFd> with parallel events-vector for clean re-arm on each poll; supports add/add_raw/remove/remove_raw/clear/len; empty-set poll short-circuits to Ok(empty). PollEvent helpers (is_readable/is_writable/is_hup/is_err). AsFd trait bridges typed wrappers to PollSet; File and Notes both impl. New syscall constants: T_SYS_NOTE_OPEN=44 / NOTIFY=45 / NOTED=46 / POSTNOTE=47 / NOTE_MASK=48. SYS_NOTIFY/SYS_NOTED constants surfaced for v1.x async-handler callers; libthyla-rs surfaces only the fd path at v1.0. ABI mirror: TNoteRecord #[repr(C)] 32-byte (16 name + u32 arg + u32 sender_pid + u64 timestamp_ns); const-assert pins size. T_NOTE_BIT_* + T_NOTE_MASK_SUPPORTED + T_POSTNOTE_SELF_PID + T_NOTED_NCONT/NDFLT constants. alloc-smoke extended: drains any child_exit synthetic post from U-2d's wait(), validates mask round-trip + with_mask RAII restore, exercises send validation (empty/oversize/snare:), send + read("interrupt"), PollSet pipe round-trip + notes-fd integration. | `6b5811c` | usr workspace cargo build clean (no warnings); `tools/test.sh` boot pass with new joey line: "alloc-smoke: Notes + Mask + PollSet OK" — validates SYS_NOTE_OPEN + SYS_POSTNOTE + SYS_NOTE_MASK + SYS_POLL + the kernel-synthetic child_exit poster end-to-end through the typed Rust surface |
| U-2d hash fixup | Update U-2d row with the impl's hash. | *(pending)* | — |
| U-2d | `t::process::{Command, Child, ExitStatus, Stdio}` + `t::process::pipe()` free fn. Command is a std::process-shaped builder (new/arg/args/stdin/stdout/stderr/caps/spawn). Stdio = Inherit \| Piped \| File(File); Null deferred (no kernel /dev/null analog). Child::wait reaps via t_wait_pid; ExitStatus::success/code/raw. Pipe cleanup discipline: PreparedStdio tracks (child_fd, keep_through_syscall, parent_keeps) so the parent's copy of the child-end pipe fd drops at end of spawn(). Adds T_SYS_WAIT_PID=22 + T_SYS_SPAWN_FULL_ARGV=49 syscall constants, t_wait_pid + t_spawn_full_argv SVC wrappers, #[repr(C)] TSpawnArgs (56-byte mirror with compile-time size assertion), T_SYS_SPAWN_ARGV_MAX/DATA_MAX + T_SPAWN_NAME_MAX/MAX_FDS bounds. File::from_raw_handle pub(crate) helper for wrapping kernel-returned fds (pipes, etc.). alloc-smoke extended: pipe() round-trip + EOF + Command::spawn(hello-rs).stdin/stdout/stderr(Piped).spawn().wait().success(). | `ecf1a1a` | usr workspace cargo build clean; tools/test.sh boot pass with new joey lines: "hello from /hello-rs" (alloc-smoke-spawned) + "alloc-smoke: pipe + Command + Child OK" |
| U-2c-fs hash fixup | Update U-2c-fs row with the impl's hash. | *(pending)* | — |
| U-2c-fs | `t::fs::{Metadata, OpenOptions}` + free functions `fs::{metadata, exists, is_file, is_dir}`. Metadata is `#[repr(C)]` 72-byte mirror of kernel `struct t_stat` with const fn accessors (len, is_file, is_dir, is_char_device, is_symlink, is_empty, mode, permissions, nlink, atime/mtime/ctime_sec, qid_path/vers/type, blksize, blocks). OpenOptions builder mirrors std::fs::OpenOptions: read/write/truncate compose to omode; append/create/create_new return Error::NotImplemented at v1 (no kernel SYS_TLCREATE/append surface). `File::metadata()` method backed by SYS_FSTAT. New syscall: T_SYS_FSTAT=50 + t_fstat wrapper. T_S_IF{MT,REG,DIR,CHR} mode constants added. **Deferred from U-2c-fs to a future sub-chunk**: ReadDir / DirEntry (no kernel directory-read mechanism exposed); free fns canonicalize/remove_file/copy (no kernel surfaces yet). alloc-smoke extended with Metadata + free fn + OpenOptions error-case validation. | `3c8a49b` | usr workspace cargo build clean (no warnings); `tools/test.sh` boot pass with new joey line: "alloc-smoke: Metadata + OpenOptions + free fns OK" |
| U-2c-io hash fixup | Update U-2c-io row with the impl's hash. | *(pending)* | — |
| U-2c-io | `t::io::{Read, Write, Seek, BufRead, SeekFrom, BufReader, Cursor}` traits + concrete adapters (mirror std::io). `t::fs::File` RAII over per-component SYS_WALK_OPEN walk; impls Read/Write/Seek. New syscall wrappers: t_walk_open + t_lseek + constants (T_OREAD/OWRITE/ORDWR/OTRUNC, T_SEEK_SET/CUR/END, T_WALK_OPEN_FROM_ROOT, T_WALK_OPEN_NAME_MAX). Error gets two library-only variants (UnexpectedEof, WriteZero; non-exhaustive enum, backwards-compat). File::open absolute paths only at v1 (relative + .. parent-traversal return InvalidArgument). alloc-smoke extended with Cursor + File::open(/system.key) + Read + Seek validation. | `c19e0f9` | usr workspace cargo build clean (no warnings); `tools/test.sh` boot pass with new joey line: "alloc-smoke: Cursor + File + Read + Seek OK" — validates SYS_WALK_OPEN, SYS_READ, SYS_LSEEK round-trip through the typed Rust trait surface |
| U-2c-path hash fixup | Update U-2c-path row with the impl's hash. | *(pending)* | — |
| U-2c-path | `t::fs::{Path, PathBuf, Component, Components, Display, SEPARATOR}`. Pure logic, no syscalls. Path is `#[repr(transparent)]` newtype over str; PathBuf wraps String. UTF-8 by convention. parent/file_name/file_stem/extension/join/starts_with/ends_with/components iterator. Full Ord/Eq/Hash/Display/Debug. Per-binary `#[global_allocator]` declaration added to the 11 libthyla-rs consumers (hello-rs, mmio-probe, irq-probe, irq-bench, virtio-blk-probe, virtio-blk-rw, virtio-net-probe, virtio-net-arp, virtio-net-loop, virtio-input, virtio-gpu) — sets the project-wide convention that every native Rust binary opts in to ThylaAlloc. U-2c split from one chunk into U-2c-path / U-2c-io / U-2c-fs (3 thin slices). alloc-smoke extended with Path/PathBuf/Components validation. | `d141398` | usr workspace cargo build clean; `tools/test.sh` boot pass with new joey line: "alloc-smoke: Path + PathBuf + Components OK" alongside the existing alloc lines |
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
| **U-2c-path** | `t::fs::{Path, PathBuf, Component, Components, SEPARATOR}`. Pure logic, no syscalls. Mirrors std::path. | U-2b |
| **U-2c-io** | `t::io::{Read, Write, Seek, BufRead}` + `t::fs::File`. | U-2c-path |
| **U-2c-fs** | `t::fs::{OpenOptions, Metadata, ReadDir, DirEntry}` + free functions. | U-2c-io |
| **U-2d** | `t::process::{Command, Child, ExitStatus, Stdio}` + `t::process::pipe()`. | U-2c-io |
| **U-2e** | `t::notes::{Notes, Note, NoteClass, NoteMask, NoteTarget, MaskGuard}` + `t::poll::{PollSet, PollEvents, PollTimeout, PollEvent, AsFd}`. **LANDED** at the U-2e row above. | U-2c-io |
| **U-2f** | `t::territory::{mount, bind_replace, bind_before, bind_after, unmount, chroot, pivot_root, MountFlags}` + `t::cap::{Caps, grant, use_grant}`. **LANDED** at the U-2f row above. (`current` / `drop` deferred -- no kernel syscalls at v1.0; `rfork` deferred -- not a standalone syscall at v1.0.) | U-2a |
| **U-2g** | `t::thread` + `t::torpor` + `t::time` + `t::rand` + `t::tty`. | U-2c-io |
| **U-2h** | `t::ninep` (lift 9P client from corvus) + `t::hardware::{Mmio, Irq, Dma}`. Migrates corvus + virtio-* callers in same commit. | U-2c-io, U-2d |
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
