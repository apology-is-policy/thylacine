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
use alloc::vec::Vec;
use libthyla_rs::alloc::ThylaAlloc;
use libthyla_rs::env;
use libthyla_rs::io::{self, Read};
use libthyla_rs::poll::{AsFd, PollEvents, PollSet, PollTimeout};
use libthyla_rs::{
    t_close, t_mount, t_open, t_putstr, t_walk_create, t_walk_open, T_MREPL, T_OPATH, T_OREAD,
    T_WALK_CREATE_DMDIR, T_WALK_OPEN_FROM_ROOT,
};
use libutopia::repl::Repl;
use libutopia::{ansi, palette, GLYPH};

#[global_allocator]
static GLOBAL_ALLOCATOR: ThylaAlloc = ThylaAlloc;

/// The current `ut` version. Bumped at every U-* chunk that expands the
/// shell's surface. `0.9-dev` == the Phase-1 ergonomics: the session shell
/// starts in the user's home (`--home`), `cd` with no argument and the
/// `~`-abbreviated prompt resolve `$home`, and the `la`/`ll` aliases expand in
/// command position. (0.8-dev was LS-8c: the multi-fd poll loop.)
const UT_VERSION: &str = "0.9-dev";

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

/// #94-B-b: parse "--consctl-fd N" from argv -> the inherited consctl fd, or -1
/// if absent/malformed. login forwards its inherited /dev/consctl fd to ut via
/// `Command::inherit_fd` (so it lands at ut's fd 3) and passes its number here.
/// A bare-spawned ut (the boot check) is given no such arg -> -1 -> ut runs
/// unchanged. N is parsed generically (always 3 in the trusted login->ut chain).
fn parse_consctl_fd() -> i64 {
    let mut it = env::args().operands();
    while let Some(a) = it.next() {
        if a == b"--consctl-fd" {
            // Lower bound 3: a consctl fd is ALWAYS an inherited slot past the 3
            // stdio fds (the trusted chain hands it at fd 3), so 0/1/2 is never
            // valid. Rejecting them keeps a stray `--consctl-fd 1` from writing
            // the mode string to a stdio fd as raw bytes + a FALSE `ut: consctl
            // ok` witness (audit F3); a value below the stdio range is malformed.
            return match it.next().and_then(parse_u64) {
                Some(v) if (3..=i64::MAX as u64).contains(&v) => v as i64,
                _ => -1,
            };
        }
    }
    -1
}

/// #113: parse "--home PATH" from argv -> the session user's home directory, or
/// None if absent. login forwards it (`--home /home/<user>`) because there is no
/// kernel envp to carry $HOME; a bare-spawned `ut` (the boot check) is given no
/// such arg -> None -> the shell runs at "/" unchanged.
fn parse_home_arg() -> Option<String> {
    let mut it = env::args().operands();
    while let Some(a) = it.next() {
        if a == b"--home" {
            return it
                .next()
                .and_then(|p| core::str::from_utf8(p).ok())
                .map(String::from);
        }
    }
    None
}

fn parse_u64(s: &[u8]) -> Option<u64> {
    if s.is_empty() {
        return None;
    }
    let mut v: u64 = 0;
    for &c in s {
        if !c.is_ascii_digit() {
            return None;
        }
        v = v.checked_mul(10)?.checked_add((c - b'0') as u64)?;
    }
    Some(v)
}

/// D2: parse argv for a script-file operand -- the first operand that is not a
/// recognized flag (`--consctl-fd N` / `--home PATH`, whose values are
/// skipped). Returns `(script_path, script_args)` for `ut SCRIPT [args...]`, or
/// `None` for an interactive session (login spawns `ut` with only flags; the
/// bare-spawn boot check has no operands). A `#!/bin/ut` shebang spawn invokes
/// `ut <script> <args...>` with no flags, so the script is the first operand.
fn parse_script() -> Option<(String, Vec<String>)> {
    let mut it = env::args().operands();
    while let Some(a) = it.next() {
        match a {
            b"--consctl-fd" | b"--home" => {
                // A flag that takes a value; skip the value too.
                let _ = it.next();
            }
            _ => {
                let path = String::from(core::str::from_utf8(a).ok()?);
                let mut args: Vec<String> = Vec::new();
                while let Some(rest) = it.next() {
                    if let Ok(s) = core::str::from_utf8(rest) {
                        args.push(String::from(s));
                    }
                }
                return Some((path, args));
            }
        }
    }
    None
}

/// D2: run a script file non-interactively then return its exit status. Reads
/// the whole file, builds a fresh `Repl` in script mode, and evaluates it. A
/// child command inherits the console if one is held (a shebang-spawned `ut`
/// inherits its parent shell's fd 1/2). An unreadable script is a 127 (the
/// shell "command not found" convention).
fn run_script_file(path: &str, args: &[String]) -> i64 {
    let src = match read_file(path) {
        Some(s) => s,
        None => {
            let mut m = String::from("ut: cannot open script: ");
            m.push_str(path);
            m.push('\n');
            t_putstr(&m);
            return 127;
        }
    };
    let mut repl = Repl::new();
    repl.set_stdio_inherit(io::stdout_is_live());
    // A direct `ut --home <path> script.ut` honors --home; a shebang spawn
    // passes none, leaving $home unset (fine for a script).
    if let Some(home) = parse_home_arg() {
        repl.set_home(home);
    }
    repl.run_script(path, args, &src) as i64
}

