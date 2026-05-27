// libthyla-rs::process — process spawn + wait.
//
// `Command` is a builder for a child-process spawn (mirror of
// std::process::Command). `Child` is the handle for the spawned
// process; `Child::wait` reaps via SYS_WAIT_PID and returns an
// `ExitStatus`. `Stdio` controls the child's stdin/stdout/stderr.
// `pipe()` creates a connected reader/writer pair (the substrate for
// stdio plumbing AND for explicit inter-process byte-streams).
//
// Foundation chunk: U-2d per docs/UTOPIA-SHELL-DESIGN.md §15.
//
// V1.0 SCOPE:
//   - SYS_SPAWN_FULL_ARGV: name + argv + cap_mask + perm_flags + a
//     positional fd_list (0..MAX_FDS). At v1 we use the fd_list to
//     express stdin/stdout/stderr (always exactly 3 entries -- the
//     positional convention every POSIX-shaped tool expects).
//   - Spawn looks up the binary in devramfs OR the pivoted root,
//     same as the kernel's SYS_SPAWN_FULL_ARGV lookup -- callers pass
//     a bare name (no slashes) per the SYS_SPAWN_NAME_MAX constraint.
//   - Inherited cap_mask defaults to the caller's full caps (`!0u64`);
//     the kernel intersects with parent->caps so a child cannot gain
//     capabilities the parent doesn't hold.
//
// STDIO MODES:
//   - `Stdio::Inherit` — child gets parent's same-position fd (0/1/2).
//   - `Stdio::Piped`   — spawn creates a pipe; child gets the
//     relevant end; parent retains the other end in Child::stdin /
//     stdout / stderr. Parent's copy of the child's end is closed
//     after spawn so the child sees EOF on close.
//   - `Stdio::File(f)` — child gets f.as_raw_fd(); the parent's File
//     is held alive through the syscall (so the fd stays valid for
//     the kernel to refcount-bump on inheritance), then closed when
//     Command::spawn returns.
//   - `Stdio::Null`    — NOT IMPLEMENTED at v1 (no kernel /dev/null
//     analog whose fd we can supply without an extra open).
//
// DEFERRED:
//   - `env` / environment variables: SYS_SPAWN_FULL_ARGV's `_pad_envp`
//     field is reserved for envp pass-through but rejected non-zero at
//     v1.0. Until the envp surface lands, environment is inherited
//     wholesale (no per-Command override).
//   - `current_dir`: SYS_CHROOT exists but is a Territory-wide
//     operation; per-spawn cwd needs a different surface. v1.x.
//   - Status decoding beyond `success() == (status == 0)` and
//     `code() == Some(status)`. Signal-terminated processes are
//     surfaced via t::notes (U-2e); status decode that distinguishes
//     them lands then.

use crate::err::{Error, Result};
use crate::fs::File;
use crate::handle::{Handle, Rights};
use crate::{
    t_pipe, t_spawn_full_argv, t_wait_pid, TSpawnArgs, T_SPAWN_NAME_MAX,
    T_SYS_SPAWN_ARGV_DATA_MAX, T_SYS_SPAWN_ARGV_MAX,
};
use alloc_crate::string::String;
use alloc_crate::vec::Vec;

// =============================================================================
// Stdio.
// =============================================================================

/// How the spawned child sees one of its standard file descriptors.
pub enum Stdio {
    /// Inherit the parent's same-position fd (0 for stdin, 1 for
    /// stdout, 2 for stderr). The most common case.
    Inherit,
    /// Create a pipe; the child gets the appropriate end, the parent
    /// retains the other end in `Child::stdin` / `stdout` / `stderr`.
    Piped,
    /// Use the given File. Consumed by the spawn (its handle is
    /// closed in the parent on spawn return; the child has the same
    /// underlying Spoor refcount-bumped).
    File(File),
    /// Discard. **NOT YET IMPLEMENTED at v1** — needs a /dev/null
    /// analog in the kernel namespace whose fd we can supply.
    /// Spawning with `Stdio::Null` returns `Error::NotImplemented`.
    Null,
}

