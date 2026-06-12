// ut -- the Utopia shell.
//
// Two letters; mnemonic for Utopia (UTOPIA-SHELL-DESIGN.md section
// 13.1). Through U-6g `ut` is a working read-parse-eval REPL: it draws
// the Pale Fire prompt, reads input from fd 0, drives the libutopia line
// editor (U-4) + parser (U-5) + evaluator (U-6), and loops. The U-* arc
// still fills in:
//
//   U-7    fd-notes job control (Ctrl-C / Ctrl-Z / & / jobs / fg / bg).
//   U-8    Thylacine builtins (bind / mount / unmount / pivot_root /
//          rfork / cap / note)
//   U-9..N native coreutils, one or two per chunk
//   LS-8   the line-discipline substrate: LS-8a made /dev/cons pollable
//          (a `.poll` hook + the deferred poll-wake), LS-8b added termios
//          via /dev/consctl, and LS-8c (HERE) is the MULTI-fd poll loop --
//          the shell polls /dev/cons AND its own note fd together, so a
//          finished bg job / an idle Ctrl-C is serviced asynchronously
//          (UTOPIA-SHELL-DESIGN.md section 10.2). The PTY master/slave pair
//          (/dev/ptmx + /dev/pts/<n>) + per-Proc fd 0/1/2 stay Phase-8.
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
use libthyla_rs::poll::{AsFd, PollEvents, PollSet, PollTimeout};
use libthyla_rs::t_putstr;
use libutopia::repl::Repl;
use libutopia::{ansi, palette, GLYPH};

#[global_allocator]
static GLOBAL_ALLOCATOR: ThylaAlloc = ThylaAlloc;

/// The current `ut` version. Bumped at every U-* chunk that expands the
/// shell's surface. `0.8-dev` == LS-8c: the multi-fd poll loop -- the shell
/// polls /dev/cons (made pollable by LS-8a) AND its own note fd together, so a
/// background job finishing or a Ctrl-C pressed while idle at the prompt is
/// serviced asynchronously (`[N]+ Done` mid-idle; reactive line-cancel
/// mid-edit) instead of waiting for the next Enter.
const UT_VERSION: &str = "0.8-dev";

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
    // LS-2: a session `ut` holds the console as fd 0/1/2 (login spawned it
    // with Stdio::Inherit), so external commands should inherit fd 1/2 -- their
    // output then reaches the terminal. The probe (a zero-length write to fd 1)
    // confirms a live writable stdout. A bare-spawned `ut` (the boot check) has
    // no fd 1 -> the probe is false and externals keep the Piped-then-drop
    // convention (and that `ut` exits before any spawn anyway: it breaks on the
    // fd-0 read error below).
    repl.set_stdio_inherit(io::stdout_is_live());
    let mut out = io::stdout();
    // Draw the first prompt (no-op to the UART if fd 1 is absent).
    repl.draw_prompt(&mut out);

    // LS-5 / Holotype RW-0 F1: open the shell's own note queue EAGERLY for a
    // real session. A session `ut` is the console OWNER (LS-5a), so until it is
    // self-managing (has opened its note queue) an uncaught `interrupt`
    // default-terminates it (LS-5b/c) -- a Ctrl-C at a FRESH prompt, before the
    // first keystroke, would terminate the shell and log the user out. Opening
    // the queue here makes the shell self-managing from its very first prompt.
    // `stdout_is_live()` (a zero-length write to fd 1) is the session-vs-boot-
    // check discriminator already used for set_stdio_inherit above: login hands
    // a session `ut` fd 0/1/2 together, so a live fd 1 implies a live fd 0 and
    // the note fd lands at 3+. The bare-spawn boot check has an EMPTY handle
    // table + no live fd 1, so we DON'T open (an eager open there would mint the
    // note fd as fd 0 and the first read would block on the note queue instead
    // of EOFing) -- and it breaks on the fd-0 read error below before ever
    // needing notes.
    if io::stdout_is_live() {
        repl.open_notes();
    }

    // LS-8c: the multi-fd poll loop. LS-8a made /dev/cons pollable (a `.poll`
    // hook + the deferred poll-wake relay), so the shell now polls stdin (fd 0)
    // AND its own note fd together: a finished background job (`child_exit`) or
    // a Ctrl-C while idle (`interrupt`) wakes the loop the instant it arrives
    // and is serviced WITHOUT waiting for the next Enter (`[N]+ Done` mid-idle;
    // reactive line-cancel mid-edit). Pre-LS-8c the single-fd loop blocked in
    // read() and notes were delivered only at the prompt cycle. A foreground
    // command's interrupt forwarding stays in `wait_pids_interruptible` (the
    // shell is not in THIS loop while a child runs).
    let cons_fd = io::stdin().as_raw_fd();
    let mut notes_fd = repl.notes_fd();
    let mut poll = PollSet::new();
    poll.add_raw(cons_fd, PollEvents::READ);
    if let Some(nfd) = notes_fd {
        poll.add_raw(nfd, PollEvents::READ);
    }

    let mut buf = [0u8; 256];
    let exit_code: i32 = loop {
        let mut cons_ready = false;
        let mut notes_ready = false;
        let mut notes_dead = false;
        match poll.poll(PollTimeout::Block) {
            Ok(results) => {
                for ev in results {
                    if ev.fd == cons_fd {
                        // Any cons event (READ / HUP / ERR) -> attempt the read,
                        // which resolves data (feed) or EOF/error (break). A bare
                        // HUP MUST set this or a closed stdin would spin the loop.
                        cons_ready = true;
                    } else if Some(ev.fd) == notes_fd {
                        if ev.is_readable() {
                            notes_ready = true;
                        } else {
                            // The shell's own note fd should never err/hangup; if
                            // it does, mark it dead (removed below, outside the
                            // `results` borrow) so a stuck error cannot spin the
                            // loop -- notes then degrade to sync-point delivery.
                            notes_dead = true;
                        }
                    }
                }
            }
            // A poll error (e.g. no fd 0 on the bare-spawn boot check, where the
            // set holds an invalid fd 0) -> fall through to the read, which
            // surfaces the same EOF/error and breaks. Cannot spin: the read
            // returns Ok(0)/Err -> break.
            Err(_) => cons_ready = true,
        }
        if notes_dead {
            // `results` (which borrowed `poll`) has dropped -- safe to mutate.
            if let Some(nfd) = notes_fd {
                poll.remove_raw(nfd);
            }
            notes_fd = None;
        }
        // Service the async note wake FIRST: a finished bg job (`[N]+ Done`) or
        // a reactive Ctrl-C (line-cancel). A simultaneous cons keystroke then
        // repaints over the notification via feed's editor render.
        if notes_ready {
            repl.on_notes_ready(&mut out);
        }
        if cons_ready {
            match io::stdin().read(&mut buf) {
                Ok(0) | Err(_) => break repl.exit_code(),
                Ok(n) => {
                    if let Some(code) = repl.feed(&buf[..n], &mut out) {
                        break code;
                    }
                }
            }
        }
    };

    exit_code as i64
}
