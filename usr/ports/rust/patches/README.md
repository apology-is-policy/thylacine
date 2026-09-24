# track R -- the R-0/R-1 patch series (re-validated apply-to-pristine 2026-09-21)

Two patches recreate the two out-of-tree forks from pristine sources. BOTH were
re-validated apply-to-pristine on 2026-09-21 (after R-1 closed): each applies to
a truly-pristine tree with `-p1` (dry-run clean, 0 fuzz) and reproduces the fork
byte-for-byte -- rust-src 13/13 files identical + a forced rebuild produced an
`r1hello` byte-IDENTICAL to the R-1 witness binary (sha256 1c300f19...); libc
9/9 files identical to `../libc-thylacine`. See the reproduce recipe below.

R-0 EXIT REACHED: `std` compiles for `aarch64-unknown-thylacine`.

## The two patches

- **`libc-0.2.189-thylacine.patch`** (9 files) -- apply to a pristine `libc`
  crate at version **0.2.189**. Adds `src/unix/thylacine/mod.rs` (the module --
  the larger half, transcribed from the musl aarch64 layers), the
  `src/unix/mod.rs` family-dispatch arm, the `build.rs` check-cfg entry, and the
  4 `src/new/` tree gate additions (thylacine joins the posix/linux_like pthread
  cfg gates -- an upstream gap: `new/musl/` assumed every musl target is Linux).
- **`rust-src-thylacine.patch`** (13 files) -- apply with `-p1` from the rust-src
  ROOT (`.../lib/rustlib/src/rust`). Adds:
  - `library/Cargo.toml` -- `[patch.crates-io] libc = { path = ... }` (build-std
    does NOT honour a patch in the user crate; std's libc must be patched here).
  - `library/std/build.rs` -- `thylacine` in the full-std allowlist (else std is
    marked `restricted_std` and every program needs `#![feature(restricted_std)]`).
  - `library/std/src/os/thylacine/{mod,raw,fs}.rs` -- NEW `std::os::thylacine`
    (raw type aliases + `MetadataExt`, flat `st_atime`/`st_atime_nsec` fields).
  - `library/std/src/os/mod.rs` + `os/unix/mod.rs` -- the `pub mod thylacine`
    arm + the `os/unix::platform` mapping.
  - `library/std/src/sys/thread/mod.rs` -- route `set_name` to `unsupported`
    (no-op thread naming; fine for R-0/R-1).
  - `library/std/src/sys/random/mod.rs` -- the `getrandom` arm (pouch maps
    getrandom -> Thylacine syscall 20).
  - `library/std/src/sys/paths/unix.rs` -- a `current_exe` arm returning
    Unsupported (thylacine has no /proc/self/exe yet).
  - `library/std/src/sys/args/unix.rs` -- `thylacine` in the argc/argv `imp`
    list (pouch passes argc/argv to `_start`).

## Recreate the forks (fresh machine)

    # 1. libc fork
    cp -R ~/.cargo/registry/src/*/libc-0.2.189 ../libc-thylacine
    cd ../libc-thylacine && patch -p1 < <repo>/usr/ports/rust/patches/libc-0.2.189-thylacine.patch

    # 2. rust-src (patched IN the sysroot; the pin is track-R-exclusive)
    rustup component add rust-src --toolchain nightly-2026-09-20
    cd "$(rustc +nightly-2026-09-20 --print sysroot)/lib/rustlib/src/rust"
    patch -p1 < <repo>/usr/ports/rust/patches/rust-src-thylacine.patch

    # 3. point library/Cargo.toml's [patch] libc path at ../libc-thylacine
    #    (the patch uses an absolute path from this session; edit to match).

## Build

    cd <a std crate>; cargo +nightly-2026-09-20 build \
      -Z build-std=std -Z json-target-spec \
      --target <repo>/usr/ports/rust/aarch64-unknown-thylacine.json

## GOTCHA: build-std fingerprint staleness (cost hours; do not forget)

`-Z build-std` does NOT reliably rebuild a patched std-dep (libc) or re-run
std's `build.rs` when their SOURCE changes -- it silently reuses a stale rlib
and reports false results. After editing the libc fork OR any rust-src file,
either `rm -rf target/<triple>` (a full clean is definitive) or, for a libc-only
edit, remove `target/<triple>/debug/{build/libc,incremental/libc-*,deps/liblibc-*}`.
ALWAYS confirm the build log says `Compiling libc v0.2.189 (.../libc-thylacine)`
and/or `Compiling std v0.0.0`.

## Regenerating these patches

    # libc: diff a pristine copy against the fork (a/ b/ prefixes -> -p1)
    diff -ruN --exclude=.cargo-checksum.json a-libc(pristine) b-libc(fork)
    # rust-src: diff pristine (rustup remove+add restores it; your os/thylacine
    # SURVIVES remove/add since rustup only manages manifest files) against your
    # edited copy, a/ b/ prefixes.
