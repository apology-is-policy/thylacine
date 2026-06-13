// kaua -- the native console TUI substrate (LS-7; docs/KAUA.md).
//
// Immediate-mode + double-buffered cell-diff (the ratatui model brought
// native), over /dev/cons + /dev/consctl, themed with Bonfire
// (docs/UTOPIA-VISUAL.md). The pure layers here (style / rect / buffer, and
// the coming widget / layout / event) are terminal-free + host-testable; the
// cons/consctl backend (kaua::term, T-1b) is the only no_std / libthyla-rs +
// audit-bearing layer.
//
// `#![cfg_attr(not(test), no_std)]`: the crate is no_std for the
// aarch64-unknown-none userspace target, but builds against std under
// `cargo test` so the pure layers run on the host.

#![cfg_attr(not(test), no_std)]

extern crate alloc;

pub mod buffer;
pub mod rect;
pub mod style;

pub use buffer::{Buffer, Cell};
pub use rect::Rect;
pub use style::{Attr, Color, Style};
