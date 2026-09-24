// libutopia::eval::discipline -- the console line-discipline VOCABULARY.
//
// Split out of `console` 2026-09-22 for one reason: everything here is pure,
// and `console` is not. That module makes three `t_write`/`t_fstat` calls, so
// it sits behind the crate's `backend` feature and cannot be built for a host
// target -- which meant `is_raw_command`, a hardcoded allowlist deciding
// whether a child gets the raw-mode dance, had tests that ran on no machine at
// all. The module even said so, and worked around it locally with
// compile-time asserts mirroring its `#[cfg(test)]` ones.
//
// A pure module is the fix rather than more mirrors: these assertions now
// EXECUTE. The `const _: () = assert!(...)` guards are kept anyway -- they fire
// on the device build, where a host test cannot, so the two cover different
// machines rather than the same one twice.
//
// What belongs here: the mode strings, the screen-restore sequence, and the
// name predicates. What does NOT: anything that writes an fd. If a function
// here ever needs a syscall it belongs back in `console`.

/// ut's prompt-mode line discipline (the ABSOLUTE consctl form -- every flag
/// named, so the result is independent of the prior state): raw byte-at-a-time
/// (no canonical line assembly), no kernel echo (the line editor draws its own),
/// ISIG so Ctrl-C cooks to the `interrupt` note the shell services, no INPUT
/// CR->NL translation (the editor handles CR itself) -- but OUTPUT NL->CR-NL
/// (`+onlcr`) STAYS ON: output post-processing is orthogonal to raw input
/// (POSIX splits them for exactly this reason), and a console-direct session's
/// children (`ls` etc.) write plain `\n` line endings that only the kernel's
/// ONLCR arm can cook. The pre-fix `-onlcr` sent bare LF to the wire for the
/// whole session; QEMU's `mon:stdio` mux silently re-inserted CRs for every
/// terminal viewer, so the gap stayed invisible until the first honest raw
/// renderer (the Aurora fbcon) drew the ls staircase. The editor's own output
/// is ONLCR-proof either way: its explicit `\r`+CSI redraws pass through
/// untouched, and its deliberate `\n`s WANT the implied carriage return.
/// This is BOTH the mode `Repl::console_apply_default` establishes at startup
/// AND the mode ut restores after a raw child -- one vocabulary, no drift.
pub(crate) const PROMPT_MODE: &[u8] = b"-icanon -echo +isig -icrnl +onlcr";

/// Full-raw line discipline for a TUI child: like `PROMPT_MODE` but ISIG is
/// OFF, so Ctrl-C (0x03) reaches the child as a raw byte it reads and
/// interprets rather than being cooked into an `interrupt` note that would
/// terminate it -- AND `onlcr` is OFF: a fullscreen TUI positions with
/// explicit CSI sequences and owns every byte it emits; translating a
/// deliberately-bare LF behind its back would be the driver editorializing.
/// Two deltas from the prompt mode (`isig`, `onlcr`), both restored together.
pub(crate) const RAW_MODE: &[u8] = b"-icanon -echo -isig -icrnl -onlcr";

/// PTY-4b: the COOKED line discipline a job-control shell gives an ordinary
/// (non-TUI) foreground job on a pts -- the POSIX terminal default a ported
/// or line-oriented child expects (canonical line assembly, kernel echo,
/// ISIG signal cooking, CR/NL translation both ways; byte-identical to the
/// ptyfs cooked default, PTY-2b). The real-shell discipline: raw is the LINE
/// EDITOR's mode, cooked is the CHILD's -- bash/readline restore the tty to
/// cooked around every foreground job and re-raw it at the next prompt. Only
/// the jc path uses this (the console session never hands children the
/// console input, so its children see no discipline at all).
pub(crate) const CHILD_MODE: &[u8] = b"+icanon +echo +isig +icrnl +onlcr";

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

/// Compile-time byte-slice equality (`==` on `&[u8]` is not `const`).
const fn bytes_eq(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    let mut i = 0;
    while i < a.len() {
        if a[i] != b[i] {
            return false;
        }
        i += 1;
    }
    true
}

// The #106-F3 drift guards, as COMPILE-TIME asserts that fire on the no_std
// device build -- libutopia has no host test harness (the crate is unconditionally
// `#![no_std]`), so the `#[cfg(test)]` literal asserts below never run. These do.
// PROMPT/RAW differ by isig + onlcr (see each doc); RESTORE_SCREEN is the
// cross-crate mirror of `kaua::term::Terminal::leave`, pinned to the SAME literal
// on the kaua side by `kaua::encode::tests::restore_screen_is_the_pinned_sequence`,
// so a drift on either side fails its own build. login's MODE_DEFAULT carries the
// matching PROMPT-mode assert (it must equal PROMPT_MODE so the login->ut
// boundary is flat).
const _: () = assert!(bytes_eq(PROMPT_MODE, b"-icanon -echo +isig -icrnl +onlcr"));
const _: () = assert!(bytes_eq(RAW_MODE, b"-icanon -echo -isig -icrnl -onlcr"));
const _: () = assert!(bytes_eq(CHILD_MODE, b"+icanon +echo +isig +icrnl +onlcr"));
const _: () = assert!(bytes_eq(RESTORE_SCREEN, b"\x1b[0m\x1b[?7h\x1b[?25h\x1b[?1049l"));

