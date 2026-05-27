// ut -- the Utopia shell.
//
// Two letters; mnemonic for Utopia (UTOPIA-SHELL-DESIGN.md section
// 13.1). v1.0-skeleton scope at U-3: print a Pale Fire version banner
// via libutopia + exit cleanly. Subsequent U-* chunks fill in:
//
//   U-4    line editor (raw mode + emacs keys + history)
//   U-5    rc-shape parser + AST
//   U-6    evaluator core + poll() main loop + redirection + pipefail
//   U-7    fd-notes job control (Ctrl-C / Ctrl-Z / & / jobs / fg / bg)
//   U-8    Thylacine builtins (bind / mount / unmount / pivot_root /
//          rfork / cap / note)
//   U-9..N native coreutils, one or two per chunk
//   U-PTY  PTY substrate (/dev/ptmx + /dev/pts/<n> + /dev/consctl)
//   U-Z    bring-up integration test
//
// Per the Plan 9 native-vs-ported split (ARCHITECTURE.md section 3.5):
// `ut` is a native libthyla-rs binary. NOT a Pouch port. NO musl.
// NO POSIX shim. Its panic handler + _start come from libthyla-rs;
// its allocator is libthyla-rs's ThylaAlloc (the convention per
// CLAUDE.md "Native vs ported userspace programs").
//
// Joey spawns /ut at boot AFTER /u-test. Boot-path expectation: the
// shell prints its banner + exits 0; joey reaps + continues. The
// banner emits 24-bit-colour ANSI escapes; terminals that don't grok
// them degrade in their own way (typically by ignoring the SGR and
// printing the text plainly). Per UTOPIA-VISUAL.md section 4.4 the
// recommended terminal set (Ghostty / Kitty / Alacritty / iTerm2 /
// WezTerm / foot) all support 24-bit colour by default.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;
use libthyla_rs::alloc::ThylaAlloc;
use libthyla_rs::t_putstr;
use libutopia::{ansi, palette, GLYPH};

#[global_allocator]
static GLOBAL_ALLOCATOR: ThylaAlloc = ThylaAlloc;

/// The current `ut` skeleton version. Bumped at every U-* chunk that
/// expands the shell's surface. `v0.0.1-skeleton` == U-3 (banner +
/// exit; no REPL, no line editor, no parser, no evaluator).
const UT_VERSION: &str = "0.0.1-skeleton";

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // The Pale Fire version banner. Three composed segments per
    // UTOPIA-VISUAL.md section 3:
    //
    //   `⊢`               glyph orange (palette::GLYPH = #e07840)
    //   ` ut vX.Y-... -- `  foreground white (default; no escape)
    //   `Thylacine ...`   path blue (palette::PATH = #8898b4)
    //
    // The third segment uses Path colour because the tagline is
    // receded metadata, mirroring the prompt-path discipline.
    //
    // Each coloured segment ends with ESC[0m (libutopia::ansi::fg
    // wraps + resets), so colour cannot bleed into joey's subsequent
    // boot-log output.
    let mut banner = String::new();
    banner.push_str(&ansi::fg(palette::Role::Glyph, GLYPH));
    banner.push_str(" ut v");
    banner.push_str(UT_VERSION);
    banner.push_str(" -- ");
    banner.push_str(&ansi::fg(palette::Role::Path,
                              "Thylacine textual shell (U-3 skeleton)"));
    banner.push('\n');
    t_putstr(&banner);

    // Skeleton scope: no REPL at U-3. The shell prints its banner and
    // exits cleanly. Joey's boot-path check expects status == 0.
    0
}