// PreparedStdio is the per-slot result of resolving a Stdio variant
// before the syscall. Carries (a) the fd the kernel will install into
// the child's positional slot, (b) any File the parent must keep
// alive through the syscall (so its fd stays open until SYS_SPAWN
// refcount-bumps the underlying Spoor), and (c) any File the parent
// will retain past the syscall (the "other end" of a Piped pair).
//
// `keep_through_syscall` and `parent_keeps` are distinct: the former
// drops at end of Command::spawn (so the parent's copy of the
// child-given end is released for EOF semantics); the latter is
// surfaced via Child::stdin / stdout / stderr.
struct PreparedStdio {
    child_fd: i32,
    keep_through_syscall: Option<File>,
    parent_keeps: Option<File>,
}

fn resolve_stdio(stdio: &mut Stdio, slot_index: u32) -> Result<PreparedStdio> {
    // Replace the field with Inherit so we can move out by variant.
    let owned = core::mem::replace(stdio, Stdio::Inherit);
    match owned {
        Stdio::Inherit => Ok(PreparedStdio {
            child_fd: slot_index as i32,
            keep_through_syscall: None,
            parent_keeps: None,
        }),
        Stdio::Null => Err(Error::NotImplemented),
        Stdio::File(f) => {
            // The child gets f's fd. The parent must hold f alive
            // through the syscall (otherwise the fd is closed before
            // the kernel can refcount-bump). Caller drops it after
            // spawn returns.
            let fd = f.as_raw_fd();
            Ok(PreparedStdio {
                child_fd: fd,
                keep_through_syscall: Some(f),
                parent_keeps: None,
            })
        }
        Stdio::Piped => {
            // Create a pipe; decide which end goes to the child based
            // on the slot (0 = stdin -> child reads; 1/2 = stdout/err
            // -> child writes).
            // SAFETY: t_pipe is a SVC wrapper; failure returns (-1, 0).
            let (rd, wr) = unsafe { t_pipe() };
            if rd < 0 {
                return Err(Error::from_syscall_return(rd)
                    .err()
                    .unwrap_or(Error::Io));
            }
            let rd_file = File::from_raw_handle(Handle::from_raw(
                rd as i32,
                Rights::READ | Rights::WRITE | Rights::TRANSFER,
            ));
            let wr_file = File::from_raw_handle(Handle::from_raw(
                wr as i32,
                Rights::READ | Rights::WRITE | Rights::TRANSFER,
            ));
            let (child_end, parent_end) = if slot_index == 0 {
                // stdin: child reads (rd), parent writes (wr).
                (rd_file, wr_file)
            } else {
                // stdout / stderr: child writes (wr), parent reads (rd).
                (wr_file, rd_file)
            };
            let child_fd = child_end.as_raw_fd();
            Ok(PreparedStdio {
                child_fd,
                keep_through_syscall: Some(child_end),
                parent_keeps: Some(parent_end),
            })
        }
    }
}

// =============================================================================
// Command builder.
// =============================================================================

/// Spawn a child process.
///
/// Mirror of `std::process::Command`. Construct via `Command::new(name)`,
/// chain configuration setters, call `spawn()` to launch.
///
/// Example:
///
/// ```ignore
/// use libthyla_rs::process::{Command, Stdio};
///
/// let mut child = Command::new("hello-rs")
///     .stdout(Stdio::Piped)
///     .spawn()?;
///
/// // ... read from child.stdout.unwrap() ...
///
/// let status = child.wait()?;
/// assert!(status.success());
/// ```
pub struct Command {
    name: String,
    args: Vec<String>,
    stdin: Stdio,
    stdout: Stdio,
    stderr: Stdio,
    cap_mask: u64,
}

impl Command {
    /// Construct a Command that will spawn the binary named `name`.
    /// `name` is a single component (no `/`); the kernel looks it up
    /// in devramfs OR the pivoted root.
    #[inline]
    pub fn new(name: impl Into<String>) -> Command {
        Command {
            name: name.into(),
            args: Vec::new(),
            stdin: Stdio::Inherit,
            stdout: Stdio::Inherit,
            stderr: Stdio::Inherit,
            cap_mask: !0u64, // inherit all caps; kernel intersects with parent
        }
    }

    /// Append one argv entry. (argv[0] is the binary name -- so the
    /// first `.arg()` call produces argv[1] from the child's view.)
    #[inline]
    pub fn arg(&mut self, arg: impl Into<String>) -> &mut Command {
        self.args.push(arg.into());
        self
    }

