# RUST-STD -- a Rust `std` target for Thylacine (track R)

**Status: RATIFIED (operator, 2026-09-21). Scripture-before-code.** This note
fixes the load-bearing, hard-to-reverse decisions -- the target name, the
`std::os::thylacine` surface, and the toolchain-pinning policy -- before a line
of the port is written, per the house rule that scripture precedes code where
the code would otherwise determine the scripture. It is track **R** of the
browser arc (`docs/BROWSER-DESIGN.md`, ratified 2026-09-21); the brief is
`docs/handoffs/041-rust-std-track-to-aux.md`. Effort: `xhigh` (the operator's
vote for the whole arc).

**The operator ratified, 2026-09-21:**
- **The target is `aarch64-unknown-thylacine`** (section 5).
- **`std`-on-pouch is a SANCTIONED way to write new first-party Thylacine
  programs, not ports only** (section 9, O-5). This **amends ARCHITECTURE 3.5**
  (which today splits userspace into native `no_std` on `libthyla-rs` and ported
  POSIX on pouch); the amendment is OWED and coordinated with main (ARCH 3.5 is
  shared core scripture and aux's worktree is 154 commits behind main -- editing
  it here would fight main's newer tree). It does not gate R-0/R-1: no
  first-party program is written on the target until it can build one.
- **R-0 is authorized** (the target: fork setup, spec, libc module, std arms).

The `std::os::thylacine` surface (section 6) and toolchain-pinning policy
(section 7) are ratified as the engineering approach; per-arm specifics are
proven at R-0 (section 13). Everything else is engineering the brief settled.

**AS-BUILT (R-0 REACHED, 2026-09-21): `std` compiles for
`aarch64-unknown-thylacine`.** Corrections to this note's pre-build guesses,
now verified (durable + reproducible in `usr/ports/rust/patches/`):
- errno is STANDARD musl, NOT synthesized (fixed in the status table + the libc
  section).
- The rust-src arms are SMALLER than sections 6/8 estimated. What rides FREE via
  `target_family="unix"` + `target_env="musl"`: `os/mod.rs` dispatch, the
  `stack_overflow` opt-out (non-membership = zero code), and `library/unwind`
  (no arm -- the `target_env="musl"` link arm covers it). What actually needed
  arms: `os/thylacine/{mod,raw,fs}` (reached via `os/unix::platform`, not
  `os/mod.rs`), `sys/thread` set_name (-> unsupported), `sys/random` getrandom,
  `sys/paths` current_exe (-> unsupported), `sys/args` imp, and the
  `std/build.rs` restricted_std allowlist. There is NO separate `sys/*/unix.rs`
  cfg-arm series -- the component `unix.rs` files take generic-unix defaults.
- Build mechanism: `-Z build-std=std -Z json-target-spec` (the flag is required
  on this nightly); std's libc is patched in rust-src's `library/Cargo.toml`
  (build-std ignores a `[patch]` in the user crate); rust-src is patched in the
  sysroot in place (no `../rust-thylacine` copy). Full detail:
  `usr/ports/rust/patches/README.md`.

---

## 1. What this is, and the first milestone

A Rust target for Thylacine whose `std` works, so that ordinary crates.io
programs build for the device. It is the gate for **Servo** (the arc's second
engine) and worth having on its own -- it opens crates.io to Thylacine.

**Exit witness, milestone R-1:** a `std` hello-world -- threads, a file read, a
TCP connect, a `HashMap`, and a panic that unwinds -- built by `cargo` and run
on the device.

This is not `libthyla-rs`. Native Thylacine Rust stays `no_std` on
`aarch64-unknown-none` over `libthyla-rs` (`usr/.cargo/config.toml`), untouched.
`std`-on-pouch is a **third** userspace substrate beside ARCHITECTURE 3.5's
native/ported split -- ratified as a sanctioned first-party substrate, not ports
only (section 9, O-5).

---

## 2. The route (settled; main's research, BROWSER-DESIGN 4.3/6-P4/13)

A **`target_family = "unix"`** target over the pouch libc, reusing
`library/std/src/sys/pal/unix`. Not a bespoke platform layer. Measured
precedents: GNU Hurd 2023 (+626/-35 in `rust-lang/rust` PR 115230, +3,297 in
`libc` PR 3325); QNX +603; HelenOS +244 (PR 139310). A bespoke `sys/pal/<os>`
costs ~4x (Motor OS +2,297, Xous +2,484) and pays worse, because most crates
gate on `cfg(unix)`.

**Out of tree first:** a forked `rust` and a forked `libc` crate beside
`llvm-thylacine`, a pinned toolchain (custom-target JSON fields are unstable).
The repo carries the build wiring + patches; the forks live outside it (the
`llvm-thylacine` / `webkit-thylacine` precedent). Tier-3 upstream once the diff
is stable and under ~1k lines (Managarm, HelenOS are the recent precedents).

**The `libc` crate module is the larger half of the work**, not the `std` diff.

---

## 3. The tree's starting line (verified at `origin/main`, 2026-09-21)

| Piece | State |
|---|---|
| C toolchain | clang 22 / LLVM `llvmorg-22.1.8` fork, real `aarch64-thylacine` triple (`docs/LLVM-DESIGN.md` CL-3). Static libunwind + libc++abi + libc++; unwinding proven on-device. |
| pouch libc | musl **1.2.5**-derived (`third_party/musl/`, pristine) + a **30-patch** boundary-line series (`usr/lib/pouch/patches/0001..0030`). Upper half = musl unmodified; lower half = Thylacine-native. Sysroot: `build/sysroot/{include,lib,bin}` (`libc.a`, CRT). Syscall ABI is Thylacine-native numbers (musl's `bits/syscall.h` regenerated); errno is STANDARD musl (AS-BUILT R-0 correction: the seam's `syscall_ret.c` passes `-errno` in `[-4095,-2]` through unchanged and maps a flat -1 to EIO -- no `bits/errno.h` patch; getuid's -38 = musl ENOSYS confirms). |
| Native Rust | `no_std` on the built-in `aarch64-unknown-none` over `libthyla-rs`; **144 crates vendored** under `third_party/rust`. **No `std` port** -- this arc. |
| Custom targets | flagged in `usr/.cargo/config.toml` as a "Phase 5+" option, never built. |

---

## 4. The pouch surface `std` rests on -- and the gaps (verified, POUCH-DESIGN 8)

The load-bearing facts, because they decide what `std` can and cannot do here.

**What works, and is the enabler:**
- **The allocator.** pouch `malloc` = musl `mallocng`, resting on the native
  `burrow_attach(len)->vaddr` / `burrow_detach(vaddr,len)` pair (POUCH-DESIGN
  8.1 RESOLVED). Rust's default `System` allocator is the libc `malloc`, so it
  works with **no** Rust-side memory work. `brk`, `madvise`, `mprotect`,
  `mremap` are tolerated no-ops; mallocng needs none.
- pthreads, `poll`, pipes, pty, `fork`/`execve` (patch 0026), `AF_INET` via
  netd, `AF_UNIX SOCK_STREAM` over `/srv` (`SO_PEERCRED`), `getrandom`, the wall
  clock, ELF TLS. This covers the R-1 milestone's threads / file / TCP / panic.

**The gaps `std` must be built to tolerate:**
- **Stack guard pages -- an inherited v1.0 limitation** (POUCH-DESIGN 8.2,
  P6-pouch-threads-b F2). pouch `__mmap` ignores `prot` and returns RW;
  `__mprotect` is `ENOSYS`. musl's `pthread_create` maps the stack `PROT_NONE`
  then `mprotect`s the writable part -- both return RW, so the guard region is
  RW and a stack overflow corrupts it silently, faulting only past the whole
  region. This RW-guard limitation is **pthread-created (spawned) stacks only**
  (main's turn-8 correction): the MAIN thread's stack is a 1 MiB SPARSE
  demand-zero reservation `[0x7ff00000, 0x80000000)` over a REAL `prot==0` guard
  VMA the kernel maps at exec (`exec_map_user_stack` = `burrow_create_anon_lazy`
  since LINEAGE L-4a; the old "committed whole at exec" comment was stale), so
  the main thread HAS a working guard and touching low pages just demand-zeroes
  them. The `std` side of this is CLEAN and needs no kernel change (prior-art
  survey, section 6): `std`'s own guard install
  (`sys/pal/unix/stack_overflow.rs`) is gated on an explicit OS allowlist, so
  the opt-out is simply **not adding `thylacine` to that list** -- the no-op
  stub runs, `mprotect` is never called, and we lose only the friendly
  "thread overflowed its stack" message. `std`'s `Thread::new`
  (`sys/thread/unix.rs`) sets the stack size but never calls
  `pthread_attr_setguardsize`, so `std` never asks pthread for a guard. The one
  RESIDUAL risk is **pouch-side, not `std`-side**: musl's `pthread_create` may
  `mprotect` a default guard internally; under ENOSYS that could fail
  `pthread_create` (to confirm at R-0). If so the fix is a pouch change (default
  guardsize 0), a yip item for main, **NOT** a permission syscall on this track
  (brief: "do not add a permission syscall"; that is main's B-1 / I-12).
- **file-backed `mmap` is `ENOSYS` by design** (I-12 / ARCH 6.5). `std::fs`
  never mmaps, so `std` itself is unaffected; the crate tail's `memmap2` will
  hit it and needs a fallback (its `stub` feature or a patch) -- a later item.
- **Static linking only; no `dlopen`** (#115). `std` is fine; note it for
  crates that probe for dynamic loading.
- `panic = "unwind"` should work (libunwind is present + proven); **confirm on
  device, do not assume** -- it is R-1's unwind leg.
- No `socketpair` (BROWSER-DESIGN 3); `std` does not require it.

**Two pouch fixes already in flight on main's `browser-b0` (`da87cffe`, measured
on device bringing up JavaScriptCore), NOT yet on main -- both bite `std`:**
- **Patch 0033 -- the main-thread stack extent.** `pthread_getattr_np()` reports
  a ONE-PAGE main stack (4096 B) because musl's main-thread arm probes the
  extent with `mremap()`, which the seam ENOSYSes (errno != ENOMEM), so the loop
  never runs. Real main stack is 1 MiB. **Rust `std`'s unix main-thread path
  asks exactly this** (main's finding). 0033 states the exec mapping. Main is
  landing 0033/0034 on main via a from-scratch sysroot rebuild + suite + ci; if
  R-0 blocks before they arrive, **cherry-pick from `browser-b0` @`0d3f8ee1` --
  do not re-derive** (main's turn-8 correction; NOT `da87cffe` -- 0034 changed
  in main's self-audit: a value running to the end of the read buffer is now a
  miss, honest `-1` leaving errno as the caller had it).
- **Patch 0034 -- `sysconf`.** `_SC_PHYS_PAGES` / `_SC_AVPHYS_PAGES` return
  uninitialised stack (upstream never checks the ENOSYSed `sysinfo()`); 0034
  reads `/ctl/memory`, `-1` on a miss. Some crates call this.

Spare threads get musl's 128 KiB default stack; Rust sets its 2 MiB spawn stack
explicitly, so `std` is fine -- but a crate relying on the default is tight.

**Bring-up debugging (steal from main):** on pouch `abort()` is a SILENT
`_Exit(127)` -- indistinguishable from `ut`'s "command not found", and `ut`
does not clear `$errstr` on success. A `panic = "abort"` binary dying silently
looks identical. Main's fix, worth reusing for `std` bring-up: a ~12-line
`abort()` override that prints return addresses through `_Unwind_Backtrace`
(libunwind is in the sysroot), linked ahead of `libc.a` -- turns each silent
127 into a one-boot diagnosis.

---

## 5. DECISION -- the target name and spec  *(RATIFIED 2026-09-21)*

**Recommendation: `aarch64-unknown-thylacine`.**

Rust triples are `<arch>-<vendor>-<os>[-<env>]`. Two strings must not be
conflated: the Rust **target name** (the `--target` string / JSON filename) is a
free identifier and need not be a valid LLVM triple (RTEMS ships
`armv7-rtems-eabihf`); the spec's **`llvm-target`** field is what LLVM's `Triple`
parses positionally, and `cfg(target_os)` / `target_family` come from the spec's
own `os` / `families` fields, not from the triple string. So the clang C triple
`aarch64-thylacine` (2-component) and the Rust name are independent -- only
ABI/arch agreement at the link step matters, and it holds.

Why the vendor `unknown` rather than matching clang's 2-component form: every
comparable OS port carries it -- `aarch64-unknown-{redox,helenos,fuchsia,haiku,hermit}`,
`x86_64-unknown-hurd-gnu`, `aarch64-unknown-managarm-mlibc` -- and Fuchsia was
deliberately **renamed** from `x86_64-fuchsia` to `x86_64-unknown-fuchsia` for
consistency, which is direct precedent against a vendorless name. The tier-3
policy is explicit: "use naming consistent with any existing targets ... the
same names and naming conventions as used elsewhere ... getting the name right
is important even for a tier 3 target." A single libc (pouch) means no env
segment is needed; if a second libc is ever foreseen the managarm precedent
supports `-thylacine-pouch` (env = libc name), but that is not the case now.

Set `llvm-target` to the same `aarch64-unknown-thylacine` (a 2-component
`llvm-target` would make LLVM see os=Unknown -- usually harmless for aarch64
codegen but it needlessly discards OS-conditional behavior). The spec is a
checked-in JSON (section 7), copying the Hurd unix-family base's field posture:
`arch: aarch64`, `data-layout` + `max-atomic-width: 128` + `features: "+v8a"`
(the aarch64 defaults), `os: "thylacine"` (via the `Os::Other` escape, section
7), `target-family: ["unix"]`, `has-thread-local: true`, `relro-level: full`,
`position-independent-executables` per the pouch link posture, the pouch
`linker`, and `panic-strategy: unwind` (section 6; libunwind is present). Exact
`data-layout` / linker / PIE-vs-static fields are copied from a `--print
target-spec-json` of a near target and reconciled with the pouch sysroot's
actual link line at R-0 -- an implementation detail, not a scripture decision.

The **scripture decision here is the name**: `aarch64-unknown-thylacine`.

---

## 6. DECISION -- the `std::os::thylacine` surface  *(RATIFIED 2026-09-21)*

Small, and modelled on `std::os::hurd`. Three files under
`library/std/src/os/thylacine/`:
- **`mod.rs`** -- the module root (`pub mod fs; pub mod raw;`). This is the only
  compile-*required* file (so the `os/mod.rs` arm resolves).
- **`fs.rs`** -- the `MetadataExt` trait (`std::os::thylacine::fs::MetadataExt`),
  the `st_*` accessors over pouch's `stat` (`st_dev, st_ino, st_mode, st_nlink,
  st_uid, st_gid, st_rdev, st_size, st_atime[_nsec], st_mtime[_nsec],
  st_ctime[_nsec], st_blksize, st_blocks`). Idiomatic, not strictly required to
  compile, but downstream unix crates expect `std::os::<os>::fs::MetadataExt`
  and `std::os::unix::fs::MetadataExt` to exist, so we provide it.
- **`raw.rs`** -- the deprecated per-OS raw type aliases (the legacy surface);
  provided for parity.

Plus two registration lines: `#[cfg(target_os = "thylacine")] pub mod
thylacine;` in `os/mod.rs`, and the `platform` glob
`#[cfg(target_os = "thylacine")] pub use crate::os::thylacine::*;` in
`os/unix/mod.rs`.

**The `sys/*/unix.rs` arms.** Current `std` (what a fresh pin targets) moved the
per-subsystem code OUT of `sys/pal/unix/` into `library/std/src/sys/{args, env,
fd, fs, net, process, thread, pipe, random, stdio, time, os_str, path}/unix.rs`;
`sys/pal/unix/` now holds only `mod.rs`, `stack_overflow.rs`, `time.rs`,
`thread_parking.rs`, `conf.rs`, `weak/`, `sync/`. The Hurd PR (115230) is the
semantic map of *which* arms exist; the paths are the current ones. Where pouch
matches musl (most arms), the edit is **adding `target_os = "thylacine"` to an
existing `any(...)` cfg list**, not new code -- which is what keeps the diff
small. The per-arm specifics (which `stat`/`dirent`/`SOCK_*`/`clock` fields
exist, `current_exe`'s route) are confirmed against pouch at R-0.

**The stack-guard opt-out (the biggest simplifier).**
`sys/pal/unix/stack_overflow.rs` gates its real SIGSEGV handler + guard install
on an explicit `target_os` allowlist (linux/freebsd/hurd/macos/netbsd/openbsd/
solaris/illumos). **We simply do NOT add `thylacine`** -> the no-op stub runs,
`mprotect` is never called, no guard page is expected. No code is written for
this; non-membership IS the opt-out. (Section 4 has the pouch-side residual.)

**The scripture decision here is the surface shape**: the three
`os/thylacine/{mod,raw,fs}.rs` files (with `MetadataExt` over pouch's `stat`),
the two registration lines, `target_os = "thylacine"` added to the existing unix
`sys/*/unix.rs` arms, and non-membership in the `stack_overflow` allowlist.

---

## 7. DECISION -- toolchain pinning policy  *(RATIFIED 2026-09-21)*

**Out-of-tree first, via a pinned nightly + a JSON custom target + `-Z
build-std` over a patched `rust-src` and a forked `libc` crate. NO rustc build.**

The pieces and why each is pinned:
- **The JSON custom-target spec is explicitly unstable** ("target JSON
  properties are not stable ... always pin your compiler version"), so the
  toolchain is pinned to one exact nightly.
- **`target_os = "thylacine"` needs no rustc fork.** The compiler's `Os` enum
  has an `Other(String)` escape, so a JSON spec with `os: "thylacine"` resolves
  `cfg(target_os = "thylacine")`. (`check-cfg` will warn on the unknown os;
  benign.)
- **`-Z build-std` (nightly) rebuilds `core`/`alloc`/`std`** for the target the
  toolchain does not ship precompiled: `cargo +<pin> build -Z
  build-std=core,alloc,std --target aarch64-unknown-thylacine.json`. It builds
  `std` from the pinned `rust-src`, so **our `sys/*/unix.rs` + `os/thylacine`
  arms live in a patched `rust-src`** -- that is the "fork" (a patch series over
  a pinned `rust-src`, not a compiled rustc). The forked `libc` crate is pointed
  at with `[patch.crates-io] libc = { path = ... }` (exact wiring confirmed at
  R-0).

**What the repo carries** (the forks live outside it, like `llvm-thylacine`):
`rust-toolchain.toml` (the exact `nightly-YYYY-MM-DD` pin + `rust-src`),
`aarch64-unknown-thylacine.json`, a `tools/` build recipe (mirroring the clang /
Mesa fork recipes), and the `rust-src` + `libc` patch series. The pin advances
deliberately, never drifts.

**Tier-3 in-tree, later** (once the diff is stable and < ~1k lines): add an
`Os::Thylacine` variant, `base/thylacine.rs` (copying Hurd's unix-family base --
Hurd, not HelenOS, is the one that sets `families = ["unix"]`), and
`targets/aarch64_unknown_thylacine.rs`. In-tree removes the `build-std` /
`check-cfg` friction and the known JSON-target std feature-detection gaps
(wg-cargo-std-aware#60: a JSON target once "did not see networking"). Managarm
and HelenOS are the recent tier-3 precedents.

**The scripture decision here is the policy**: out-of-tree pinned-nightly + JSON
+ `build-std` over a patched `rust-src` + forked `libc` now; tier-3 in-tree
once stable.

---

## 8. The out-of-tree layout

- **A patched `rust-src`** (the pinned nightly's `rust-src` component + a patch
  series adding the `sys/*/unix.rs` arms, `os/thylacine/{mod,raw,fs}.rs`, and
  the `library/unwind` link arm). Built by `-Z build-std`, not a compiled rustc.
- **`libc-thylacine`** (forked `rust-lang/libc`, pinned): the new-OS module
  `src/unix/thylacine/mod.rs` (AS-BUILT: one file suffices, aarch64-only),
  seeded from `src/unix/linux_like/linux/musl/` + its `b64/aarch64` arch file.
  AS-BUILT R-0 correction: errno constants are STANDARD musl (NOT synthesized --
  see the status table row); SYS_* numbers mostly ENOSYS in pouch but std as a
  non-linux unix calls libc FUNCTIONS, not raw `syscall(SYS_*)`, so they do not
  reach std. Plus the `src/unix/mod.rs` dispatch arm, the `build.rs` check-cfg,
  and the `src/new/` pthread-gate additions (an upstream gap for non-linux musl).
  Only the `library/std`-referenced items at first (the initial Hurd libc PR was ~3.3k
  lines; the module grows as it completes). This is the larger half of the work.
- **In this repo:** `aarch64-unknown-thylacine.json`, the `rust-toolchain.toml`
  pin, the `tools/` build recipe (mirroring the clang/Mesa fork recipes), the
  `rust-src` + `libc` patch series, status rows in `docs/browser-status.md`
  (track R), and the milestone probes. Everything is reproducible-from-source
  and rebuilt on the build host.

---

## 9. O-5 -- what `std`-on-pouch is *for*  *(RATIFIED 2026-09-21)*

ARCHITECTURE 3.5 splits userspace into native `no_std` on `libthyla-rs` and
ported POSIX code on pouch. `std`-on-pouch is a **third** substrate: Rust
programs that are POSIX-shaped. **The operator ruled (2026-09-21): it is a
SANCTIONED way to write new first-party Thylacine programs, not ports only.** So
Thylacine userspace now has three sanctioned substrates:
1. native `no_std` Rust on `libthyla-rs` (the Thylacine-shaped default);
2. ported foreign POSIX code on pouch (musl + boundary-line patches);
3. **first-party `std` Rust on pouch** (this target) -- for programs that want
   `std` + the crates.io ecosystem and accept the pouch POSIX surface.

The choice between (1) and (3) for a NEW program is the author's, per the
program's needs (a Thylacine-shaped daemon wanting the smallest surface -> (1);
a program wanting threads/`std::fs`/`std::net`/crates -> (3)). This is not a
demotion of `libthyla-rs`, which stays the native default and is untouched.

**ARCHITECTURE 3.5 amendment OWED.** The §3.5 wording (native/ported dichotomy)
must gain this third substrate. ARCH is shared core scripture and aux's worktree
is 154 commits behind main, so the edit is coordinated with main (who owns the
merge) rather than made on a stale tree -- flagged to main on yip. It does not
gate R-0/R-1 (the distinction only matters once a first-party `std` program is
written, which is post-R-1).

---

## 10. Build + host plan

The Rust bootstrap is all-core and ~20-30 GiB. The operator freed space on the
Mac (36 GiB as of 2026-09-21), so the bootstrap runs **on the Mac**, with core
coordination with main on yip (WebKit and Rust bootstrap are both all-core jobs
on the one 8-core machine -- `hold mac` + announce before each). **Fallback:**
`thyla-keep` (the permanent GCP aarch64 builder, 32c/125 GB/150 GB disk, already
holds the LLVM fork) if the Mac's headroom or contention bites.

---

## 11. The crate tail + the milestone plan

**Crate tail Servo needs** (servo #30541): `libc` (biggest), `mio` (has a
`poll(2)` selector -- a one-line list patch or `--cfg mio_unsupported_force_poll_poll`),
`socket2`, `nix`, `rustix`, `getrandom`, `ring` (aarch64 asm, OS-independent) or
`aws-lc-sys` (C+asm+CMake -- a risk; rustls+ring is the fallback), `ipc-channel`
(`force-inprocess`), `memmap2` (the file-mmap gap, section 4), `parking_lot`.

**Sub-chunks (in order):**
- **R-0** -- the target: the pin + JSON spec, the `libc` module, the `std`
  `sys/*/unix.rs` + `os/thylacine` arms, the `stack_overflow` opt-out (non-
  membership -- no code), the `library/unwind` hookup for `panic = "unwind"`,
  `-Z build-std`. Depends on pouch **0033** (main-thread stack) + **0034**
  (sysconf) from main's `browser-b0` -- cherry-pick from `da87cffe` if not yet
  on main (section 4). Confirm at R-0: pouch `pthread_create`'s default guard
  under ENOSYS (section 4 residual), the exact per-arm field specifics, and
  `panic = "unwind"` on device (fall back to `abort` only if the unwind wiring
  stalls, the HelenOS precedent -- with the `abort`-backtrace shim from section 4
  so a silent 127 is diagnosable). Exit: `std` compiles for
  `aarch64-unknown-thylacine`.
- **R-1** -- the milestone hello on the device (threads/file/TCP/HashMap/unwind).
- **R-2** -- the crate tail (libc first, then mio/socket2/getrandom/ring/...).
- **R-3** -- Servo bringup (gated on R-2), tracked against BROWSER-DESIGN 4.3.

---

## 12. Risks

- **The guard-page opt-out is not clean.** If `std`'s stack-overflow handling
  cannot be disabled by cfg, a small `std` patch is needed (not a kernel
  change). Bounded; section 6 is the first thing to verify in R-0.
- **`-Z build-std` + a custom `libc` fork** is the fragile seam (the pin exists
  for exactly this). Mitigate by pinning a known-good nightly and keeping the
  `std` diff under ~1k lines.
- **`aws-lc-sys` / `ring`** for the TLS crates in the tail is a C/asm/CMake
  build; rustls + ring is the fallback. R-2, not R-0/R-1.
- **`getuid`/`geteuid`/`getgid`/`getegid`/`getppid` return the raw ENOSYS
  sentinel** `0xFFFFFFDA` = `(uid_t)-38` (main's turn-8 finding; the kernel has
  `SYS_GETUID=73`/`SYS_GETGID=74` but CL-1a wired only `getpid`; musl treats
  these as cannot-fail). `std` itself does not need them for R-1, but crates in
  the tail will. The fix is **NOT this track's**: stratumd consumes the value
  (admin-uid, the keyslot-token gate, dataset-root ownership), so it is an
  A-3-surface chunk owned by main/the operator. Track R depends on it landing
  before any tail crate that reads a uid. (`umask`/`times`/`uname` are parked
  too but fail visibly.)
- **The arc must never risk the v1.0-rc** (ROADMAP 11; BROWSER-DESIGN 10). Track
  R is independent of the kernel release path.

---

## 13. Sources

The Thylacine-side facts (sections 3-4) are verified against `origin/main`
(`docs/POUCH-DESIGN.md` 8.1/8.2, `usr/lib/pouch/patches/`, `usr/.cargo/config.toml`,
`docs/LLVM-DESIGN.md`). The Rust-side facts (sections 5-8) come from a prior-art
survey of primary sources on 2026-09-21 (rustc target-tier-policy + custom-target
books; rust-lang/rust PR 115230 [Hurd std], PR 139310 [HelenOS target-registration
only, NOT a std port]; rust-lang/libc PR 3325 [Hurd libc]; current
`library/std/src/sys` + `.../os/hurd` + `.../sys/pal/unix/stack_overflow.rs` +
`.../sys/thread/unix.rs`; `compiler/rustc_target/src/spec/{mod,base/hurd,base/helenos}.rs`;
platform-support.md; wg-cargo-std-aware). Items flagged **"confirm at R-0"** above
were marked unverified in the survey (LLVM 2-component triple parsing; per-arm
field specifics; `fs.rs` compile-required vs idiomatic; the pouch `pthread_create`
guard-under-ENOSYS risk; the `[patch.crates-io] libc` exact wiring) and are proven
against the pinned toolchain + pouch at R-0, not asserted here.
