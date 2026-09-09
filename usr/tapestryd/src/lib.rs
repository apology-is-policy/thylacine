// tapestryd (lib) -- the compositor's thinking half: everything that decides
// geometry, roles and bindings, and nothing that syscalls. Every module here
// is host-testable; the bin half (main.rs, the `guest` feature) owns the
// driver, the GPU rings, the 9P server and the event loop.
//
// The split landed 2026-09-09 because the crate had none: bin-only plus an
// unconditional libthyla-rs meant `cargo test -p tapestryd` failed to compile
// on the host, so chords.rs's four tests had never executed. See the manifest
// for the mechanism.

#![no_std]

extern crate alloc;

pub mod chords;
pub mod keymap;
pub mod pane;
