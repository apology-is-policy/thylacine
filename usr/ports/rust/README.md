# track R -- Rust `std` for Thylacine (the in-repo wiring)

Scripture: `docs/RUST-STD-DESIGN.md` (RATIFIED 2026-09-21). Status:
`docs/browser-status.md` track R (owned by main). This directory holds the
**in-repo** half of the port; the forks live outside the repo as siblings (the
`llvm-thylacine` / `mesa-thylacine` / `webkit-thylacine` pattern).

## Layout

- `rust-toolchain.toml` -- the pinned nightly (`nightly-2026-09-20`, rustc
  1.100.0 `bba531001`) + `rust-src`. **Scoped here on purpose** -- never move it
  to the repo root or `usr/`, or it switches the native no_std userspace build
  (stable) to nightly (rustup searches upward).
- `aarch64-unknown-thylacine.json` -- the custom target spec (the ratified
  name). Adapted from `aarch64-unknown-linux-musl` (pouch is musl-derived):
  `os=thylacine`, `env=musl`, static (`dynamic-linking:false`,
  `crt-static-default:true`), `target-family:["unix"]`, `panic-strategy:unwind`
  (libunwind is in the sysroot), `linker=pouch-clang` (gnu-cc, driver-handled
  CRT), tier 3.
- `patches/` -- the `rust-src` std-arms patch series (the `sys/*/unix.rs` +
  `os/thylacine/{mod,raw,fs}.rs` arms + the `library/unwind` link arm + the
  `stack_overflow` non-membership opt-out). NOT YET WRITTEN (R-0 step 3).

## The out-of-tree forks (siblings, not in this repo)

- `../rust-thylacine/` -- a copy of the pinned nightly's `rust-src` + the
  `patches/` series applied, pointed at by `-Z build-std`. NOT YET CREATED.
- `../libc-thylacine/` -- forked `rust-lang/libc` with a new
  `src/unix/thylacine/{mod,b64,align,no_align}.rs` module seeded from
  `src/unix/linux_like/linux/musl/` (+ its `b64/aarch64` arch file), edited for
  pouch's syscall numbers + **synthesized errno values** (not Linux's); plus the
  `src/unix/mod.rs` dispatch arm and the `build.rs` `target_os` registration.
  Pointed at via `[patch.crates-io] libc`. NOT YET CREATED (the larger half).

## Build approach (R-0)

Out-of-tree: pinned nightly + this JSON + `-Z build-std` over the patched
`rust-src` + the forked `libc`. No rustc build.

    cargo +nightly-2026-09-20 build -Z build-std=core,alloc,std \
      --target usr/ports/rust/aarch64-unknown-thylacine.json

This is ALL-CORE -- **hold the Mac + announce to main on yip before running it**
(main runs all-core WebKit builds on the same 8-core machine). Everything up to
build-std (this JSON, the toolchain pin, the libc module, the std arms) is
no-Mac.

## Confirm at R-0 build time (marked unverified until build-std links + runs)

- The JSON link fields: `position-independent-executables` (static-PIE vs
  static-nopic -- the loader rejects DYNAMIC PIE, task #145; static-PIE ships in
  the tree, e.g. the musl static curl), CRT handling by the `pouch-clang`
  driver, and whether `linker=pouch-clang` is reachable/correct from the recipe.
- `env=musl`: honest (pouch is musl-derived) and helps musl-generic crate cfgs;
  confirm no crate mis-routes `target_env="musl"` as Linux.
- `panic=unwind` on device (fall back to `abort` + the abort-backtrace shim only
  if unwind wiring stalls -- the HelenOS precedent).
- pouch `pthread_create`'s default guard under ENOSYS (RUST-STD-DESIGN section 4
  residual): the MAIN thread has a real `prot==0` guard VMA, but a spawned
  pthread's guard is RW; if `pthread_create` fails on the ENOSYS `mprotect`, the
  fix is a pouch default-guardsize-0 change (a yip item for main).

## Pouch dependencies (on main's `browser-b0`, main landing them on main)

- **0033** -- the one-page main-thread stack (`pthread_getattr_np` under the
  ENOSYS `mremap`); Rust std's main-thread path hits it. `@0d3f8ee1`.
- **0034** -- `sysconf(_SC_PHYS_PAGES/_SC_AVPHYS_PAGES)` uninitialised. `@0d3f8ee1`.
- **0035** -- `__stdio_read` refill (fscanf/fgets); an R-2 crate-tail dep (C
  parsers), not R-1. `@e0fc2422`.
- The `getuid`/`geteuid`/`getgid`/`getegid`/`getppid` = `0xFFFFFFDA` ENOSYS
  sentinel is a separate A-3-surface chunk owned by main/the operator (stratumd
  consumes the uid); an R-2 crate-tail dep, not R-1.

Cherry-pick from `browser-b0` only if R-0 blocks before they reach main; do not
re-derive (main's instruction).

## R-0 remaining steps (in order; all no-Mac until build-std)

1. Create `../libc-thylacine/` -- the `src/unix/thylacine` module (the larger
   half). Seed from the musl arm; keep only `library/std`-referenced items.
2. Create `../rust-thylacine/` -- copy the pinned `rust-src`; add the std arms
   (`patches/` series): the `sys/*/unix.rs` cfg arms, `os/thylacine/{mod,raw,fs}`,
   the `library/unwind` arm, and the `stack_overflow` opt-out (non-membership).
3. Wire `[patch.crates-io] libc` + a `tools/` build recipe (mirroring the
   `clade-*` recipes).
4. Hold the Mac + announce main -> `-Z build-std`; iterate the JSON link fields.
   Exit: `std` compiles for `aarch64-unknown-thylacine`.
