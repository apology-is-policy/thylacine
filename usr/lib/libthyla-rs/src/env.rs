// libthyla-rs::env — process environment (argv today; envp is a v1.x seam).
//
// Closes DOC-GAP-REPORT G03 [P1]: a native program could not read its own
// argv/argc. The kernel populates a System-V startup frame on the user
// stack (exec_setup_with_argv; kernel/include/thylacine/exec.h "Shape B"),
// but libthyla-rs's `_start` did not capture it and exposed no accessor.
//
// As-built: `_start` (lib.rs) loads argc from `[sp]` and `&argv[0]` from
// `sp + 8` BEFORE any function prologue runs (the prologue moves sp, so the
// capture has to be the very first thing), stashes both into process-global
// statics, then calls the app's `rs_main()`. `env::args()` reads those
// statics back. Both startup shapes are handled by the same capture: Shape A
// ("no argv") puts argc = 0 at `[sp]`, so `args()` reports an empty operand
// list; Shape B puts the real argc + argv there.
//
// LIFETIME: the argv pointer array and the strings it references live in the
// initial stack frame, at addresses >= the initial sp. The live stack grows
// DOWNWARD from sp (and peer threads get their own stacks), so a well-behaved
// program never overwrites them -- they are valid for the whole process
// lifetime, hence `&'static [u8]`.
//
// ENVP: the kernel reserves the `_pad_envp` spawn-ABI slot but does not pass
// an environment at v1.0 (the startup frame's envp slot is a single NULL).
// There is therefore no `var()` / `vars()` yet; it lands when the envp
// surface does (DOC-GAP G15).

use crate::err::{Error, Result};
use crate::rt_raw_args;
use alloc_crate::string::String;

// The kernel's SYS_OPEN_PATH_MAX (1024) + NUL. dot_path is bounded by it, so a
// stack buffer of this size always holds the full cwd.
const CWD_BUF: usize = 1025;

/// The current working directory (LS-4), as an owned absolute path String.
/// Mirrors `std::env::current_dir`. The kernel tracks one cwd per Proc (the
/// Plan 9 "dot"); a relative `fs::`/open path resolves against it.
pub fn current_dir() -> Result<String> {
    let mut buf = [0u8; CWD_BUF];
    // SAFETY: buf is a valid writable byte buffer of len CWD_BUF.
    let rc = unsafe { crate::t_getcwd(buf.as_mut_ptr(), buf.len()) };
    let len = Error::from_syscall_return(rc)? as usize;
    let s = core::str::from_utf8(&buf[..len]).map_err(|_| Error::InvalidArgument)?;
    Ok(String::from(s))
}

/// Set the per-Proc cwd to `path` (LS-4). Mirrors `std::env::set_current_dir`.
/// The kernel requires `path` to resolve to a directory the caller can search
/// (X); spawned children inherit the new cwd.
pub fn set_current_dir(path: &str) -> Result<()> {
    // SAFETY: path is a valid &str (ptr + len).
    let rc = unsafe { crate::t_chdir(path.as_ptr(), path.len()) };
    Error::from_syscall_return(rc)?;
    Ok(())
}

/// The process's command-line arguments, read from the kernel-populated
/// initial stack frame. `argv[0]` is the program name (the spawn convention
/// -- `process::Command` builds `argv = [name, args...]`); positional
/// operands start at index 1.
#[derive(Clone, Copy)]
pub struct Args {
    argc: usize,
    argv: *const *const u8,
    next: usize,
}

/// The calling process's arguments, including `argv[0]` (the program name).
/// Use [`Args::operands`] to iterate just the positional operands.
#[inline]
pub fn args() -> Args {
    let (argc, argv) = rt_raw_args();
    // SAFETY: rt_raw_args returns exactly the (argc, &argv[0]) the kernel
    // placed on the startup frame and `_start` captured; the frame is
    // 'static (see the module header's LIFETIME note).
    unsafe { Args::from_raw(argc, argv) }
}

impl Args {
    /// # Safety
    /// `argc`/`argv` must describe a valid startup frame: `argv` points at
    /// `argc` non-null, NUL-terminated C-string pointers in the 'static
    /// initial frame. Callers other than [`args`] should not construct this.
    #[inline]
    pub unsafe fn from_raw(argc: usize, argv: *const *const u8) -> Args {
        Args { argc, argv, next: 0 }
    }

    /// Total count including `argv[0]` (the program name).
    #[inline]
    pub fn len(&self) -> usize {
        self.argc
    }

    /// True when there are no arguments at all (not even `argv[0]`) -- i.e.
    /// the Shape-A "no argv" startup frame.
    #[inline]
    pub fn is_empty(&self) -> bool {
        self.argc == 0
    }

    /// `argv[0]`, the program name, or `None` if argc == 0.
    #[inline]
    pub fn prog_name(&self) -> Option<&'static [u8]> {
        self.get(0)
    }

    /// `argv[i]` as a NUL-trimmed byte slice, or `None` if out of range.
    pub fn get(&self, i: usize) -> Option<&'static [u8]> {
        if i >= self.argc {
            return None;
        }
        // SAFETY: i < argc; from_raw's contract guarantees argv[0..argc] are
        // valid, NUL-terminated C strings in the 'static initial frame.
        unsafe {
            let p = *self.argv.add(i);
            if p.is_null() {
                return None;
            }
            Some(cstr_bytes(p))
        }
    }

    /// `argv[i]` as `&str`, or `None` if out of range or not valid UTF-8.
    #[inline]
    pub fn get_str(&self, i: usize) -> Option<&'static str> {
        self.get(i).and_then(|b| core::str::from_utf8(b).ok())
    }

    /// Iterator over the positional operands -- `argv[1..]`, skipping the
    /// program name. The common coreutils entry point.
    #[inline]
    pub fn operands(&self) -> Operands {
        Operands { args: *self, i: 1 }
    }
}

// Iterating an `Args` yields every entry INCLUDING argv[0]; use `.operands()`
// to skip the program name.
impl Iterator for Args {
    type Item = &'static [u8];
    fn next(&mut self) -> Option<&'static [u8]> {
        let i = self.next;
        if i >= self.argc {
            return None;
        }
        self.next += 1;
        self.get(i)
    }
}

/// Iterator over positional operands (`argv[1..]`).
pub struct Operands {
    args: Args,
    i: usize,
}

impl Iterator for Operands {
    type Item = &'static [u8];
    fn next(&mut self) -> Option<&'static [u8]> {
        let item = self.args.get(self.i);
        if item.is_some() {
            self.i += 1;
        }
        item
    }
}

/// Length of the NUL-terminated C string at `p`, returned as a borrowed
/// slice (no copy). The terminator is excluded.
///
/// # Safety
/// `p` must point at a NUL-terminated byte sequence that outlives `'static`
/// (the startup frame strings do).
unsafe fn cstr_bytes(p: *const u8) -> &'static [u8] {
    let mut n = 0usize;
    while *p.add(n) != 0 {
        n += 1;
    }
    core::slice::from_raw_parts(p, n)
}
