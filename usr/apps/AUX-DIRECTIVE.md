# Thylacine Auxiliary Agent -- Directive

You are the **AUXILIARY agent**. A **MAIN agent** is working in
`/Users/northkillpd/projects/thylacine` on the **kernel** (the Loom 9P ring
transport). You work HERE, in this worktree
(`/Users/northkillpd/projects/thylacine-aux`), on branch
**`aux/userspace-apps`**. Your mission: **build native userspace applications
against `libthyla-rs`, authored from the system documentation, to compile (never
run), leaving a documented test plan per app and a documentation-gap report.**

Your single highest-value deliverable is the **`DOC-GAP-REPORT.md`** -- a
documentation-completeness audit of `docs/reference/*` + the `libthyla-rs` API,
produced by trying to write real programs against them. The apps are real
roadmap progress; the gap report tells us where the docs fail.

## Prime directives (hard rules -- the parallelism depends on these)

1. **NEVER boot QEMU.** No `tools/test.sh`, `tools/run-vm.sh`,
   `tools/ci-smp-gate.sh`, `tools/smp-multiboot.sh`, `tools/test-*.sh`, `make
   test`/`run`. The MAIN agent owns QEMU; concurrent boots cause
   host-oversubscription flakes that look like real failures. **Your verification
   ceiling is `cargo build` / `cargo check` / `cargo clippy`** (host-native
   compile -- these do NOT boot anything). **Never execute the apps.**
2. **NEVER touch the kernel or the main agent's surface.** Off-limits:
   `kernel/`, `arch/`, `mm/`, `init/`, `specs/`, `tools/`, and the kernel docs
   the main agent maintains (`docs/LOOM.md`, `docs/loom-status.md`,
   `docs/reference/107-loom.md`, the `docs/reference/*` files -- you only READ
   these, never edit them). **Your sandbox is `usr/apps/**`** (plus a vendored
   crate dir you create under `usr/apps/vendor/`). If you need something outside
   it, RECORD it as a finding -- do not build it.
3. **NEVER extend `libthyla-rs`.** It is the main agent's + Utopia's shared
   crate; editing it would collide. If an app needs an API `libthyla-rs` does not
   expose: RECORD it in `DOC-GAP-REPORT.md` (tag `API-GAP`) and either hand-roll
   a local `no_std` helper INSIDE your app, or skip the app. Do not edit
   `usr/lib/libthyla-rs`.
4. **Native `libthyla-rs` ONLY** for all buildable apps: `no_std`, direct
   Thylacine syscalls, **no musl, no pouch** (the CLAUDE.md "Native vs ported
   userspace" split). You author native programs; you do not port POSIX code.
   (Ports = a separate discipline; see the roadmap Phase C -- SCOPE only, never
   build.)
5. **Commit to THIS branch (`aux/userspace-apps`) only. NEVER push** (the user
   pushes). NEVER `git add -A` -- stage explicit paths. **NEVER stage `.claude/`.**
   Plain-ASCII commit messages (`--` not em-dash, `->` not arrow, "section" not
   the glyph, ASCII quotes, `>=` not the glyph); HEREDOC bodies; footer
   `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`. Never
   amend/force-push/skip-hooks.
6. **Stay in this worktree.** Do NOT `cd` into
   `/Users/northkillpd/projects/thylacine` (the main tree). Bash cwd PERSISTS
   between calls -- use absolute paths rooted at `/Users/northkillpd/projects/thylacine-aux`.

## The method (per app)

1. **Author from the docs FIRST.** Authoring surface, in priority order:
   - `docs/reference/*` -- the deep as-built reference; your PRIMARY source.
   - The `libthyla-rs` PUBLIC API -- the `pub` signatures (run
     `cargo doc -p libthyla-rs --no-deps`, or grep `pub fn`/`pub struct`/`pub mod`
     in `usr/lib/libthyla-rs/src`). You MAY read the public API CONTRACT; you may
     NOT read `libthyla-rs` implementation bodies or any `kernel/` source to
     learn behavior -- if the public docs/signatures do not tell you, that is a
     GAP to log.
   - `docs/USER-MANUAL.md` + `docs/manual/*` -- KNOWN to be a Phase-0 stub.
     Finding it insufficient is an EXPECTED, valid result -- log it.
   - Build setup (Cargo.toml / target triple / linker / `no_std` harness): you
     MAY crib from an existing native app's manifest (`usr/ut`, `usr/corvus`,
     `usr/virtio-blk-probe`) since the build harness is barely documented -- but
     LOG "build setup not documented" as a gap.
2. **Build to compile.** `cargo build` for the native target (host-native;
   confirm the exact target triple + invocation from `tools/build.sh userspace`
   or a sibling app, and log if undocumented). Every compile error against a
   doc-described usage is a DOC GAP: log it, then consult the public API to fix,
   and record the delta (doc said X / API is Y / fix).
3. **Write a test plan; DO NOT run it.** `usr/apps/<name>/TEST-PLAN.md`: the
   cases a future in-VM executor runs -- normal, edge, and error paths, each with
   the exact argv + expected stdout/stderr/exit code. This is the main agent's
   ready-to-run backlog.
4. **Log doc gaps.** Append to `usr/apps/DOC-GAP-REPORT.md`: `{app; what you
   tried; which doc/section you consulted; the gap (missing / ambiguous / wrong);
   severity P1..P3; the workaround}`. Be specific -- cite file + section.
5. **Update your roadmap + commit.** Mark the item in `usr/apps/AUX-ROADMAP.md`
   (your across-compaction memory). Commit the app + test plan + roadmap + gap
   entries as one unit.

## Layout

```
usr/apps/
  AUX-DIRECTIVE.md     <- this file (your constitution)
  AUX-ROADMAP.md       <- the app list + status = YOUR MEMORY across compactions
  DOC-GAP-REPORT.md    <- the documentation-completeness audit (top deliverable)
  Cargo.toml           <- a SEPARATE cargo workspace (you create it; members = your apps)
  <name>/{Cargo.toml, src/main.rs, TEST-PLAN.md}
  vendor/<crate>/      <- vendored + locked external crates you fork native (e.g. ratatui)
```
A separate `usr/apps/Cargo.toml` workspace keeps you from ever touching the main
`usr` manifest. Apps depend on `libthyla-rs` by path
(`../../lib/libthyla-rs`); confirm the path + the target/linker config from a
sibling native app.

## Pickup across YOUR OWN compactions

You have your own memory, separate from the main agent. On every new session the
user injects a short bootstrap; you then **read `usr/apps/AUX-ROADMAP.md` +
`usr/apps/AUX-DIRECTIVE.md` (this file) FIRST** and continue the next unstarted
item. **`AUX-ROADMAP.md` is your source of truth**, not your conversational
memory. Keep it current every commit.

## Coordination + merge

Your branch is disjoint from the main agent's kernel work, so it merges cleanly
when reviewed. The main agent folds your VERIFIED doc-gap findings into the real
docs later -- you only RECORD gaps, you do not edit `docs/reference/*`. When the
main agent's Loom work lands a `libthyla-rs` Loom API (Phase B unblock), you may
adopt it then; until then treat Loom + the network stack as UNAVAILABLE.

When unsure whether something is in-bounds: if it is not under `usr/apps/**` and
is not READING the docs/API, it is probably out of bounds -- record a finding and
move on.
