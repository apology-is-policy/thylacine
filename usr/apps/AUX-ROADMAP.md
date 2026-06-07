# Thylacine Auxiliary Roadmap (native userspace apps)

**This is the auxiliary agent's memory + worklist.** Read `AUX-DIRECTIVE.md`
first for the rules. Work top-down; mark each item `[ ]` -> `[wip]` -> `[done]`
and keep this file current on every commit. Status legend:
`[ ]` not started, `[wip]` in progress, `[done]` compiles + test plan written +
gaps logged, `[blocked]` needs an unbuilt dependency (Phase B/C).

Each Phase-A item's "done" = (a) `usr/apps/<name>/` compiles via `cargo build`
for the native target, (b) `usr/apps/<name>/TEST-PLAN.md` exists, (c) any doc
gaps are in `DOC-GAP-REPORT.md`. NEVER run anything.

---

## Phase A -- native, build-to-compile, fully parallel-safe (DO THIS NOW)

### A0 -- bootstrap (do FIRST; de-risks the toolchain + the first gap batch)
- [done] `hello` -- the minimal native libthyla-rs app: print a line, exit 0.
  Landed `usr/apps/Cargo.toml` (separate workspace) + `usr/apps/hello/` +
  `usr/apps/scripts -> ../scripts` symlink (linker-script reachability).
  `cargo build --release` clean; ELF static aarch64, `_start`@0x400000,
  `rs_main`@0x400018, W^X-clean (one R+E PT_LOAD; no RW needed). Build setup
  turned out to be DOCUMENTED (docs/reference/38-userspace.md) -- contrary
  to the roadmap's P1 expectation; recorded as discoverability/staleness
  gaps instead (G01 P3, G04 P2) + the sub-workspace path trap (G02 P2).
  **BIG FINDING: G03 [P1] -- native apps cannot read their own argv** (see
  Notes below). TEST-PLAN written. Toolchain: cargo/rustc 1.94.1.

