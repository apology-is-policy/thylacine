// haul (lib) -- the parts that compute and never syscall.
//
// The split exists for the reason tapestryd's manifest states as THE H-2a
// LESSON: a no_std BIN crate's tests are DORMANT. haul's bin half depends on
// libthyla-rs, whose inline aarch64 asm cannot even be assembled for the host,
// so anything living only in main.rs can never be tested here -- and a test
// that cannot compile is indistinguishable from one that passes.
//
// That matters more for this crate than most, because the thing in it is
// CRYPTO. `npxf` is pinned against known-answer vectors produced by npxf's own
// C++ implementation (`kat/vectors.txt`), which is the only way to know the two
// agree byte for byte without booting a guest and reading an opaque tag
// failure.
//
//   cargo test -p haul --lib --no-default-features --target aarch64-apple-darwin
//
// `not(test)` rather than a bare `no_std`: the libtest harness needs std, and
// under cfg(test) `alloc` still resolves (std re-exports it), so the module
// source is identical in both builds.
#![cfg_attr(not(test), no_std)]

extern crate alloc;

pub mod addr;
pub mod cmdline;
pub mod npxf;
