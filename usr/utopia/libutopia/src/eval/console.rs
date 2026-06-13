// libutopia::eval::console -- the console line-discipline vocabulary + the
// raw-mode foreground-child dance (LS-7 / Kaua T-4).
//
// ut owns the session's `/dev/consctl` fd PRIVATELY (#94-B-b): login forwards it,
// ut establishes its prompt mode through it (`Repl::console_apply_default`), and
// -- here -- flips the console to RAW around a full-screen TUI child (nora) and
// back. The child NEVER touches consctl and is NEVER console-attached; ut stays
// the sole consctl writer, so I-27 (the console-ATTACH / SAK elevation gate) is
// untouched. This is line discipline, not the console capability: console-OWNER
// (the Ctrl-C target / the shell that may set the discipline) is distinct from
// console-ATTACH (the SAK / elevation anchor) per ARCH 17.1.
//
// THE DANCE (stmt::exec_external, the foreground-external spawn site):
//   1. set RAW (-isig, so Ctrl-C is a raw 0x03 keystroke the child reads, NOT an
//      `interrupt` note that would terminate it) BEFORE the spawn, so the child's
//      first read already sees raw bytes.
//   2. spawn with stdin/stdout/stderr Inherit -- the child gets the console
//      directly (a TUI reads fd 0 + draws fd 1). The normal path leaves stdin
//      Piped-drop (an ordinary foreground child does not read the console).
//   3. a plain by-pid wait (NOT the interruptible one): with ISIG off the kernel
//      posts no `interrupt` for this child, and forwarding a note to a raw TUI
//      would be wrong -- the child owns the console until it exits.
//   4. on the child's exit OR death, restore ut's PROMPT mode + re-emit the
//      screen-restore escapes to fd 1 -- the CRASH BACKSTOP. `no_std` apps run
//      `panic = abort`, so a crashed TUI's `Terminal::Drop` does NOT run; without
//      ut's restore the console would be wedged in the alt-screen with a hidden
//      cursor and a non-echoing, raw line discipline.

/// ut's prompt-mode line discipline (the ABSOLUTE consctl form -- every flag
/// named, so the result is independent of the prior state): raw byte-at-a-time
/// (no canonical line assembly), no kernel echo (the line editor draws its own),
/// ISIG so Ctrl-C cooks to the `interrupt` note the shell services, no CR/NL
/// translation (the editor handles CR). This is BOTH the mode
/// `Repl::console_apply_default` establishes at startup AND the mode ut restores
/// after a raw child -- one vocabulary, no drift.
pub(crate) const PROMPT_MODE: &[u8] = b"-icanon -echo +isig -icrnl -onlcr";

/// Full-raw line discipline for a TUI child: byte-identical to `PROMPT_MODE`
/// except ISIG is OFF, so Ctrl-C (0x03) reaches the child as a raw byte it reads
/// and interprets rather than being cooked into an `interrupt` note that would
/// terminate it. No-echo / no-canon / no-CR-NL all still hold (the child draws
/// its own screen). The ONLY delta from the prompt mode is the `isig` sign --
/// the dance is a single-bit flip and its restore.
pub(crate) const RAW_MODE: &[u8] = b"-icanon -echo -isig -icrnl -onlcr";

/// The screen-restore escape sequence ut re-emits to fd 1 after a raw child exits
/// or dies. BYTE-IDENTICAL to `kaua::term::Terminal::leave`'s output, in order:
/// RESET_SGR `\x1b[0m` + ENABLE_AUTOWRAP `\x1b[?7h` + SHOW_CURSOR `\x1b[?25h` +
/// LEAVE_ALT_SCREEN `\x1b[?1049l`. On a CLEAN exit the child's own
/// `Terminal::Drop` already emitted this (idempotent -- a second leave is inert);
/// on a CRASH (`panic = abort`, no Drop) this is the SOLE restore. libutopia does
/// not depend on kaua, so this is a hand-maintained cross-crate mirror: if
/// `kaua::term::Terminal::leave` ever changes its escapes or their order, THIS
/// must change in lockstep (the same drift-mirror discipline as the syscall-number
/// tables).
pub(crate) const RESTORE_SCREEN: &[u8] = b"\x1b[0m\x1b[?7h\x1b[?25h\x1b[?1049l";

