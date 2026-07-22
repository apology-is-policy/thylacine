// nora -- a native modal text editor on Kaua (LS-7; docs/KAUA.md).
//
// The library half: the pure, host-testable editor engine. `text` is the
// line-buffer (the tui-textarea replacement), `editor` the modal core (adapted
// from Stratum's editor.rs onto kaua key events), `view` the renderer into a
// kaua Buffer, `theme` the Bonfire palette. None of these touch a terminal or
// libthyla-rs -- the binary (src/main.rs) wires them to the Kaua backend
// (Terminal + PollSource) + file I/O behind the `backend` feature.
//
// `#![cfg_attr(not(test), no_std)]`: no_std for aarch64-unknown-none; std under
// `cargo test` so the engine runs on the host. Host-test the library only
// (the bin needs the backend):
//   cargo test -p nora --no-default-features --lib --target <host-triple>

#![cfg_attr(not(test), no_std)]

extern crate alloc;

pub mod debug;
pub mod diag;
pub mod editor;
pub mod syntax;
pub mod text;
pub mod theme;
pub mod vartree;
pub mod view;
pub mod wrap;

pub use debug::{DebugView, GoroutineRow, StackRow, VarRow};
pub use vartree::VarNode;
pub use diag::{Diagnostics, LineDiag, Severity};
pub use editor::{Candidate, DapRequest, DashPane, Editor, LspRequest, Mode, Request};
pub use syntax::{HlClass, HlSpan, Lang};
pub use text::TextBuffer;