### A1 -- coreutils, io/fs basics (broad doc coverage; warm-up)
- [wip] `echo` `cat` `true` `false` `pwd` `basename` `dirname` `wc` `head` `tail`
  `tee` `cmp`
  - **Foundation: `usr/apps/aux-rt`** (shared crate) -- hand-rolls the argv
    accessor (G03 workaround: a naked `rs_main` SP-capture, BYTE-VERIFIED in
    echo's disassembly) + fd-0/1/2 stdio (G05 workaround over t_read/t_write)
    + `aux_rt::main!` entry macro + print!/println!/eprintln!. This unblocks
    the entire argv-taking arc. G03 stays P1 (DOC/API gap) but is RESOLVED
    for authoring.
  - [done] `echo` -- argv chain disassembly-verified; gaps G05/G06.
  - [done] `true` `false` -- trivial (exit 0 / 1).
  - [done] `pwd` -- degenerate "/" (no cwd at v1.0; gap G07).
  - [done] `basename` `dirname` -- exercise Path::file_name/parent; gap G08
    (POSIX-divergence recovered in-app).
  - [done] `cat` -- File::open + io::Read + aux-rt copy; stdin/"-"; absolute
    paths only.
  - [done] `wc` `head` `tail` `cmp` -- File read + counting/line logic;
    head streams (early-stop), tail/cmp/wc slurp. All clippy-clean, W^X-clean.
  - [done] `tee` -- uses the new aux-rt::fs shim; -a degrades to truncate
    with a warning (append unsupported, G09).
  - New gaps this batch: G07 (no cwd, P2), G08 (Path/POSIX divergence, P3),
    G09 (fs has no create/append/readdir though the kernel does, P2 API-GAP),
    G10 (t_readdir type-byte encoding unspecified, P3).
  - **A1 COMPLETE (12/12 apps).**
  - **Foundation extended: `aux-rt::fs`** -- OwnedFd (io::Read/Write/Seek +
    Drop-close over a raw fd; the File::from_raw_fd libthyla-rs withholds) +
    open/create/mkdir/remove_file/remove_dir/rename/read_dir over the raw
    t_open/t_walk_create/t_unlink/t_rename/t_readdir wrappers. This unblocks
    ALL of A2.

### A2 -- coreutils, fs mutation + metadata
- [wip] 9/11 built on aux-rt::fs; 2 blocked (no kernel surface).
  - [done] `mkdir` (-p), `rmdir`, `rm` (-rRf, recursive), `cp` (-r,
    multi-src-into-dir), `mv` (rename + cross-session copy+unlink fallback),
    `touch` (-c; create-only, mtime bump unsupported G12), `stat`,
    `ls` (-la1; long format), `realpath` (lexical-only, G11/G07).
  - [blocked] `ln`, `readlink` -- NO kernel link/symlink/readlink surface
    (G11). Not built (nothing to hand-roll against; unlike G09 there is no
    raw wrapper). Revisit if a t_symlink/t_link/t_readlink lands.
  - New gaps: G11 (no link/symlink/readlink surface, P2 API-GAP),
    G12 (no atime/mtime setter, P2).
  - All built apps compile + clippy clean (0 warnings), W^X-clean.

### A3 -- coreutils, process / env / text
- [wip] batch 1 (8) done; batch 2 (text tools) + blocked ones remain.
  - [done] `sleep` (time::sleep, s/m/h/d, summed), `uname` (static G16),
    `env` (degenerate G15), `which` (degenerate G15/G07), `seq`,
    `yes` (broken-pipe safe), `hexdump` (canonical), `uniq` (-c, adjacent).
  - [ ] batch 2: `sort` `cut` `tr` `grep` (simple) `xargs` (xargs exercises
    process::Command spawn -- good gap-finder). Pure text/compute; low gap
    yield except xargs.
  - [blocked] `date` -- no wall/monotonic clock (G13). `id`/`whoami` -- no
    self-identity/getpid accessor (G14). Not built.
  - New gaps: G13 (no clock, P2), G14 (no self-identity/getpid, P2 API-GAP),
    G15 (no env vars, P2 API-GAP), G16 (no uname/sysinfo, P3).
  - All built compile + clippy clean (0 warnings).

### A4 -- Thylacine-DISTINCTIVE native tools (the BEST doc-gap finders)
These exercise the surfaces a generic coreutil author would not know how to use
without good docs -- the per-Proc namespace, capabilities, and the synthetic
introspection trees. Expect the richest gap findings here.
- [ ] `ns` -- print the calling Proc's namespace (mounts/binds; Plan 9 `ns`)
- [ ] `bind` / `mount` / `unmount` -- namespace composition (Plan 9 / SYS_MOUNT)
- [ ] `srv` -- list the per-territory `/srv` services
- [ ] `9p` -- walk / read / write a 9P tree (Plan 9 `9p`; the libthyla-rs 9P client)
- [ ] `ps` / `kill` / a `/proc` browser -- the process surface (+ CAP_KILL)
- [ ] `cap` -- inspect / grant capabilities via the `cap` device (clearance/legate)
- [ ] `sysinfo` (a.k.a. `doctor`) -- read `/proc` + the kernel `/ctl` admin surface

### A5 -- nora: the native modal editor (FLAGSHIP arc)
A ratatui-based, Helix-modal editor, made native-forever. Seed inputs (NOT
"docs" -- existing code you may port from): the editor component at
`/Users/northkillpd/projects/stratum/tui/src/editor.rs` and upstream `ratatui`.
The docs-fidelity test applies to the THYLACINE integration (the console/PTY
backend, the ctl-tree editing, the namespace) -- that is where the gaps will be.
- [ ] A5.0 -- fork + vendor `ratatui` at a LOCKED version into
  `usr/apps/vendor/ratatui-native/`; strip `std`/`crossterm`; make it
  `no_std + alloc`; write a Thylacine-native backend (raw console / PTY draw +
  input) over `libthyla-rs`. (Heavy doc-gap surface: is raw terminal I/O
  documented? the console device? the PTY? -- log it all.)
- [ ] A5.1 -- a tiny TUI smoke (a clock, or 2048/tetris) on the native backend,
  to prove it COMPILES + the backend shape is right before nora depends on it.
- [ ] A5.2 -- the editor engine (port `editor.rs`): buffer/rope, viewport,
  cursor, undo.
- [ ] A5.3 -- modal (Helix subset): normal / insert / select; the keymap.
- [ ] A5.4 -- the Thylacine-native features: edit ctl-type / grafted (non-fs)
  trees (read/write synthetic nodes), fuzzy file find (Helix SPACE+f), multiple
  buffers, persistent scratch buffers kept across sessions (Notepad++ style),
  and more (extend as the design firms up).

---

## Phase B -- native but BLOCKED on unbuilt deps (DESIGN + SKELETON + TEST PLAN now; build later)

The deliverable here is NOT a compiling app -- it is a design doc + an
API-shaped skeleton (written against the documented FUTURE Loom/net surface) + a
test plan + a "what it needs to be unblocked" list. No execution.
- [blocked] B1 -- `httpd`: an HTTP/1.1 server using **Loom** (blocked on the
  Loom-6 native Loom API + the network stack). Showcases the Loom trap-
  amortization win.
- [blocked] B2 -- network clients/servers (echo server, a tiny HTTP/DNS client,
  a chat server) -- blocked on the `/net/` filesystem (not built).
- [blocked] B3 -- a Loom-backed bulk-copy / high-fanout demo (the latency
  benchmark Loom-6 wants) -- blocked on Loom-6.

---

## Phase C -- PORTS (a DIFFERENT discipline: pouch/musl, execution-needed; SCOPE/FEASIBILITY ONLY)

Ports are NOT native libthyla-rs and NOT docs-only-buildable; several need
EXECUTION to verify (a compiler, a graphics lib). So the aux deliverable is a
**feasibility + gap-analysis doc**, not a build. The actual port is a later
main-track or dedicated effort. (These gap analyses are themselves high-value
roadmap input.)
- [ ] C1 -- **Zig toolchain** feasibility: the libc / syscall surface a native
  Zig compiler needs, mapped against what pouch + the kernel provide today; the
  gap list; a porting plan; a definition of the "self-hosting on Thylacine"
  milestone. (The dream: author native programs in **nora**, compile them with a
  native **Zig** -- record as a roadmap aspiration + a concrete dependency map.)
- [ ] C2 -- **SDL** feasibility: the graphics (virtio-gpu) / input (virtio-input)
  / audio surface SDL needs vs what exists; the gap list; a porting plan. (Needs
  execution to verify -> scope only.)

---

## Notes / open questions for the user (the aux agent appends here)
- **[P1, roadmap-affecting] Native apps cannot read their own argv/argc**
  (DOC-GAP G03). libthyla-rs's `_start` does `bl rs_main` with no SP capture
  and no callee-side argv accessor exists, though the kernel already
  populates a SysV/auxv frame on the stack (P6). This blocks the
  argument-taking half of A1-A3 (`echo`, `cat FILE`, `cp`, `mv`, `seq`,
  `grep`, ...) as authored-from-docs. A1's FIRST task is to test a
  hand-rolled in-app `#[unsafe(naked)]` SP-capture workaround; if it links
  cleanly it unblocks the arc (and is a great documented workaround), if not
  those apps ship without argv (stdin-only forms) + the limitation is logged
  per app. The clean fix is a libthyla-rs `args()` accessor or an
  `rs_main(argc, argv)` entry -- main-agent territory, recorded as API-GAP.
  No action needed from you unless you want to redirect; proceeding under
  autonomy.
  - **UPDATE (A1):** RESOLVED with an in-app workaround -- `usr/apps/aux-rt`'s
    naked `rs_main` reads the Shape-B frame; byte-verified in echo's
    disassembly. The arc is unblocked. The clean upstream fix (a libthyla-rs
    `args()` accessor) is still owed and tracked as G03 (API-GAP) for the
    main agent.
- **[P1] No terminal-backed fd 0/1/2 for native apps at v1.0** (DOC-GAP G06).
  Native stdout/stdin only exist when wired (pipeline/redirect/inherit); the
  shell prints via the UART (t_putstr). Coreutils are authored POSIX-shaped
  (read fd0 / write fd1) so they compose in pipelines; standalone visibility
  needs the console/PTY surface that does not exist yet. This is THE question
  the A5 nora editor will hit head-on (it needs raw terminal I/O). Flagging
  early; no redirect needed.