/// Whether `argv0` names a full-screen TUI child that needs the raw-mode dance.
/// Matches on the BASENAME so `/bin/nora` and a bare `nora` both qualify. v1.0
/// carries a fixed set (`nora`, `ptyhost`, `prowl`, `quarry`, `lantern` -- the
/// PTY-4 session host wants the outer console as a raw byte pipe, so the pts it
/// hosts is the one line discipline; `prowl` is the full-screen process
/// monitor; `quarry` is the GPU demo-bench launcher; `lantern` is the Beacon
/// deck presenter); a binary self-declaring its console needs (a spawn flag or
/// an on-disk manifest) is a recorded v1.x seam (KAUA.md) -- until then a name
/// a user gives their own non-TUI binary that collides with this set is a known
/// limitation.
///
/// `lantern` is a member for the INPUT half only: it needs `-icanon -echo`
/// (a keystroke is a keystroke) and `-isig` (so its own `q`/Ctrl-C handling
/// runs, and a Ctrl-C does not terminate a talk through a note it never sees).
/// It is NOT a full-screen TUI and never enters the alt-screen -- doing so
/// would make a Halcyon tile paint its raw mono grid instead of the rich
/// document lantern exists to show -- so `RAW_MODE`'s `-onlcr` leaves it
/// cooking its own line endings (`lantern::cook`). The screen backstop this
/// dance re-emits on exit is harmless to it: every escape in `RESTORE_SCREEN`
/// is idempotent, and leaving an alt-screen never entered is inert.
pub fn is_raw_command(argv0: &str) -> bool {
    let base = argv0.rsplit('/').next().unwrap_or(argv0);
    matches!(base, "nora" | "ptyhost" | "prowl" | "quarry" | "lantern")
}

/// Whether `argv0` names a console-PASSTHROUGH wrapper: a thin foreground command
/// that launches an interactive SUB-SHELL and so must be handed the console
/// (Inherit fd 0/1/2) instead of the console path's Piped-drop stdin -- otherwise
/// the sub-shell inherits a write-closed pipe on fd 0 and exits at birth on EOF
/// (IM-4). Distinct from `is_raw_command`: the child is a shell, NOT a full-screen
/// TUI, so it keeps the outer shell's PROMPT discipline (`+onlcr` for its
/// children's output; `-icanon -echo` for its own line editor; `+isig` so the ut
/// prompt read still cooks Ctrl-C to the `interrupt` note the shell services) --
/// the RAW-mode dance (`-isig -onlcr`) would be wrong twice over. Matches on the
/// BASENAME so `/bin/imperium` and a bare `imperium` both qualify. v1.0 carries a
/// single member (`imperium`, the elevation sub-shell); the same self-declaring-
/// binary seam noted on `is_raw_command` (a spawn flag / on-disk manifest) applies.
pub fn is_console_passthrough(argv0: &str) -> bool {
    let base = argv0.rsplit('/').next().unwrap_or(argv0);
    matches!(base, "imperium")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mode_strings_are_the_pinned_consctl_forms() {
        // PROMPT vs RAW differ by exactly isig (signal cooking stays with the
        // shell) and onlcr (output cooking ON at the prompt for children's
        // plain-\n writes; OFF for a TUI that owns every byte) -- if these
        // diverge further the dance would change more than those two bits.
        assert_eq!(PROMPT_MODE, b"-icanon -echo +isig -icrnl +onlcr");
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
        // PTY-4c: the session host wants the outer console raw (a byte pipe).
        assert!(is_raw_command("ptyhost"));
        assert!(is_raw_command("/bin/ptyhost"));
        // prowl-2: the full-screen process monitor (Kaua TUI).
        assert!(is_raw_command("prowl"));
        assert!(is_raw_command("/bin/prowl"));
        // quarry: the GPU demo-bench launcher (Kaua TUI).
        assert!(is_raw_command("quarry"));
        assert!(is_raw_command("/bin/quarry"));
        // lantern: the Beacon deck presenter -- the raw INPUT half only (it
        // never enters the alt-screen; see is_raw_command's note).
        assert!(is_raw_command("lantern"));
        assert!(is_raw_command("/bin/lantern"));
        // Ordinary externals stay on the normal spawn path.
        assert!(!is_raw_command("cat"));
        assert!(!is_raw_command("/bin/ut"));
        assert!(!is_raw_command("noragami")); // basename must match exactly
        assert!(!is_raw_command("nora.bak"));
        assert!(!is_raw_command(""));
    }

    #[test]
    fn is_console_passthrough_matches_imperium_by_basename() {
        // IM-4: the elevation sub-shell wrapper needs Inherit stdin, PROMPT
        // discipline (NOT the RAW dance).
        assert!(is_console_passthrough("imperium"));
        assert!(is_console_passthrough("/bin/imperium"));
        assert!(!is_console_passthrough("imperiumx")); // basename must match exactly
        assert!(!is_console_passthrough("ut"));
        assert!(!is_console_passthrough(""));
        // The two console-child sets are DISJOINT: a raw TUI is never a
        // passthrough wrapper and vice versa (the exec_external dispatch relies
        // on this -- it checks is_raw_command first, then is_console_passthrough).
        assert!(!is_console_passthrough("nora"));
        assert!(!is_raw_command("imperium"));
    }
}
