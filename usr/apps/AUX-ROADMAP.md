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
- [ ] `hello` -- the minimal native libthyla-rs app: print a line, exit 0.
  Goal: stand up `usr/apps/Cargo.toml` (workspace) + the target/linker config +
  prove `cargo build` works for the native target. Document the ENTIRE build
  setup as a gap if it is not in the docs (it likely is not -- a P1 finding).

### A1 -- coreutils, io/fs basics (broad doc coverage; warm-up)
- [ ] `echo` `cat` `true` `false` `pwd` `basename` `dirname` `wc` `head` `tail`
  `tee` `cmp`

### A2 -- coreutils, fs mutation + metadata
- [ ] `mkdir` `rmdir` `rm` `cp` `mv` `ln` `touch` `stat` `ls` `readlink`
  `realpath`

### A3 -- coreutils, process / env / text
- [ ] `env` `which` `sleep` `seq` `yes` `date` `uname` `id` `whoami`
  `sort` (simple) `uniq` `cut` `tr` `grep` (simple) `xargs` `hexdump`

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
- (none yet)
