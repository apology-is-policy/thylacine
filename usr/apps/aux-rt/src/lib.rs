// aux-rt -- the auxiliary track's shared native runtime.
//
// This crate exists ONLY to hand-roll the two surfaces libthyla-rs does not
// expose to a native app today (see usr/apps/DOC-GAP-REPORT.md):
//
//   * G03 [P1] -- a native app cannot read its own argv/argc. libthyla-rs's
//     _start does `bl rs_main` with no SP capture and there is no callee
//     accessor, though the kernel DOES populate a System-V startup frame on
//     the stack (P6-pouch-kernel-auxv). aux-rt supplies a `#[naked]` rs_main
//     that captures sp BEFORE any prologue runs and marshals (argc, argv)
//     into a C-ABI call to the app's `aux_main`.
//
//   * G05 [P1] -- there is no Stdout/Stdin/Stderr handle (io.rs has the
//     Read/Write traits but no concrete fd-0/1/2 sinks, and fs::File has no
//     from_raw_fd). aux-rt wraps the raw `t_read`/`t_write` SVC wrappers
//     (which DO exist, lib.rs:687/708) into io::Read/io::Write handles.
//
// aux-rt does NOT fork or extend libthyla-rs; it composes its public API.
// When libthyla-rs grows a native args() accessor + stdio handles, every
// app here drops aux-rt for the upstream surface in a one-line change.
//
// ENTRY CHAIN:  _start (libthyla-rs)  --bl-->  rs_main (aux-rt, naked)
//                 --b-->  aux_main (app, via aux_rt::main!)  -->  run (app)
// rs_main never returns in the Rust sense: the tail `b aux_main` leaves x30
// pointing at _start, so aux_main's i64 return flows straight to _start's
// SYS_EXITS (0 => exits("ok"); non-zero => exits("fail")).

#![no_std]

extern crate alloc as alloc_crate;

use core::arch::naked_asm;

// Re-export the io traits so the print!/println! macros (and callers) need
// no `use libthyla_rs::io::...` of their own.
pub use libthyla_rs::err::{Error, Result};
pub use libthyla_rs::io::{Read, Write};

use libthyla_rs::{t_read, t_write};

// The application binary defines this (typically via aux_rt::main!). aux-rt
// only DECLARES it; `sym aux_main` in the naked rs_main emits a relocation
// resolved at final link, exactly as libthyla-rs's _start references the
// (downstream-defined) rs_main symbol.
extern "C" {
    fn aux_main(argc: usize, argv: *const *const u8) -> i64;
}

// rs_main -- the symbol libthyla-rs's _start calls (`bl rs_main`). At entry,
// sp still points at the System-V startup frame's argc word (Shape B, the
// argv-bearing layout; docs/reference/86-pouch-stratumd-boot.md "Shape A vs
// Shape B" + docs/reference/27-exec.md "Initial process stack"):
//
//   [sp + 0]            argc                 (u64)
//   [sp + 8 + 8*i]      argv[i]              (*const u8, NUL-terminated), i in 0..argc
//   [sp + 8 + 8*argc]   NULL                 (argv terminator)
//   ... envp NULL, six auxv entries, AT_RANDOM block, then the strings ...
//
// A naked fn emits no prologue, so sp is untouched: we read argc from [sp],
// compute &argv[0] = sp+8, and tail-call aux_main(argc, argv).
#[unsafe(naked)]
#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    naked_asm!(
        "bti c",            // landing pad (harmless on the direct bl from _start)
        "ldr x0, [sp]",     // x0 = argc            (frame[0])
        "add x1, sp, #8",   // x1 = &argv[0]        (frame + 8)
        "b   {main}",       // tail-call aux_main(argc, argv); ret -> _start
        main = sym aux_main,
    )
}

// =============================================================================
// Args -- callee-side argv access (the G03 workaround).
// =============================================================================

/// The process's command-line arguments, read from the kernel-populated
/// initial stack frame. `argv[0]` is the program name (the spawn convention,
/// process.rs: "argv = [name, args...]"); positional operands start at index 1.
///
/// LIFETIME: the argv pointer array and the strings they reference live in
/// the initial stack frame, at addresses >= the initial sp. The live stack
/// grows DOWNWARD from sp, so a well-behaved program never overwrites them --
/// they are valid for the whole process lifetime, hence `&'static [u8]`.
#[derive(Clone, Copy)]
pub struct Args {
    argc: usize,
    argv: *const *const u8,
    next: usize,
}

impl Args {
    /// # Safety
    /// `argc`/`argv` must be the values delivered by aux-rt's rs_main (a
    /// valid Shape-B frame): `argv` points at `argc` non-null C-string
    /// pointers. Callers other than the aux_rt::main! macro should not
    /// construct this.
    pub unsafe fn from_raw(argc: usize, argv: *const *const u8) -> Args {
        Args { argc, argv, next: 0 }
    }

    /// Total count including `argv[0]` (the program name).
    pub fn len(&self) -> usize {
        self.argc
    }

    pub fn is_empty(&self) -> bool {
        self.argc == 0
    }

    /// `argv[0]`, the program name, or `None` if argc == 0.
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
    pub fn get_str(&self, i: usize) -> Option<&'static str> {
        self.get(i).and_then(|b| core::str::from_utf8(b).ok())
    }

    /// Iterator over the positional operands -- `argv[1..]`, skipping the
    /// program name. The common coreutils entry point.
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

