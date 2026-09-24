// aurora (lib) -- the renderer's pure half: the cell painter (`render`), the
// F10 settings panel's state machine and drawing (`osd`), and the config
// file's grammar (`config`). None of it takes a syscall except the config
// file's load and save, which the `backend` feature gates; the bin half
// (main.rs) owns the console drain/feed pair, the tapestry surface and the
// loop.
//
// The split exists for the tests. aurora was a bin-only `no_std` crate, so
// `cargo test` could not build it for any host, and the nine unit tests these
// modules carried were written "DORMANT" -- pinned contracts that nothing ran.
// With the bin's dependencies behind `backend`, `--no-default-features` builds
// the three modules for the host and `tools/test-rust.sh` runs them.

#![no_std]

extern crate alloc;

pub mod config;
pub mod osd;
pub mod render;