/// Slurp a file to a String (best-effort). `None` on open/read failure.
fn read_file(path: &str) -> Option<String> {
    let mut f = libthyla_rs::fs::File::open(path).ok()?;
    let mut s = String::new();
    f.read_to_string(&mut s).ok()?;
    Some(s)
}

/// Per-user /tmp (Go Stage 6): mkdir `<home>/tmp` and bind it MREPL over
/// `/tmp` in the shell's OWN namespace -- inherited by every session child.
///
/// Why here and not login: the global /tmp rides the shared SYSTEM-attach
/// system mount, where a create's OWNER is the attach identity (9P identity
/// is per-Tattach; only the gid travels per-op) -- so a user's mode-0700
/// `MkdirTemp` under the global /tmp (Go's `$WORK`; any POSIX `mkdtemp`)
/// landed SYSTEM-owned and the user could not create inside it (a bare
/// `go build` failed `mkdir $WORK/b0NN: operation not permitted`). And
/// login runs as PRINCIPAL_SYSTEM, which cannot create (or even X-search)
/// inside the 0700 user-owned home -- only the shell, born AS the user,
/// can. The namespace bind is the Plan 9 answer: a private, user-owned,
/// encrypted-at-rest tmp with no env knob, covering hardcoded-/tmp
/// consumers too. Mounts are per-Proc territory ops (unprivileged); ut's
/// territory is a private snapshot clone, so the bind scopes exactly to
/// the session subtree and dies with it. Best-effort: a failure is logged
/// and the session proceeds (tools needing a writable /tmp degrade).
fn bind_user_tmp(home: &str) {
    unsafe {
        let hd = t_open(
            T_WALK_OPEN_FROM_ROOT,
            home.as_ptr(),
            home.len(),
            T_OPATH,
        );
        if hd < 0 {
            t_putstr("ut: tmp bind: home open failed\n");
            return;
        }
        let cf = t_walk_create(hd, b"tmp".as_ptr(), 3, T_OREAD, T_WALK_CREATE_DMDIR | 0o755);
        if cf >= 0 {
            let _ = t_close(cf);
        }
        let td = t_walk_open(hd, b"tmp".as_ptr(), 3, T_OPATH);
        let _ = t_close(hd);
        if td < 0 {
            t_putstr("ut: tmp bind: home tmp mkdir failed\n");
            return;
        }
        let rc = t_mount(b"/tmp".as_ptr(), 4, td, T_MREPL);
        let _ = t_close(td); // t_mount holds its own ref on the source Spoor
        if rc == 0 {
            t_putstr("ut: tmp bound (per-user /tmp from the home)\n");
        } else {
            t_putstr("ut: tmp bind: mount over /tmp failed\n");
        }
    }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // D2: `ut SCRIPT [args...]` -- non-interactive script execution. A script
    // path operand (the first non-flag arg; e.g. from a `#!/bin/ut` shebang
    // spawn, or a direct `ut prog.ut`) runs the file then exits with its
    // status -- and prints NO version banner (a script is not an interactive
    // session). login spawns `ut` with only flags -> no script -> the
    // interactive REPL below; the bare-spawn boot check has no operands ->
    // also interactive (and still prints the banner).
    if let Some((path, args)) = parse_script() {
        return run_script_file(&path, &args);
    }

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

    // #94-B-b: a session `ut` receives login's inherited /dev/consctl fd
    // (--consctl-fd N), so the SHELL -- not login -- owns the console line
    // discipline for the session. Establish ut's prompt mode (raw: the line
    // editor draws its own echo; ISIG so Ctrl-C is the `interrupt` note ut
    // services) through it, and on success emit the boot-log witness that the
    // inherited-fd consctl reach extends to a user-identity shell (the #94-B end
    // of the gate drop; the hard regression is the kernel `devdev.cons_gate`
    // test). The fd is held on the Repl for LS-7's foreground-child mode dance.
    // A bare-spawned `ut` parses -1 and runs unchanged.
    let consctl_fd = parse_consctl_fd();
    if consctl_fd >= 0 {
        repl.set_consctl_fd(consctl_fd as i32);
        if repl.console_apply_default() {
            t_putstr("ut: consctl ok (console line discipline via inherited fd)\n");
        } else {
            t_putstr("ut: consctl unavailable (mode-set rejected)\n");
        }
    }

    // #113: a session ut learns its home from login (--home <path>) -- set $home
    // and chdir into it, so the shell starts in the user's home and the first
    // prompt below already shows ~ (a bare-spawned ut gets no --home: skipped,
    // runs at /). Before draw_prompt so the initial prompt reflects the home.
    if let Some(home) = parse_home_arg() {
        bind_user_tmp(&home);
        repl.set_home(home);
    }

    // #115a: install namespace-driven Tab completion for a real session -- it
    // scans /bin (the #58 exec namespace) for the external-command set. Gated on
    // a live console (session-only), like open_notes below: the bare-spawn boot
    // check has no session namespace + exits on the fd-0 read before the loop.
    if io::stdout_is_live() {
        repl.install_completion();
        // #115b: load + enable ~/.ut_history persistence (after set_home, so
        // $home resolves the path). Session-only, same gate as completion.
        repl.install_history();
    }

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
