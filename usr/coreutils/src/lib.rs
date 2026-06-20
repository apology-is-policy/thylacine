// coreutils -- shared presentation helpers for the Thylacine coreutils.
//
// The native coreutils (ls, grep, stat, ...) share one visual language: the
// Bonfire palette (docs/UTOPIA-VISUAL.md U-2, matching nora + ut) rendered as
// truecolor SGR, box-drawing furniture for listings, and a color gate so the
// SAME formatting code stays byte-clean when color is off.
//
// THE DISCIPLINE (COREUTILS-THYLACINE-DESIGN.md): color belongs on PRESENTATION
// (a listing, a header) and DIAGNOSTICS (errors), never on a data PAYLOAD a pipe
// consumes (cat/sort/cut/tee/wc emit clean bytes) -- a colored payload corrupts
// `tool | tool`. So these helpers are for the presentation tools; the filters do
// not pull them in.
//
// PURE no_std + alloc (no libthyla-rs), so the width math + the SGR wrapping are
// host-tested (`cargo test -p coreutils --lib --target aarch64-apple-darwin`).
// The TTY probe that would decide `--color=auto` needs a syscall, so it lives in
// the bin (ls) that wants it, not here.

#![no_std]

extern crate alloc;

pub mod boxd;
pub mod color;
pub mod palette;
pub mod size;

// Backend-gated (these touch libthyla-rs): the metadata-presentation helpers
// (ls / stat / realm / qid) and the --help / bad-usage plumbing.
#[cfg(feature = "backend")]
pub mod meta;
#[cfg(feature = "backend")]
pub mod netpump;
#[cfg(feature = "backend")]
pub mod usage;
