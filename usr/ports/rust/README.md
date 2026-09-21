# track R -- Rust `std` for Thylacine (the in-repo wiring)

Scripture: `docs/RUST-STD-DESIGN.md` (RATIFIED 2026-09-21). Status:
`docs/browser-status.md` track R (owned by main). This directory holds the
**in-repo** half of the port; the forks live outside the repo as siblings (the
`llvm-thylacine` / `mesa-thylacine` / `webkit-thylacine` pattern), and
`patches/` carries the validated series that recreates them.

**R-0 EXIT REACHED (2026-09-21): `std` compiles for `aarch64-unknown-thylacine`.**
core+alloc+libc+std all build via `-Z build-std=std`; a std probe crate (HashMap
+ env::args) links against it. Reproducible + validated: see `patches/README.md`.
R-1 next (a cargo-built std hello RUN ON DEVICE: threads/file/TCP/HashMap/
panic-unwind), which needs the pouch runtime patches (`patches/README` + the
"Pouch dependencies" section).

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
- `patches/` -- the two VALIDATED patches (`libc-0.2.189-thylacine.patch` +
  `rust-src-thylacine.patch`) + `patches/README.md` (the apply/reproduce guide).
  These recreate both forks from pristine sources. AS-BUILT, not the design
  note's guesses: errno is STANDARD musl (not synthesized); the rust-src arms
  are `os/thylacine/{mod,raw,fs}` + `os/mod.rs`/`os/unix` + `sys/thread`
  set_name + `sys/random` getrandom + `sys/paths` current_exe + `sys/args` imp +
  the `std/build.rs` restricted_std allowlist (NOT a `library/unwind` arm --
  unwind rides `target_env=musl`; NOT a `stack_overflow` arm -- non-membership
  is zero-code).

## The out-of-tree forks (siblings, not in this repo)

- **rust-src** -- patched IN the sysroot at
  `~/.rustup/toolchains/nightly-2026-09-20-*/lib/rustlib/src/rust` (the pin is
  track-R-exclusive, so mutating it in place is safe; reversible via `rustup
  component remove/add rust-src`). `-Z build-std` reads it from there. There is
  NO separate `../rust-thylacine/` copy -- the durable artifact is
  `patches/rust-src-thylacine.patch`.
- `../libc-thylacine/` -- forked `libc` 0.2.189 with `src/unix/thylacine/mod.rs`
  (the module, transcribed from the musl aarch64 layers), the `src/unix/mod.rs`
  dispatch arm, `build.rs` check-cfg, and the `src/new/` pthread-gate additions.
  Pointed at via `[patch.crates-io] libc` in rust-src's `library/Cargo.toml`.
  Durable artifact: `patches/libc-0.2.189-thylacine.patch`.

## Build approach (R-0)

Out-of-tree: pinned nightly + this JSON + `-Z build-std` over the (possibly
stock) `rust-src` + the forked `libc`. No rustc build.

    cargo +nightly-2026-09-20 build -Z build-std=core,alloc,std \
      -Z json-target-spec \
      --target usr/ports/rust/aarch64-unknown-thylacine.json

`-Z json-target-spec` is REQUIRED on this nightly (1.100.0): a `.json` target
spec is refused without it. The full `std` build is heavier and multi-core --
**hold the Mac + announce to main on yip before running it** (main runs
all-core WebKit builds on the same 8-core machine). But `build-std=core,alloc`
and `build-std=core` + a libc-only probe are LIGHT (low-core, ~13s) and need no
hold; iterate the libc module against those first.

## Confirmed this session (R-0 grounding)

Findings, with confidence labels (measured = ran it; sourced = read the pouch
seam / pinned rust-src):

- **[measured]** The target JSON is valid: `core` + `alloc` compile cleanly for
  `aarch64-unknown-thylacine` (~13s, low-core). arch / data-layout / features /
  max-atomic-width all accepted by rustc+LLVM.
- **[measured]** std pins `libc v0.2.189` (rust-src `library/Cargo.lock`); the
  fork base is that exact version.
- **[sourced]** errno values are STANDARD musl, NOT synthesized. Pouch's
  `src/internal/syscall_ret.c` (patch 0001) passes `-errno` in `[-4095,-2]`
  through unchanged and maps flat -1 to EIO; no patch touches `bits/errno.h`;
  and the tracked `getuid` bug returns `0xFFFFFFDA` = `(u32)(-38)` = `-ENOSYS`
  with 38 the standard musl/Linux ENOSYS. => the libc thylacine module uses
  standard musl aarch64 errno constants. (Corrects RUST-STD-DESIGN's
  "synthesized errno" premise -- design-note fix owed.)
- **[sourced]** Syscall NUMBERS: pouch (patch 0001) maps only 8 `__NR_` 1:1
  (exit=0, exit_group=60, read=9, write=10, close=11, mlockall=16, getrandom=20,
  set_tid_address=36); every other becomes `0xFFFF` -> `-ENOSYS`. std as a
  NON-linux generic unix calls libc FUNCTIONS (provided by patches 0003-0032),
  not raw `syscall(SYS_*)`, so the ENOSYS'd numbers don't reach std.