unsafe fn cstr_bytes(p: *const u8) -> &'static [u8] {
    let mut n = 0usize;
    while *p.add(n) != 0 {
        n += 1;
    }
    core::slice::from_raw_parts(p, n)
}

// =============================================================================
// Stdio -- fd-0/1/2 handles over the raw t_read/t_write wrappers (G05).
// =============================================================================

const FD_STDIN: i64 = 0;
const FD_STDOUT: i64 = 1;
const FD_STDERR: i64 = 2;

/// Handle to fd 0. NOTE (G05): at v1.0 a standalone native app has no
/// terminal-backed fd 0 -- reads succeed only when a parent/shell wired the
/// fd (e.g. a pipeline). A read on an unwired fd returns an error.
pub struct Stdin;
/// Handle to fd 1. Same v1.0 caveat as Stdin: visible output requires fd 1
/// to be wired (pipeline / redirect / parent-inherited). For an always-on
/// diagnostic channel use libthyla_rs::t_putstr (the kernel UART).
pub struct Stdout;
/// Handle to fd 2.
pub struct Stderr;

pub fn stdin() -> Stdin {
    Stdin
}
pub fn stdout() -> Stdout {
    Stdout
}
pub fn stderr() -> Stderr {
    Stderr
}

impl Read for Stdin {
    fn read(&mut self, buf: &mut [u8]) -> Result<usize> {
        let rc = unsafe { t_read(FD_STDIN, buf.as_mut_ptr(), buf.len()) };
        Error::from_syscall_return(rc).map(|n| n as usize)
    }
}

impl Write for Stdout {
    fn write(&mut self, buf: &[u8]) -> Result<usize> {
        let rc = unsafe { t_write(FD_STDOUT, buf.as_ptr(), buf.len()) };
        Error::from_syscall_return(rc).map(|n| n as usize)
    }
    fn flush(&mut self) -> Result<()> {
        Ok(())
    }
}

impl Write for Stderr {
    fn write(&mut self, buf: &[u8]) -> Result<usize> {
        let rc = unsafe { t_write(FD_STDERR, buf.as_ptr(), buf.len()) };
        Error::from_syscall_return(rc).map(|n| n as usize)
    }
    fn flush(&mut self) -> Result<()> {
        Ok(())
    }
}

/// Best-effort write of all of `buf` to stdout. Errors are swallowed: a
/// standalone run with an unwired fd 1 should not spuriously fail (G05).
pub fn out(buf: &[u8]) {
    let _ = stdout().write_all(buf);
}

/// Best-effort write of all of `buf` to stderr.
pub fn err(buf: &[u8]) {
    let _ = stderr().write_all(buf);
}

/// Stream every byte of `r` into `w` using a 4 KiB buffer. Returns the total
/// bytes copied. The workhorse for cat/tee-style apps. (libthyla-rs::io has
/// no `copy` free fn -- DOC-GAP G05's family.)
pub fn copy<R: Read + ?Sized, W: Write + ?Sized>(r: &mut R, w: &mut W) -> Result<u64> {
    let mut buf = [0u8; 4096];
    let mut total = 0u64;
    loop {
        match r.read(&mut buf)? {
            0 => return Ok(total),
            n => {
                w.write_all(&buf[..n])?;
                total += n as u64;
            }
        }
    }
}

/// Read all of `r` into a fresh `Vec` (thin wrapper over the io::Read
/// `read_to_end` default method).
pub fn slurp<R: Read + ?Sized>(r: &mut R) -> Result<alloc_crate::vec::Vec<u8>> {
    let mut v = alloc_crate::vec::Vec::new();
    r.read_to_end(&mut v)?;
    Ok(v)
}

// =============================================================================
// Entry + print macros.
// =============================================================================

/// Define the app entry. `$run` is `fn(Args) -> i64`; its return becomes the
/// process exit status. Usage:
///
/// ```ignore
/// aux_rt::main!(run);
/// fn run(args: aux_rt::Args) -> i64 { 0 }
/// ```
#[macro_export]
macro_rules! main {
    ($run:path) => {
        /// # Safety
        /// Called only by aux-rt's `rs_main`, which delivers `argc`/`argv`
        /// from a valid Shape-B startup frame. Not for direct invocation.
        #[no_mangle]
        pub unsafe extern "C" fn aux_main(argc: usize, argv: *const *const u8) -> i64 {
            // In an `unsafe fn`, the unsafe `from_raw` call needs no block
            // (2021 edition). argc/argv are exactly what rs_main marshalled.
            let args = $crate::Args::from_raw(argc, argv);
            $run(args)
        }
    };
}

/// `print!` to stdout (best-effort; fully-qualified so callers import nothing).
#[macro_export]
macro_rules! print {
    ($($arg:tt)*) => {{
        let _ = <$crate::Stdout as $crate::Write>::write_fmt(
            &mut $crate::stdout(), core::format_args!($($arg)*));
    }};
}

/// `println!` to stdout (best-effort).
#[macro_export]
macro_rules! println {
    () => {{ $crate::out(b"\n"); }};
    ($($arg:tt)*) => {{
        $crate::print!($($arg)*);
        $crate::out(b"\n");
    }};
}

/// `eprintln!` to stderr (best-effort).
#[macro_export]
macro_rules! eprintln {
    () => {{ $crate::err(b"\n"); }};
    ($($arg:tt)*) => {{
        let _ = <$crate::Stderr as $crate::Write>::write_fmt(
            &mut $crate::stderr(), core::format_args!($($arg)*));
        $crate::err(b"\n");
    }};
}