    /// Append multiple argv entries.
    pub fn args<I, S>(&mut self, args: I) -> &mut Command
    where
        I: IntoIterator<Item = S>,
        S: Into<String>,
    {
        for a in args {
            self.args.push(a.into());
        }
        self
    }

    /// Configure stdin.
    #[inline]
    pub fn stdin(&mut self, redir: Stdio) -> &mut Command {
        self.stdin = redir;
        self
    }

    /// Configure stdout.
    #[inline]
    pub fn stdout(&mut self, redir: Stdio) -> &mut Command {
        self.stdout = redir;
        self
    }

    /// Configure stderr.
    #[inline]
    pub fn stderr(&mut self, redir: Stdio) -> &mut Command {
        self.stderr = redir;
        self
    }

    /// Set the cap_mask the kernel intersects with the parent's caps
    /// to compute the child's cap set. Default: `!0u64` (inherit all).
    /// Use `0` to drop all caps; use specific `T_CAP_*` bits to grant
    /// a subset.
    #[inline]
    pub fn caps(&mut self, cap_mask: u64) -> &mut Command {
        self.cap_mask = cap_mask;
        self
    }

    /// Spawn the child. Returns a `Child` handle; the parent retains
    /// any `Stdio::Piped` ends as `Child::stdin` / `stdout` / `stderr`.
    pub fn spawn(&mut self) -> Result<Child> {
        // Validate name length up front.
        if self.name.is_empty() || self.name.len() > T_SPAWN_NAME_MAX {
            return Err(Error::InvalidArgument);
        }

        // Build the argv buffer. argv_data is NUL-separated; each
        // entry is NUL-terminated; argc = NUL count. The kernel
        // delivers argv = [name, args...] to the child; the name IS
        // included in our argv buffer as argv[0].
        let mut argv_buf: Vec<u8> = Vec::new();
        let mut argc: u32 = 0;
        argv_buf.extend_from_slice(self.name.as_bytes());
        argv_buf.push(0);
        argc += 1;
        for a in &self.args {
            argv_buf.extend_from_slice(a.as_bytes());
            argv_buf.push(0);
            argc += 1;
        }
        if argc as usize > T_SYS_SPAWN_ARGV_MAX {
            return Err(Error::InvalidArgument);
        }
        if argv_buf.len() > T_SYS_SPAWN_ARGV_DATA_MAX {
            return Err(Error::InvalidArgument);
        }

        // Resolve each stdio slot. Errors propagate before we mint
        // any kernel state; on success each PreparedStdio carries the
        // fd to hand the child + the Files to hold/return.
        let stdin_p = resolve_stdio(&mut self.stdin, 0)?;
        let stdout_p = resolve_stdio(&mut self.stdout, 1)?;
        let stderr_p = resolve_stdio(&mut self.stderr, 2)?;

        // fd_list is always 3 entries at v1: positional stdin/stdout/stderr.
        let fd_list: [u32; 3] = [
            stdin_p.child_fd as u32,
            stdout_p.child_fd as u32,
            stderr_p.child_fd as u32,
        ];

        let args_record = TSpawnArgs {
            name_va: self.name.as_ptr() as u64,
            argv_data_va: argv_buf.as_ptr() as u64,
            fd_list_va: fd_list.as_ptr() as u64,
            name_len: self.name.len() as u32,
            argv_data_len: argv_buf.len() as u32,
            argc,
            fd_count: 3,
            perm_flags: 0,
            _pad_envp: 0,
            cap_mask: self.cap_mask,
        };

        // SAFETY: every pointer in args_record points into a buffer
        // owned by this scope (name, argv_buf, fd_list); each is alive
        // until the syscall returns. PreparedStdio's keep_through_syscall
        // Files hold any child-end fds alive across the SVC. The
        // kernel copies all data before returning.
        let rc = unsafe { t_spawn_full_argv(&args_record as *const _) };
        let pid_or_err = Error::from_syscall_return(rc);

        // Whether spawn succeeded or failed, the parent's copies of
        // the child-end pipe fds (and File(f) handles) must drop here
        // -- the kernel either took its refcount on success, or
        // released the pipe pair on failure (no extra clean-up needed
        // beyond Drop running). Pull the Files out of PreparedStdio
        // into a let-binding whose scope ends after this point so the
        // borrow checker doesn't tangle.
        let _drop_stdin = stdin_p.keep_through_syscall;
        let _drop_stdout = stdout_p.keep_through_syscall;
        let _drop_stderr = stderr_p.keep_through_syscall;

        let pid = pid_or_err?;

        Ok(Child {
            pid: pid as i32,
            stdin: stdin_p.parent_keeps,
            stdout: stdout_p.parent_keeps,
            stderr: stderr_p.parent_keeps,
        })
    }
}