/// Write an absolute consctl mode command to `fd` (the kernel applies one write
/// atomically -- the TCSAFLUSH discipline, `cons_set_mode_cmd`). Best-effort:
/// returns true iff the whole command was accepted (n == len). A bad fd / a
/// pre-LS-8b kernel rejects the I/O -> false -> the caller proceeds without
/// driving the discipline (no regression: the console keeps whatever mode it was
/// in).
pub(crate) fn set_mode(fd: i32, cmd: &[u8]) -> bool {
    // SAFETY: t_write is the SYS_WRITE SVC wrapper; cmd is a valid byte slice and
    // fd is the caller's consctl fd (or any fd -- a write to a bad fd just fails).
    let w = unsafe { libthyla_rs::t_write(fd as i64, cmd.as_ptr(), cmd.len()) };
    w == cmd.len() as i64
}

/// Emit the screen-restore escapes to fd 1 (the crash backstop). Best-effort: a
/// failed write is unrecoverable here and the next prompt redraw repaints anyway.
pub(crate) fn restore_screen() {
    // SAFETY: t_write is the SYS_WRITE SVC wrapper; RESTORE_SCREEN is a valid
    // static byte slice and fd 1 is the shell's console output.
    let _ = unsafe { libthyla_rs::t_write(1, RESTORE_SCREEN.as_ptr(), RESTORE_SCREEN.len()) };
}

/// Whether `argv0` names a full-screen TUI child that needs the raw-mode dance.
/// Matches on the BASENAME so `/bin/nora` and a bare `nora` both qualify. v1.0
/// carries a fixed set (`nora`); a binary self-declaring its console needs (a
/// spawn flag or an on-disk manifest) is a recorded v1.x seam (KAUA.md) -- until
/// then a name a user gives their own non-TUI binary that collides with this set
/// is a known limitation.
pub fn is_raw_command(argv0: &str) -> bool {
    let base = argv0.rsplit('/').next().unwrap_or(argv0);
    matches!(base, "nora")
}

// NB: like every libutopia `#[cfg(test)]` module, these are host-unrunnable
// today (the crate is unconditionally `#![no_std]`, so `cargo test` cannot build
// a std test harness) -- they document the contract; the RUNNABLE coverage is the
// `ls-7` LS-CI (the whole dance in QEMU: is_raw_command positive + the mode flips
// + the restore) plus every other scenario (cat/echo via the normal path = the
// is_raw_command negative).
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mode_strings_are_the_pinned_consctl_forms() {
        // The dance is a single-bit flip: PROMPT carries `+isig`, RAW `-isig`,
        // every other flag identical -- if these diverge further the dance would
        // change more than the signal-cooking bit.
        assert_eq!(PROMPT_MODE, b"-icanon -echo +isig -icrnl -onlcr");
        assert_eq!(RAW_MODE, b"-icanon -echo -isig -icrnl -onlcr");
    }

    #[test]
    fn restore_screen_is_the_kaua_leave_sequence() {
        // RESET_SGR + ENABLE_AUTOWRAP + SHOW_CURSOR + LEAVE_ALT_SCREEN, in order.
        // Mirrors kaua::term::Terminal::leave (libutopia cannot depend on kaua, so
        // this literal is the drift guard the audit cross-checks against term.rs).
        assert_eq!(RESTORE_SCREEN, b"\x1b[0m\x1b[?7h\x1b[?25h\x1b[?1049l");
    }

    #[test]
    fn is_raw_command_matches_nora_by_basename() {
        assert!(is_raw_command("nora"));
        assert!(is_raw_command("/bin/nora"));
        assert!(is_raw_command("/usr/local/bin/nora"));
        // Ordinary externals stay on the normal spawn path.
        assert!(!is_raw_command("cat"));
        assert!(!is_raw_command("/bin/ut"));
        assert!(!is_raw_command("noragami")); // basename must match exactly
        assert!(!is_raw_command("nora.bak"));
        assert!(!is_raw_command(""));
    }
}
