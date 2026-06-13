// kaua -- the native console TUI substrate (LS-7; docs/KAUA.md).
//
// Immediate-mode + double-buffered cell-diff (the ratatui model brought
// native), over /dev/cons + /dev/consctl, themed with Bonfire
// (docs/UTOPIA-VISUAL.md). The pure layers here (style / rect / buffer / event
// / input / encode, and the coming widget / layout) are terminal-free +
// host-testable; the cons backend (kaua::term, gated on `backend`) is the only
// libthyla-rs + audit-bearing layer.
//
// `#![cfg_attr(not(test), no_std)]`: the crate is no_std for the
// aarch64-unknown-none userspace target, but builds against std under
// `cargo test` so the pure layers run on the host.
//
// FEATURES:
//   - `backend` (DEFAULT) -- compile `kaua::term`, which needs libthyla-rs (an
//     aarch64-thylacine-only crate). The normal device build keeps this on.
//   - Host tests of the pure layers drop it (libthyla-rs does not build on the
//     host):  `cargo test -p kaua --no-default-features --target <host-triple>`.

#![cfg_attr(not(test), no_std)]

extern crate alloc;

pub mod buffer;
pub mod encode;
pub mod event;
pub mod input;
pub mod layout;
pub mod rect;
pub mod style;
pub mod widget;

#[cfg(feature = "backend")]
pub mod source;
#[cfg(feature = "backend")]
pub mod term;

pub use buffer::{Buffer, Cell};
pub use event::{Event, KeyCode, KeyEvent, Mods};
pub use input::Parser;
pub use layout::{Constraint, Direction, Layout};
pub use rect::Rect;
pub use style::{Attr, Color, Style};
pub use widget::{Block, List, Paragraph, Span, StatusLine, Widget};

#[cfg(feature = "backend")]
pub use source::{EventSource, PollSource};
#[cfg(feature = "backend")]
pub use term::Terminal;
