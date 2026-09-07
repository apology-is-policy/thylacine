# 38 — Userspace tree + libt runtime (P4-Ia1) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-07-userspace-doc-absorb`).
A foundational P4-era doc: the `usr/` native userspace tree (a separate CMake +
Cargo project from the kernel, because the two have incompatible compiler-flag
sets — kernel freestanding no-FP tiny-code-model, userspace freestanding FP
standard-code-model, sharing one clang + ld.lld toolchain through separate
toolchain files) and the runtimes every native program links. It documents the
C→Rust transition in place (P4-Ia1 `libt` C runtime → P4-Ic4 `libthyla-rs` Rust
runtime), and its content lives, code-verified and current, across three
dossiers:

- **the build** (the `usr/` CMake + Cargo project, the two-toolchain split, the
  `aarch64-thylacine` Rust target, the artifact ledger and its stale-artifact
  guards) — sub-substrate-build, which produces "the native and Rust userspace"
  among the rest of the bootable image:

      vault/system/substrate/sub-substrate-build.md   (audit: none)

- **`libt`, the C runtime** (`usr/lib/libt/src/start.S` + the header-only SVC
  wrapper layer `include/thyla/{syscall,poll}.h`) — the userspace side of the
  syscall ABI, so it is owned where that ABI lives:

      vault/system/kernel/entry/sub-kernel-syscall-abi.md   (audit: hard)

- **`libthyla-rs`, the Rust runtime** (the `no_std` `_start` via `global_asm!`, the
  single required `#[panic_handler]`, the syscall wrappers each native Rust binary
  links) — the current native runtime that superseded `libt` for authored
  programs:

      vault/system/userspace/runtime/sub-libthyla-rs.md   (audit: light)

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Clean multi-redirect — a historical doc whose surfaces are all covered.** The
  P4-Ia1 framing ("libt is the native runtime") is superseded: authored native
  userspace is now `libthyla-rs` (Rust), and `libt` (C) survives as a small legacy
  runtime — `start.S` plus header SVC wrappers — for the few remaining C probes.
  The doc itself already tracks the P4-Ic4 handover, so nothing is lost; the vault
  dossiers are the current source of truth.
- **`usr/Cargo.toml` (the native workspace manifest) is UNOWNED** — a
  build-structure orphan. Not force-folded: it belongs with the build-config
  authoring backlog (the uncovered `tools/` build-config surface that
  `150-build-config` also leaves live), not with a dossier about `tools/build.sh`.
  Noted here for that sweep.
- **Everything else was covered.** The `usr/` project structure, the two-toolchain
  split, `libt`, and `libthyla-rs` are all as-built across the three dossiers.
  Zero code change.
