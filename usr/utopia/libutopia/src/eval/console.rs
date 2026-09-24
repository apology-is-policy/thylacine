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

// The vocabulary itself lives in the pure sibling `discipline`, which host
// tests can reach; this module is the part that writes to an fd.
pub use super::discipline::{is_console_passthrough, is_raw_command};
pub(crate) use super::discipline::{CHILD_MODE, PROMPT_MODE, RAW_MODE, RESTORE_SCREEN};

/// Write an absolute consctl mode command to `fd` (the kernel applies one write
/// atomically -- `cons_set_mode_cmd`; a write clearing ICANON DELIVERS the pending
/// canonical line to the reader, PTY-DESIGN "Mode writes deliver, never
/// discard" -- which is what makes type-ahead across a job's end survive the
/// PROMPT_MODE re-arm). Best-effort:
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

// === PTY-4b: pts detection (the session-dance trigger) ===

/// The ptyfs endpoint-qid contract (PTY-DESIGN section 5, the documented
/// ptsname ABI): `PTS_FLAG | N<<8 | filekind`, filekind 1 = master, 2 = slave.
const PTS_QID_FLAG: u64 = 1 << 40;
const PTS_FK_SLAVE: u64 = 2;

/// If fd 0 is a pts SLAVE, return its index `N`. The same TWO-GATE
/// discrimination the PTY-3 pouch dispatcher uses, native side: S_ISCHR
/// FIRST (ptyfs reports `S_IFCHR` for its endpoints; netd's `/net` qids also
/// carry bit 40 but report `S_IFREG`, so the mode gate keeps a socket-backed
/// fd 0 out), THEN the qid flag + filekind. `None` on the console (devcons
/// has no `stat_native` -> fstat fails), a pipe, a file, or a pts MASTER --
/// every non-hosted case, so the caller's dance is skipped and the shell
/// runs its console path unchanged.
pub(crate) fn pts_slave_n_of_fd0() -> Option<u32> {
    let mut st = [0u8; 88]; // #100: t_stat ABI is 88 bytes (kernel copies out sizeof(t_stat))
    // SAFETY: t_fstat is the SYS_FSTAT SVC wrapper; st is a valid 88-byte
    // t_stat buffer (the ABI-pinned size).
    if unsafe { libthyla_rs::t_fstat(0, st.as_mut_ptr()) } != 0 {
        return None;
    }
    let mut w = [0u8; 4];
    w.copy_from_slice(&st[40..44]); // t_stat.mode @40
    let mode = u32::from_le_bytes(w);
    if (mode & 0o170000) != 0o020000 {
        return None; // not a character device
    }
    let mut q = [0u8; 8];
    q.copy_from_slice(&st[8..16]); // t_stat.qid_path @8
    let qid = u64::from_le_bytes(q);
    if qid & PTS_QID_FLAG == 0 || (qid & 0xff) != PTS_FK_SLAVE {
        return None;
    }
    Some(((qid >> 8) & 0xff_ffff) as u32)
}

// NB: like every libutopia `#[cfg(test)]` module, these are host-unrunnable
// today (the crate is unconditionally `#![no_std]`, so `cargo test` cannot build
// a std test harness) -- they document the contract; the RUNNABLE coverage is the
// `ls-7` LS-CI (the whole dance in QEMU: is_raw_command positive + the mode flips
// + the restore) plus every other scenario (cat/echo via the normal path = the
// is_raw_command negative).
