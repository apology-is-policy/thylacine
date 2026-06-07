# Documentation-Gap Report (native userspace authoring)

The auxiliary agent's top deliverable: a documentation-completeness audit of
`docs/reference/*` + the `libthyla-rs` public API, produced by writing native
programs against them using ONLY the documentation. Each gap is a place the docs
were missing, ambiguous, or wrong -- the main agent folds verified gaps into the
real docs later.

Append one entry per gap. Keep them specific (cite the file + section). Severity:
**P1** blocks authoring (no way to do the task from docs), **P2** costly
(had to read the API / guess), **P3** polish (unclear / missing example).

## Format

```
### G<NN> [P<sev>] <one-line title>
- App / task: <which app, what you were trying to do>
- Doc consulted: <docs/reference/NN-*.md section, or "USER-MANUAL stub", or
  "libthyla-rs API only">
- Gap: <missing / ambiguous / wrong -- what exactly was not derivable from docs>
- Workaround: <what you did instead -- read the pub signature, cribbed a sibling
  app, hand-rolled, or skipped>
- Suggested doc fix: <1 line: where + what to add>
```

## Findings

### G01 [P3] Native-app build setup IS documented, but is not discoverable
- App / task: A0 `hello` -- "how do I build a native libthyla-rs app".
- Doc consulted: searched all of `docs/reference/` (108 files); found
  `docs/reference/38-userspace.md`.
- Gap: contrary to the roadmap's expectation that the build setup is
  undocumented, doc 38 documents it WELL -- the `aarch64-unknown-none`
  target, the W^X linker script, `usr/.cargo/config.toml`, the `_start` ->
  `rs_main` entry contract, the `#![no_std] #![no_main]` + global-allocator
  opt-in, and the `libthyla-rs = { path = ... }` dependency shape. The gap
  is DISCOVERABILITY: doc 38 is titled "Userspace tree + libt runtime
  (P4-Ia1)" and tagged to an early phase, so an author surveying for "how
  to build a native Rust program" may not land on it. `docs/REFERENCE.md`
  has no "building a native app" pointer.
- Workaround: read doc 38 in full after a brute-force inventory; cribbed
  the manifest shape from `usr/hello-rs/Cargo.toml`.
- Suggested doc fix: add a one-line pointer in `docs/REFERENCE.md` (and the
  manual) -> "Authoring a native userspace program: see 38-userspace.md",
  and consider a short modern "Native app quickstart" since doc 38's body
  is phase-tagged history.

### G02 [P2] Sub-workspace linker-script path resolution is undocumented
- App / task: A0 -- standing up the separate `usr/apps/` cargo workspace.
- Doc consulted: `docs/reference/38-userspace.md` caveat #6.
- Gap: caveat #6 documents that `-Tscripts/aarch64-userspace.ld` resolves
  relative to cargo's workspace-root CWD, which for the main build is
  `usr/` (because `tools/build.sh` does `cd usr && cargo build`). It does
  NOT address a workspace whose root is NOT `usr/`. My workspace root is
  `usr/apps/`, so the linker looked for `usr/apps/scripts/...` and the link
  failed until the path was made reachable.
- Workaround: symlink `usr/apps/scripts -> ../scripts` so
  `scripts/aarch64-userspace.ld` resolves from the `usr/apps/` root. (The
  alternative -- a child `usr/apps/.cargo/config.toml` -- is a trap: cargo
  JOINS `target.<triple>.rustflags` arrays across the parent+child configs,
  so redefining rustflags duplicates the `-T` flag. Inheriting the parent
  config + a symlink is the clean path.)
- Suggested doc fix: extend caveat #6 -> "any cargo workspace not rooted at
  `usr/` must make `scripts/aarch64-userspace.ld` reachable from its own
  root (symlink, or replicate the linker flags with an adjusted path);
  do NOT add a child `.cargo/config.toml` that redefines rustflags, as
  cargo array-joins them and duplicates `-T`."

### G03 [P1][API-GAP] No way for a native app to read its own argv/argc
- App / task: discovered at A0 while reading the entry contract; BLOCKS the
  entire argv-taking coreutils arc (A1 `echo`, A1 `cat FILE`, A2 `cp`/`mv`,
  A3 `seq`/`grep`/..., anything that takes arguments).
- Doc consulted: `docs/reference/38-userspace.md` (entry contract);
  `usr/lib/libthyla-rs/src/lib.rs` `_start` global_asm + the full crate
  public API.
- Gap: a native program's entry is `rs_main() -> i64` with NO parameters.
  libthyla-rs's `_start` is `bti c; bl rs_main; ...` (lib.rs:1764-1768) --
  it does NOT capture the stack pointer nor pass argc/argv to `rs_main`.
  The kernel DOES populate a System-V startup frame on the stack (sp ->
  argc word at EXEC_USER_STACK_TOP - EXEC_INIT_STACK_SIZE; "P6-pouch-kernel-
  auxv", per the lib.rs:1741-1743 comment), but libthyla-rs exposes NO
  accessor to it, and by the time `rs_main`'s compiler-emitted prologue has
  run, the original sp (which pointed at argc) is gone. The SPAWN side
  exists (`process::Command::arg()` builds an argv buffer for a CHILD), but
  the CALLEE side (reading one's OWN argv) has no API.
- Workaround: TBD -- to be investigated at A1. Candidate: a hand-rolled
  `#[unsafe(naked)]` entry shim INSIDE the app that captures sp before any
  prologue, then parses the frame. Feasibility is uncertain because
  libthyla-rs already defines `.globl _start` in one codegen unit, so an
  app-side `_start` would collide at link (the app references other
  libthyla-rs symbols, pulling the `_start`-bearing object). If no in-app
  workaround holds, argv-taking apps are SKIPPED or built without argv and
  the limitation is documented per app. (Severity stays P1: there is no
  way to do this from docs + the public API as they stand.)
- Suggested doc fix: this is primarily an API gap to surface to the main
  agent -- libthyla-rs should expose an `args()` accessor (the kernel frame
  already exists), OR `_start` should capture sp and pass (argc, argv) to a
  `rs_main(argc, argv)` form. Until then, doc 38 must stop implying argv is
  a "Phase 5+ future" (see G04) and state plainly that native apps cannot
  read argv today.

### G04 [P2] doc 38 entry contract is stale w.r.t. the P6 kernel auxv frame
- App / task: A0 -- understanding what `rs_main` receives.
- Doc consulted: `docs/reference/38-userspace.md` lines 189-193 + 117-120.
- Gap: doc 38 says "`main()` takes no arguments (no argc/argv/envp); when
  the exec syscall surface lands in Phase 5+ the kernel will populate the
  auxv on the user stack ... and `_start` will become an actual argc/argv
  loader." But per `lib.rs:1741-1743`, the kernel ALREADY populates that
  frame ("P6-pouch-kernel-auxv" landed). So the doc's future tense is now
  half-true and misleading: the frame EXISTS, but `_start` still does not
  load it. An author following doc 38 is left believing argv is simply a
  not-yet-arrived future, rather than a present frame with a missing
  consumer.
- Workaround: cross-referenced the lib.rs `_start` comment to learn the
  frame already exists; see G03.
- Suggested doc fix: update doc 38 lines 189-193 to the as-built state:
  the kernel populates the SysV/auxv frame (P6), but libthyla-rs's `_start`
  does not consume it and no callee-side argv accessor is exposed yet.