// =============================================================================
// Child + ExitStatus.
// =============================================================================

/// A spawned child process.
///
/// Drop does NOT reap the child; call `.wait()` explicitly. (This
/// matches `std::process::Child` behaviour. The kernel's child
/// becomes a zombie on exit; an un-waited Child stays as a zombie
/// in the parent's Proc table until parent itself exits.)
pub struct Child {
    pid: i32,
    /// The parent end of the stdin pipe, if `Stdio::Piped` was set
    /// for stdin. Write to this to send bytes to the child's stdin.
    pub stdin: Option<File>,
    /// The parent end of the stdout pipe, if `Stdio::Piped` was set
    /// for stdout. Read from this to receive the child's stdout.
    pub stdout: Option<File>,
    /// The parent end of the stderr pipe, if `Stdio::Piped` was set
    /// for stderr. Read from this to receive the child's stderr.
    pub stderr: Option<File>,
}

impl Child {
    /// The child's process id.
    #[inline]
    pub fn pid(&self) -> i32 {
        self.pid
    }

    /// Block until the child exits; reap and return its exit status.
    ///
    /// V1 limitation: `SYS_WAIT_PID` reaps *any* zombie child, not a
    /// specific pid. If the parent has multiple children, the reaped
    /// pid may not be `self.pid`; we accept whatever the kernel
    /// returns. Multi-child shells will need a wait-by-pid surface.
    pub fn wait(&mut self) -> Result<ExitStatus> {
        let mut status: i32 = 0;
        let rc = unsafe { t_wait_pid(&mut status as *mut i32) };
        let _reaped = Error::from_syscall_return(rc)?;
        Ok(ExitStatus(status))
    }
}

/// The exit status of a reaped child.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub struct ExitStatus(i32);

impl ExitStatus {
    /// `true` iff the child exited with status 0.
    #[inline]
    #[must_use]
    pub const fn success(&self) -> bool {
        self.0 == 0
    }

    /// The raw status. `std::process::ExitStatus::code` returns
    /// `Option<i32>` because POSIX distinguishes exit-by-code from
    /// exit-by-signal; the kernel surfaces a single i32 here, so
    /// the option is always `Some`. Signal-decode integration lands
    /// with t::notes (U-2e).
    #[inline]
    #[must_use]
    pub const fn code(&self) -> Option<i32> {
        Some(self.0)
    }

    /// The raw status integer. Available for code that needs to
    /// distinguish kernel-specific status encodings ahead of the
    /// notes-integration story.
    #[inline]
    #[must_use]
    pub const fn raw(&self) -> i32 {
        self.0
    }
}

// =============================================================================
// Free function: pipe().
// =============================================================================

/// Create a connected pipe. Returns `(reader, writer)` Files.
///
/// Backed by SYS_PIPE. Both Files own kernel handles; closing either
/// EOFs the other. Useful for explicit inter-process byte streams
/// in addition to the `Command`'s `Stdio::Piped` automation.
pub fn pipe() -> Result<(File, File)> {
    // SAFETY: SVC wrapper; failure returns (-1, 0).
    let (rd, wr) = unsafe { t_pipe() };
    if rd < 0 {
        return Err(Error::from_syscall_return(rd)
            .err()
            .unwrap_or(Error::Io));
    }
    let rd_file = File::from_raw_handle(Handle::from_raw(
        rd as i32,
        Rights::READ | Rights::WRITE | Rights::TRANSFER,
    ));
    let wr_file = File::from_raw_handle(Handle::from_raw(
        wr as i32,
        Rights::READ | Rights::WRITE | Rights::TRANSFER,
    ));
    Ok((rd_file, wr_file))
}
