# Handoff 041 -- track R: Rust `std` for Thylacine (to the aux track)

**From**: main, 2026-09-21. **To**: aux. **Why you**: the operator, the same
day they ratified `docs/BROWSER-DESIGN.md`: "I will launch Aux to deliver the
Rust STD." Main is on WebKit (B-0 onward); this track is independent of it.

## What is being asked

A Rust target for Thylacine whose `std` works, so that ordinary crates.io
programs build for the device. It is the gate for **Servo** (the browser arc's
second engine) and later Ladybird (now one third Rust, all of it `std`), and it
is worth having for reasons that have nothing to do with browsers.

Exit witness for the first milestone: **a `std` hello-world -- threads, a file
read, a TCP connect, a `HashMap`, a panic that unwinds -- built by cargo and run
on the device.**

## What research already established (sources in BROWSER-DESIGN section 13)

- **Route: a `target_family = "unix"` target over the Pouch libc, reusing
  `library/std/src/sys/pal/unix`.** Not a bespoke platform layer. Measured
  precedents: GNU Hurd 2023, +626/-35 lines in `rust-lang/rust` (PR 115230)
  and +3,297 in the `libc` crate (PR 3325); QNX `std` +603 (PR 106673); HelenOS
  +244 (PR 139310, 2025-11). Bespoke layers cost about four times that (Motor
  OS +2,297, Xous +2,484) and pay worse: most crates gate on `cfg(unix)`.
- **Where the work lands** (from the Hurd PR's file list): a `rustc_target`
  spec; `library/std/build.rs`; a new `std/src/os/<name>/{fs,raw,mod}.rs`
  (the largest piece, about 390 lines); small cfg arms across
  `sys/unix/{args,env,fd,fs,net,os,process,thread,thread_local_dtor,time,stack_overflow}`;
  four lines in `library/unwind` to link the unwinder; bootstrap; docs. **The
  `libc` crate module is the larger half.**
- **Out of tree first**: a forked `rust` + a forked `libc`, a pinned toolchain
  (custom-target JSON fields are unstable). The rebase burden stays small while
  the `std` diff is under about a thousand lines. Tier 3 upstream (named
  maintainers, no burden on others) once it is stable; Managarm and HelenOS are
  the recent precedents.
- **The crate tail Servo will need** (its OpenHarmony tracking issue, servo
  #30541, lists what actually broke there): `libc` (biggest), `mio` (already has
  a `poll(2)` selector used by Haiku/Hurd/QNX/Fuchsia -- a one-line list patch,
  or `--cfg mio_unsupported_force_poll_poll`), `socket2`, `nix`, `rustix`,
  `getrandom`, `ring` (its aarch64 assembly choice does not depend on the OS)
  or `aws-lc-sys` (C + asm + CMake: a risk; rustls + ring is the fallback),
  `ipc-channel` (`force-inprocess`), `memmap2`, `parking_lot`.

## What the tree gives you, and what it does not (measured at `99e19194`)

- The clang triple is **`aarch64-thylacine`** (LLVM fork `llvmorg-22.1.8`,
  `docs/LLVM-DESIGN.md` CL-3); libunwind + libc++abi + libc++ are static in
  the Pouch sysroot and unwinding is proven on-device. Native Rust today is
  `no_std` on the built-in `aarch64-unknown-none` target over `libthyla-rs`
  (`usr/.cargo/config.toml`); 144 crates are vendored under `third_party/rust`.
- Pouch has pthreads, `poll`, pipes, pty, `fork`/`execve`, `AF_INET` through
  netd, `AF_UNIX SOCK_STREAM` over `/srv`, `getrandom`, the wall clock, ELF TLS.
- **Pouch `mmap` is anonymous-only, kernel-chosen address; `MAP_FIXED`,
  `mprotect`, `madvise`, `mremap`, partial `munmap` and file-backed `mmap` are
  all `ENOSYS`.** `std` itself tolerates most of this, with two places to look
  at early: the main-thread and spawned-thread **stack guard page** code in
  `sys/pal/unix/thread.rs` / `stack_overflow.rs` (some OS arms `mprotect` a
  guard, others opt out -- read the current source, this is recalled, not
  verified), and `std::fs` paths that assume `mmap` never.
- Static linking only; no `dlopen`. `panic = "unwind"` should work (libunwind
  is there); confirm rather than assume.
- **The anonymous-memory surface (P1) is main's B-1 and is a scripture commit
  with the operator's signature (I-12's wording). Do not add a permission
  syscall on this track.** If `std` needs something from P1, tell main on yip.

## An open question that is the operator's, flagged in the design as O-5

ARCHITECTURE section 3.5 splits userspace in two: native `no_std` on
`libthyla-rs`, and ported POSIX code on Pouch. **Rust-`std`-on-Pouch is a third
thing.** Whether it is for ports only, or becomes a sanctioned way to write new
Thylacine programs, is not decided. Build the target; do not start writing
first-party programs against it until the operator says which it is.

## House rules that bind here as everywhere

Scripture before code where the code would otherwise determine scripture (a
new target name, a new `std::os::thylacine` API surface, and the toolchain
pinning policy all qualify -- a short design note first). Each new POSIX
surface `std` touches that Pouch lacks is a Pouch patch under the Pouch audit
discipline. Status rows go in `docs/browser-status.md` (track R) or a status doc
of your own if it grows. Coordinate host cores with main on yip: WebKit builds
and Rust bootstrap builds are both all-core jobs on one 8-core machine.
