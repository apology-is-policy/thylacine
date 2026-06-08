// ut -- the Utopia shell.
//
// Two letters; mnemonic for Utopia (UTOPIA-SHELL-DESIGN.md section
// 13.1). Through U-6g `ut` is a working read-parse-eval REPL: it draws
// the Pale Fire prompt, reads input from fd 0, drives the libutopia line
// editor (U-4) + parser (U-5) + evaluator (U-6), and loops. The U-* arc
// still fills in:
//
//   U-7    fd-notes job control (Ctrl-C / Ctrl-Z / & / jobs / fg / bg) --
//          the MULTI-fd poll() across the notes fds; predicated on the
//          U-PTY line discipline (UTOPIA-SHELL-DESIGN.md section 10.2).
//   U-8    Thylacine builtins (bind / mount / unmount / pivot_root /
//          rfork / cap / note)
//   U-9..N native coreutils, one or two per chunk
//   U-PTY  PTY substrate (/dev/ptmx + /dev/pts/<n> + /dev/consctl) --
//          a pollable line-discipline slave + termios + per-Proc fd
//          0/1/2. Until then /dev/cons is blocking-read-only (no .poll
//          hook), so the loop below blocks in read(); the single-fd
//          poll() is the U-7 seam.
//   U-Z    bring-up integration test
//
// Per the Plan 9 native-vs-ported split (ARCHITECTURE.md section 3.5):
// `ut` is a native libthyla-rs binary. NOT a Pouch port. NO musl.
// NO POSIX shim. Its panic handler + _start come from libthyla-rs;
// its allocator is libthyla-rs's ThylaAlloc.
//
// Input substrate (U-6g): `ut` reads fd 0 -- the cons handle on a real
// console (login spawns `ut` with Stdio::Inherit over the SYS_CONSOLE_OPEN
// handle) or a pipe (the CI login E2E). The blocking byte read is the
// login `read_line` precedent. When spawned bare by joey's boot check,
// `ut` has no fd 0 (rfork RFPROC gives a fresh empty handle table), so the
// first read returns an error -> `ut` prints its banner and exits 0
// cleanly (the boot check verifies status == 0; the banner goes to the
// UART via t_putstr regardless of fd 1).

#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;
use libthyla_rs::alloc::ThylaAlloc;
use libthyla_rs::io::{self, Read};
use libthyla_rs::poll::{PollEvents, PollSet, PollTimeout};
use libthyla_rs::t_putstr;
use libutopia::repl::Repl;
use libutopia::{ansi, palette, GLYPH};

#[global_allocator]
static GLOBAL_ALLOCATOR: ThylaAlloc = ThylaAlloc;

/// The current `ut` version. Bumped at every U-* chunk that expands the
/// shell's surface. `0.7-dev` == U-7a: the U-6 evaluator arc (read-parse-eval
/// REPL; built-ins; pipes; redirection; substitution) plus background jobs
/// (`&` + the job table + prompt-cycle `[N]+ Done` reaping).
const UT_VERSION: &str = "0.7-dev";

/// Emit the Pale Fire version banner. Three composed segments per
/// UTOPIA-VISUAL.md section 3: the glyph-orange right-tack, white version
/// text, path-blue tagline. Each coloured segment self-resets (ansi::fg),
/// so colour cannot bleed into subsequent output. Goes to the UART via
/// t_putstr (not fd 1), so it shows even when `ut` has no inherited stdout.
fn print_banner() {
    let mut banner = String::new();
    banner.push_str(&ansi::fg(palette::Role::Glyph, GLYPH));
    banner.push_str(" ut v");
    banner.push_str(UT_VERSION);
    banner.push_str(" -- ");
    banner.push_str(&ansi::fg(palette::Role::Path, "Thylacine textual shell"));
    banner.push('\n');
    t_putstr(&banner);
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    print_banner();

    let mut repl = Repl::new();
    let mut out = io::stdout();
    // Draw the first prompt (no-op to the UART if fd 1 is absent).
    repl.draw_prompt(&mut out);

    // The poll()+read main loop. At v1.0 the only polled fd is stdin
    // (fd 0). /dev/cons has no .poll hook (always-ready), so poll() is a
    // no-op wrapper there and the blocking read is what waits; on a pipe
    // poll() is real readiness. U-7 extends THIS PollSet with the notes
    // fds for job control; U-PTY makes cons pollable + adds line
    // discipline. PollSet::add extracts the raw fd, so the borrow ends at
    // the call -- the read below uses a fresh (zero-sized) Stdin handle.
    let mut poll = PollSet::new();
    poll.add(&io::stdin(), PollEvents::READ);

    let mut buf = [0u8; 256];
    let exit_code: i32 = loop {
        // A poll error (e.g. no fd 0 on the bare-spawn boot check) is
        // ignored; the read below then surfaces the same EOF/error and
        // breaks. This cannot spin: read() returns Ok(0)/Err -> break, or
        // Ok(n>0) -> progress.
        let _ = poll.poll(PollTimeout::Block);
        match io::stdin().read(&mut buf) {
            Ok(0) | Err(_) => break repl.exit_code(),
            Ok(n) => {
                if let Some(code) = repl.feed(&buf[..n], &mut out) {
                    break code;
                }
            }
        }
    };

    exit_code as i64
}