- **[sourced]** rust-src std arms the design note flagged all ride free or are
  not needed for COMPILE:
  - `os/thylacine` NOT needed to compile -- `os/mod.rs` gates `os::<name>` on
    `cfg(target_os="<name>")`; `os::unix` covers thylacine (target_family=unix).
    `std::os::thylacine` (design 6) is nice-to-have, a later add.
  - stack-guard opt-out needs ZERO code: `sys/pal/unix/stack_overflow.rs`
    non-membership in the allowlist routes to the no-op stub `imp`.
  - `library/unwind` rides free: the `cfg(target_env="musl")` link arm +
    the `any(unix, ...)` module arm cover thylacine (static-bundle libunwind
    under crt-static).
  => R-0 will try building `std` against STOCK rust-src first; fork/patch
  rust-src (../rust-thylacine + patches/) ONLY for arms the build actually
  demands. May be zero.
- **[measured]** libc `new/` module gap: `new/musl/` activates on
  `target_env="musl"` but its `common::linux_like`/`common::posix::pthread`
  deps were gated to specific `target_os` lists assuming every musl target is
  Linux. Fixed by adding `target_os="thylacine"` to 4 gates (new/common/mod.rs,
  new/common/linux_like/mod.rs, new/common/posix/mod.rs, new/mod.rs). Correct:
  thylacine IS a posix+musl+linux_like-shaped platform.

## Still to confirm at R-1 (NOT exercised by R-0's rlib compile)

R-0 built rlibs (core/alloc/libc/std) -- it did NOT link a final binary, so
`pouch-clang` was never invoked and the LINK fields below are still unproven.
They get exercised at R-1 when a std hello is linked + run on device.

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

## Pouch dependencies -- CONSUME FROM `browser-b0`, not `main` (2026-09-21)

These are RUNTIME pouch/musl patches. **R-0 (std COMPILES) needs NONE of them** --
this fork is a Rust `libc` CRATE (declarations), independent of the pouch musl
`.a`. They matter at **R-1** (std hello ON DEVICE).

Sourcing changed: main's ci went RED on a mount-table regression (PGRP_MAX_MOUNTS
full; operator ruled the real fix is scripture-first, main's) -- so **nothing of
main's reaches `main` soon, including these patches**. For R-1, cherry-pick from
`browser-b0` (do NOT re-derive; main's instruction):

- **0033** -- one-page main-thread stack (`pthread_getattr_np`/ENOSYS `mremap`);
  std's main-thread path. **`@e0fc2422`** (browser-b0).
- **0034** -- `sysconf(_SC_PHYS_PAGES/_SC_AVPHYS_PAGES)` uninitialised. `@e0fc2422`.
- **0035** -- `__stdio_read` refill (fscanf/fgets); R-2 crate-tail (C parsers). `@e0fc2422`.
- **0036** -- `tmpfile()` never unlinked; crate-tail concern. `@37aa3fd5`.
- **0037** -- unchecked sentinel wrappers (sysconf RLIM arm, getloadavg,
  getdtablesize, ulimit, getdomainname). `@def378cd` -- **NEVER COMPILED; do NOT
  take blind** (main's warning).
- `getuid`/`geteuid`/`getgid`/`getegid`/`getppid` = `0xFFFFFFDA` (-38) ENOSYS
  sentinel: a separate A-3-surface chunk (stratumd consumes the uid), main's,
  landing right after B-0. R-2 crate-tail, not R-1.

## R-0: DONE (2026-09-21)

1. DONE -- `../libc-thylacine/` with `src/unix/thylacine/mod.rs` (compiler-bounded
   to std's surface, ~117 base syms + 71 errno/signal/wait extras the grep
   snapshot missed). Captured: `patches/libc-0.2.189-thylacine.patch`.
2. DONE -- rust-src std arms patched IN the sysroot (no separate copy). Captured +
   validated: `patches/rust-src-thylacine.patch` (applied to pristine -> rebuilt
   green).
3. DONE -- `[patch.crates-io] libc` is in rust-src's `library/Cargo.toml` (build-std
   does not honour the user crate's patch). A `tools/` recipe is not yet written;
   the build command is in `patches/README.md`.
4. DONE -- `std` compiles for `aarch64-unknown-thylacine` (the R-0 exit). No Mac
   hold needed after all (build-std at -j2 is modest, ~13s clean).

## R-1 next

A cargo-built std hello RUN ON DEVICE (threads / file read / TCP connect /
HashMap / panic that unwinds). Needs: the pouch runtime patches (cherry-pick
0033/0034/0035 from browser-b0 @e0fc2422 -- see "Pouch dependencies"); the
final-link path (pouch-clang + the JSON LINK fields -- see "Still to confirm at
R-1"); staging the binary into the image; an on-device witness.
