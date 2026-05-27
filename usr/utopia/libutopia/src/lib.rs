// libutopia -- the Utopia application helpers.
//
// Per docs/UTOPIA-SHELL-DESIGN.md sections 14 and 15: libutopia is the
// application-layer helper crate for Utopia's textual surface. Where
// libthyla-rs is the generic kernel-surface Rust runtime (idiomatic
// wrappers over every Thylacine syscall), libutopia is the
// Utopia-specific application layer -- palette + ANSI emission + path
// display + (future U-* chunks) line editor + prompt-emit.
//
// Consumed by:
//   - `ut`     -- the Utopia shell (U-3 onward)
//   - Coreutils (`cat`, `ls`, `echo`, ...; U-9..N)
//   - Any future Utopia-shaped binary needing Pale Fire visual identity
//
// Modules at U-3 (skeleton):
//   palette  Pale Fire four-color constants + Role enum
//            (UTOPIA-VISUAL.md sections 1 and 4.1)
//   ansi     ANSI 24-bit SGR escape emission + reset
//            (UTOPIA-VISUAL.md section 4.3)
//   path     Path display helpers (HOME-abbreviation, etc.)
//            (UTOPIA-VISUAL.md section 3.1)
//
// Modules deferred to later U-* chunks:
//   line_editor (U-4)  prompt (U-4)  tty (U-PTY)
//   ninep + notes are already covered by libthyla-rs (U-2h-ninep + U-2e).

#![no_std]

// libutopia depends on alloc for String composition in ansi.rs. The
// consumer binary (`ut` or a coreutil) declares its own
// #[global_allocator] per the libthyla-rs convention (canonical choice
// is libthyla_rs::alloc::ThylaAlloc).
extern crate alloc;

pub mod ansi;
pub mod eval;
pub mod line_editor;
pub mod palette;
pub mod parser;
pub mod path;

// Re-export the canonical Pale Fire glyph -- `⊢` U+22A2 RIGHT TACK
// (UTOPIA-VISUAL.md section 2). Programs that want to emit it in
// glyph orange compose:
//
//     ansi::fg(palette::Role::Glyph, GLYPH)
//
// `⊢` is a 3-byte UTF-8 sequence (E2 8A A2). Pinning the spelling
// here keeps every emission site agreeing on bytes.
pub const GLYPH: &str = "\u{22a2}";

// Continuation glyph for multi-line input -- per UTOPIA-VISUAL.md
// section 3.2. `⋮` U+22EE VERTICAL ELLIPSIS at palette::Role::Path
// color. 3-byte UTF-8 (E2 8B AE).
pub const CONTINUATION_GLYPH: &str = "\u{22ee}";
