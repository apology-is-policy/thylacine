// libthyla-rs::io — I/O traits.
//
// Mirror of std::io's trait surface, scoped to what native Thylacine
// programs actually need at v1:
//
//   - `Read`     — pull bytes from a source.
//   - `Write`    — push bytes to a sink.
//   - `Seek`     — reposition the byte offset.
//   - `BufRead`  — Read plus a buffer for line-oriented input.
//
// Plus two concrete adapters:
//
//   - `BufReader<R>` — buffering wrapper around any `Read`. The line
//                     editor + shell input parser will use this.
//   - `Cursor<T>`    — in-memory Read/Seek over a byte slice or Vec.
//                     Useful for tests and for parsing pre-loaded data.
//
// Foundation chunk: U-2c-io per docs/UTOPIA-SHELL-DESIGN.md §15 (split
// from U-2c into U-2c-path / U-2c-io / U-2c-fs sub-chunks). Backs
// `t::fs::File`'s trait impls AND every future libthyla-rs source/sink
// type (PipeReader, PipeWriter, SrvConn streams, etc.).
//
// DESIGN NOTES:
//   - `Result<T>` is `crate::err::Result<T>` — the single error type.
//     No separate `io::Error`.
//   - EOF on read returns `Ok(0)` (POSIX convention; std mirrors).
//     `read_exact` translates EOF-before-full to `Error::UnexpectedEof`.
//   - `write` returning `Ok(0)` is a writer-stopped-making-progress
//     signal. `write_all` translates to `Error::WriteZero`.
//   - Default-impl helpers (read_to_end, read_to_string, read_exact,
//     write_all, write_fmt, stream_position, rewind, read_until,
//     read_line) come "for free" once a type implements the core
//     methods. Matches std::io.
//   - Async traits, AsyncRead/Write, vectored I/O (read_vectored,
//     write_vectored), IoSlice/IoSliceMut — all deferred. v1 is
//     synchronous-only; vectored I/O is a v1.x consideration.

use crate::err::{Error, Result};
use crate::{t_read, t_write};
use alloc_crate::string::String;
use alloc_crate::vec::Vec;
use core::cmp;

// =============================================================================
// Read.
// =============================================================================

/// Source of bytes.
///
/// Implementors define `read`; default impls of `read_to_end`,
/// `read_to_string`, and `read_exact` build on it.
pub trait Read {
    /// Read up to `buf.len()` bytes into `buf`. Returns the number of
    /// bytes read. `Ok(0)` signals EOF (the source has no more data;
    /// not an error).
    fn read(&mut self, buf: &mut [u8]) -> Result<usize>;

    /// Read all remaining bytes from `self` into `buf`, appending.
    /// Returns the number of bytes read on this call.
    fn read_to_end(&mut self, buf: &mut Vec<u8>) -> Result<usize> {
        let start_len = buf.len();
        let mut tmp = [0u8; 1024];
        loop {
            match self.read(&mut tmp) {
                Ok(0) => return Ok(buf.len() - start_len),
                Ok(n) => buf.extend_from_slice(&tmp[..n]),
                Err(e) => return Err(e),
            }
        }
    }

    /// Read all remaining bytes from `self`, append to `buf` as UTF-8.
    /// Returns the number of bytes read on this call. Fails with
    /// `Error::InvalidArgument` if the read bytes are not valid UTF-8.
    fn read_to_string(&mut self, buf: &mut String) -> Result<usize> {
        let mut v = Vec::new();
        let n = self.read_to_end(&mut v)?;
        let s = core::str::from_utf8(&v).map_err(|_| Error::InvalidArgument)?;
        buf.push_str(s);
        Ok(n)
    }

    /// Read exactly `buf.len()` bytes. EOF before full -> `UnexpectedEof`.
    fn read_exact(&mut self, mut buf: &mut [u8]) -> Result<()> {
        while !buf.is_empty() {
            match self.read(buf) {
                Ok(0) => return Err(Error::UnexpectedEof),
                Ok(n) => {
                    // Advance buf by n bytes.
                    let tmp = core::mem::take(&mut buf);
                    buf = &mut tmp[n..];
                }
                Err(e) => return Err(e),
            }
        }
        Ok(())
    }
}

// Blanket impl for `&mut R`, mirroring std.
impl<R: Read + ?Sized> Read for &mut R {
    #[inline]
    fn read(&mut self, buf: &mut [u8]) -> Result<usize> {
        (**self).read(buf)
    }
}

// =============================================================================
// Write.
// =============================================================================

/// Sink of bytes.
///
/// Implementors define `write` and `flush`. Default impls of
/// `write_all` and `write_fmt` build on them.
pub trait Write {
    /// Write up to `buf.len()` bytes from `buf`. Returns the number
    /// of bytes written. May write fewer than requested (call again
    /// for the rest, or use `write_all`).
    fn write(&mut self, buf: &[u8]) -> Result<usize>;

    /// Flush any pending buffered data to the underlying sink. For
    /// unbuffered sinks (like raw `File`) this is a no-op.
    fn flush(&mut self) -> Result<()>;

    /// Write all of `buf`. Writer-no-progress (`Ok(0)`) -> `WriteZero`.
    fn write_all(&mut self, mut buf: &[u8]) -> Result<()> {
        while !buf.is_empty() {
            match self.write(buf) {
                Ok(0) => return Err(Error::WriteZero),
                Ok(n) => buf = &buf[n..],
                Err(e) => return Err(e),
            }
        }
        Ok(())
    }

    /// Glue between `core::fmt::Write` and this trait. Lets callers
    /// `write!(w, "...")` against a `Write`.
    fn write_fmt(&mut self, args: core::fmt::Arguments<'_>) -> Result<()> {
        // Adapter that captures the first error and short-circuits.
        struct Adapter<'a, W: Write + ?Sized> {
            inner: &'a mut W,
            err: Option<Error>,
        }
        impl<W: Write + ?Sized> core::fmt::Write for Adapter<'_, W> {
            fn write_str(&mut self, s: &str) -> core::fmt::Result {
                match self.inner.write_all(s.as_bytes()) {
                    Ok(()) => Ok(()),
                    Err(e) => {
                        self.err = Some(e);
                        Err(core::fmt::Error)
                    }
                }
            }
        }
        let mut adapter = Adapter { inner: self, err: None };
        match core::fmt::write(&mut adapter, args) {
            Ok(()) => Ok(()),
            Err(_) => Err(adapter.err.unwrap_or(Error::Io)),
        }
    }
}

impl<W: Write + ?Sized> Write for &mut W {
    #[inline]
    fn write(&mut self, buf: &[u8]) -> Result<usize> {
        (**self).write(buf)
    }
    #[inline]
    fn flush(&mut self) -> Result<()> {
        (**self).flush()
    }
}

// An in-memory sink: appending to a `Vec<u8>` always succeeds (modulo
// allocation). Mirrors `std::io::Write for Vec<u8>`. Useful for buffering
// composed output before a single syscall, and for capturing a writer's
// bytes in tests (the U-6g `Repl::feed` sink).
impl Write for Vec<u8> {
    #[inline]
    fn write(&mut self, buf: &[u8]) -> Result<usize> {
        self.extend_from_slice(buf);
        Ok(buf.len())
    }
    #[inline]
    fn flush(&mut self) -> Result<()> {
        Ok(())
    }
}

// =============================================================================
// Seek.
// =============================================================================

/// Anchor for `Seek::seek`.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum SeekFrom {
    /// Absolute offset from the start of the stream.
    Start(u64),
    /// Relative offset from the end of the stream (typically negative).
    End(i64),
    /// Relative offset from the current cursor position.
    Current(i64),
}

/// Streams that can reposition their byte offset.
pub trait Seek {
    /// Reposition. Returns the new offset.
    fn seek(&mut self, pos: SeekFrom) -> Result<u64>;

    /// Convenience: the current offset (`SeekFrom::Current(0)`).
    fn stream_position(&mut self) -> Result<u64> {
        self.seek(SeekFrom::Current(0))
    }

    /// Convenience: `seek(Start(0))`, drop the result.
    fn rewind(&mut self) -> Result<()> {
        self.seek(SeekFrom::Start(0)).map(|_| ())
    }
}

// =============================================================================
// BufRead.
// =============================================================================

/// `Read` plus a buffer for line-oriented input.
///
/// Implementors expose the buffer via `fill_buf` and signal consumption
/// via `consume`. `BufReader<R>` is the canonical implementation; the
/// trait is public so future types (e.g., a 9P-stream reader) can
/// participate in `read_line` / `read_until` without re-implementing.
pub trait BufRead: Read {
    /// Return the buffered bytes available to read. Refills from the
    /// underlying source if empty.
    fn fill_buf(&mut self) -> Result<&[u8]>;

    /// Mark `amt` bytes from the buffer as consumed (subsequent reads
    /// will skip them).
    fn consume(&mut self, amt: usize);

    /// Read until `delim` (inclusive) into `buf`. Returns the number
    /// of bytes appended.
    fn read_until(&mut self, delim: u8, buf: &mut Vec<u8>) -> Result<usize> {
        let mut total = 0;
        loop {
            let (done, used) = {
                let available = self.fill_buf()?;
                if available.is_empty() {
                    return Ok(total);
                }
                if let Some(pos) = available.iter().position(|&b| b == delim) {
                    buf.extend_from_slice(&available[..=pos]);
                    (true, pos + 1)
                } else {
                    buf.extend_from_slice(available);
                    (false, available.len())
                }
            };
            self.consume(used);
            total += used;
            if done {
                return Ok(total);
            }
        }
    }

    /// Read a UTF-8 line (up to + including `'\n'`) into `buf`.
    /// Returns the number of bytes appended. Fails with
    /// `Error::InvalidArgument` if the line is not valid UTF-8.
    fn read_line(&mut self, buf: &mut String) -> Result<usize> {
        let mut v = Vec::new();
        let n = self.read_until(b'\n', &mut v)?;
        let s = core::str::from_utf8(&v).map_err(|_| Error::InvalidArgument)?;
        buf.push_str(s);
        Ok(n)
    }
}

// =============================================================================
// BufReader<R> — buffered wrapper.
// =============================================================================

const DEFAULT_BUF_CAPACITY: usize = 8 * 1024;

/// Buffered wrapper around any `Read`.
///
/// Amortises syscall cost across many small reads + provides the
/// `BufRead` surface for line-oriented input.
pub struct BufReader<R> {
    inner: R,
    buf: Vec<u8>,
    pos: usize,
}

impl<R: Read> BufReader<R> {
    /// Wrap `inner` with the default 8 KiB buffer.
    #[inline]
    pub fn new(inner: R) -> Self {
        Self::with_capacity(DEFAULT_BUF_CAPACITY, inner)
    }

    /// Wrap `inner` with a buffer of `capacity` bytes (floored at 1).
    ///
    /// A zero-capacity buffer would make `fill_buf` read into an empty slice
    /// (`Ok(0)`) and report a false EOF on the `BufRead` surface; the 1-byte
    /// floor keeps the buffered path making progress.
    pub fn with_capacity(capacity: usize, inner: R) -> Self {
        BufReader {
            inner,
            buf: Vec::with_capacity(capacity.max(1)),
            pos: 0,
        }
    }

    /// Consume the BufReader, returning the wrapped reader. Drops any
    /// remaining buffered data.
    #[inline]
    pub fn into_inner(self) -> R {
        self.inner
    }

    /// Borrow the wrapped reader.
    #[inline]
    pub fn get_ref(&self) -> &R {
        &self.inner
    }

    /// Mutably borrow the wrapped reader. (Bypassing the buffer via
    /// this reference will desync the buffered position; use sparingly.)
    #[inline]
    pub fn get_mut(&mut self) -> &mut R {
        &mut self.inner
    }
}

impl<R: Read> Read for BufReader<R> {
    fn read(&mut self, dst: &mut [u8]) -> Result<usize> {
        // Bypass buffer for large reads when the buffer is empty:
        // no point copying through the buffer if the caller wants
        // more bytes than the buffer holds.
        if self.pos == self.buf.len() && dst.len() >= self.buf.capacity() {
            self.discard_buffer();
            return self.inner.read(dst);
        }
        let avail = self.fill_buf()?;
        let n = cmp::min(avail.len(), dst.len());
        dst[..n].copy_from_slice(&avail[..n]);
        self.consume(n);
        Ok(n)
    }
}

impl<R: Read> BufRead for BufReader<R> {
    fn fill_buf(&mut self) -> Result<&[u8]> {
        if self.pos >= self.buf.len() {
            // Buffer exhausted. Refill from inner. set_len(cap) exposes the
            // Vec's uninitialized spare capacity, but ONLY for the read: every
            // exit below restores len to reflect exactly the INITIALIZED bytes,
            // so a `&self.buf[..]` is never formed over uninit memory. The Err
            // leg in particular must reset -- otherwise len stays at cap with an
            // uninitialized tail, the next call sees pos < len, skips the
            // refill, and hands the caller `&self.buf[pos..]` over uninitialized
            // heap (UB + a process-local info-leak of recycled allocations).
            let cap = self.buf.capacity();
            // SAFETY: u8 has no invalid bit patterns; the buffer's capacity
            // bytes are allocated. The length is corrected on every path before
            // any read of the buffer's contents.
            unsafe {
                self.buf.set_len(cap);
            }
            let n = match self.inner.read(&mut self.buf[..]) {
                // Clamp the trusted count to cap: a kernel ABI regression
                // returning n > cap would make set_len(n) violate the Vec
                // invariant (out-of-bounds reads), strictly worse than a short
                // read. The clamp turns that into safe truncation.
                Ok(n) => cmp::min(n, cap),
                Err(e) => {
                    // SAFETY: 0 <= cap. Reset to the empty state so the
                    // uninitialized tail is never observable and the next call
                    // re-fills cleanly.
                    unsafe {
                        self.buf.set_len(0);
                    }
                    self.pos = 0;
                    return Err(e);
                }
            };
            unsafe {
                self.buf.set_len(n);
            }
            self.pos = 0;
        }
        Ok(&self.buf[self.pos..])
    }

    fn consume(&mut self, amt: usize) {
        self.pos = cmp::min(self.pos + amt, self.buf.len());
    }
}

impl<R: Read> BufReader<R> {
    fn discard_buffer(&mut self) {
        self.buf.clear();
        self.pos = 0;
    }
}

// =============================================================================
// Cursor<T> — in-memory Read/Seek over a byte slice.
// =============================================================================

/// In-memory `Read` + `Seek` over a byte container.
///
/// Useful for tests that want to drive a `Read` consumer without an
/// actual file, and for parsing pre-loaded data without re-reading.
///
/// `T` is the underlying storage; common choices are `&[u8]`,
/// `Vec<u8>`, or `Box<[u8]>`. Implements Read + Seek when `T: AsRef<[u8]>`.
pub struct Cursor<T> {
    inner: T,
    pos: u64,
}

impl<T> Cursor<T> {
    /// Wrap `inner` with a starting position of 0.
    #[inline]
    pub const fn new(inner: T) -> Self {
        Cursor { inner, pos: 0 }
    }

    #[inline]
    pub fn into_inner(self) -> T {
        self.inner
    }

    #[inline]
    pub fn get_ref(&self) -> &T {
        &self.inner
    }

    #[inline]
    pub fn position(&self) -> u64 {
        self.pos
    }

    #[inline]
    pub fn set_position(&mut self, pos: u64) {
        self.pos = pos;
    }
}

impl<T: AsRef<[u8]>> Read for Cursor<T> {
    fn read(&mut self, buf: &mut [u8]) -> Result<usize> {
        let slice = self.inner.as_ref();
        let pos = self.pos as usize;
        if pos >= slice.len() {
            return Ok(0);
        }
        let n = cmp::min(buf.len(), slice.len() - pos);
        buf[..n].copy_from_slice(&slice[pos..pos + n]);
        self.pos += n as u64;
        Ok(n)
    }
}

impl<T: AsRef<[u8]>> Seek for Cursor<T> {
    fn seek(&mut self, pos: SeekFrom) -> Result<u64> {
        let len = self.inner.as_ref().len() as u64;
        let new = match pos {
            SeekFrom::Start(p) => p,
            SeekFrom::End(off) => {
                if off >= 0 {
                    len.checked_add(off as u64).ok_or(Error::InvalidArgument)?
                } else {
                    let abs = (-off) as u64;
                    len.checked_sub(abs).ok_or(Error::InvalidArgument)?
                }
            }
            SeekFrom::Current(off) => {
                if off >= 0 {
                    self.pos
                        .checked_add(off as u64)
                        .ok_or(Error::InvalidArgument)?
                } else {
                    let abs = (-off) as u64;
                    self.pos.checked_sub(abs).ok_or(Error::InvalidArgument)?
                }
            }
        };
        self.pos = new;
        Ok(new)
    }
}

// =============================================================================
// Standard streams — fd-0/1/2 handles (DOC-GAP G05).
// =============================================================================
//
// `Stdin`/`Stdout`/`Stderr` wrap the inherited fds 0/1/2 over the raw
// `t_read`/`t_write` SVC wrappers. They are zero-sized and do NOT own the
// fd: dropping one does NOT close 0/1/2 (unlike `fs::File`, whose Drop
// closes its handle). This matters -- a `File` over fd 1 would close the
// inherited stdout on drop.
//
// v1.0 CAVEAT (DOC-GAP G06): a standalone native program has no
// terminal-backed fd 0/1/2. A read/write succeeds only when a parent wired
// the fd (a pipeline element, a redirect, or an inherited fd). A write to an
// unwired fd 1 returns an error (callers that must not fail standalone use
// the best-effort `print!`/`println!` macros, which swallow the error). For
// an always-on diagnostic channel use `crate::t_putstr` (the kernel UART).

const FD_STDIN: i64 = 0;
const FD_STDOUT: i64 = 1;
const FD_STDERR: i64 = 2;

/// Handle to the process's standard input (fd 0). Non-owning; see the
/// module's "Standard streams" note for the v1.0 unwired-fd caveat.
pub struct Stdin;
/// Handle to the process's standard output (fd 1). Non-owning.
pub struct Stdout;
/// Handle to the process's standard error (fd 2). Non-owning.
pub struct Stderr;

/// A handle to fd 0 (standard input).
#[inline]
pub fn stdin() -> Stdin {
    Stdin
}
/// A handle to fd 1 (standard output).
#[inline]
pub fn stdout() -> Stdout {
    Stdout
}
/// A handle to fd 2 (standard error).
#[inline]
pub fn stderr() -> Stderr {
    Stderr
}

/// Probe whether fd 1 (stdout) is a live, writable handle. A zero-length
/// `SYS_WRITE` validates the handle without emitting bytes: the kernel
/// checks the fd even for len 0 (POSIX "bad fd is EBADF regardless of
/// len"), and `devcons_write` / pipe writes short-circuit n==0. Returns
/// true iff fd 1 can be inherited by a spawned child -- the signal a shell
/// uses to decide whether external commands inherit the console (visible
/// output) or get the fd-less `Stdio::Piped`-then-drop convention. Cheaper
/// + more universal than `fstat` (the console Dev has no `stat_native`).
#[inline]
pub fn stdout_is_live() -> bool {
    let mut s = Stdout;
    s.write(&[]).is_ok()
}

impl Read for Stdin {
    fn read(&mut self, buf: &mut [u8]) -> Result<usize> {
        // SAFETY: buf is a valid user-VA byte slice; fd 0 is read-only here.
        let rc = unsafe { t_read(FD_STDIN, buf.as_mut_ptr(), buf.len()) };
        Error::from_syscall_return(rc).map(|n| n as usize)
    }
}

impl Write for Stdout {
    fn write(&mut self, buf: &[u8]) -> Result<usize> {
        // SAFETY: buf is a valid user-VA byte slice.
        let rc = unsafe { t_write(FD_STDOUT, buf.as_ptr(), buf.len()) };
        Error::from_syscall_return(rc).map(|n| n as usize)
    }
    fn flush(&mut self) -> Result<()> {
        // Unbuffered: every write is an immediate SYS_WRITE.
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

impl crate::poll::AsFd for Stdin {
    #[inline]
    fn as_raw_fd(&self) -> i32 {
        FD_STDIN as i32
    }
}
impl crate::poll::AsFd for Stdout {
    #[inline]
    fn as_raw_fd(&self) -> i32 {
        FD_STDOUT as i32
    }
}
impl crate::poll::AsFd for Stderr {
    #[inline]
    fn as_raw_fd(&self) -> i32 {
        FD_STDERR as i32
    }
}

/// Copy the entire contents of `reader` into `writer` using an 8 KiB
/// staging buffer. Returns the total number of bytes copied. The workhorse
/// for `cat`/`tee`-style filters. Mirrors `std::io::copy`.
pub fn copy<R: Read + ?Sized, W: Write + ?Sized>(reader: &mut R, writer: &mut W) -> Result<u64> {
    let mut buf = [0u8; 8 * 1024];
    let mut total = 0u64;
    loop {
        match reader.read(&mut buf)? {
            0 => return Ok(total),
            n => {
                writer.write_all(&buf[..n])?;
                total += n as u64;
            }
        }
    }
}

/// Best-effort write of all of `buf` to stdout (fd 1); errors swallowed.
/// The unconditional-output workhorse for the coreutils -- a standalone run
/// with an unwired fd 1 (G06) should not spuriously fail. Pairs with the
/// `print!` / `println!` macros for the byte-slice (non-formatted) case.
#[inline]
pub fn out(buf: &[u8]) {
    let _ = stdout().write_all(buf);
}

/// Best-effort write of all of `buf` to stderr (fd 2); errors swallowed.
#[inline]
pub fn err(buf: &[u8]) {
    let _ = stderr().write_all(buf);
}

/// A stdout sink that LATCHES the first write error: once a write to fd 1
/// fails, every later `put` / `write!` is a no-op and `failed()` stays true.
/// Lets a coreutil emit its payload without `?`-threading a `Result` through
/// every formatting helper, then report a single "write error" + a nonzero
/// exit at the end -- the cat/head discipline, which `io::out` (swallow and
/// keep trying) does not provide. Use this for PAYLOAD (the data the utility
/// exists to produce); a silent truncation that still exits 0 is the data-loss
/// class this guards against. The `core::fmt::Write` impl is infallible by
/// design (it routes through the latch), so `write!(sink, ...)` never returns
/// an error to handle -- check `failed()` once when done.
pub struct OutSink {
    out: Stdout,
    failed: bool,
}

impl OutSink {
    #[inline]
    #[must_use]
    pub fn new() -> Self {
        OutSink { out: Stdout, failed: false }
    }

    /// Write all of `buf` to stdout. A no-op once a prior write has failed.
    #[inline]
    pub fn put(&mut self, buf: &[u8]) {
        if self.failed {
            return;
        }
        if self.out.write_all(buf).is_err() {
            self.failed = true;
        }
    }

    /// True once any write has failed -- the caller's nonzero-exit signal.
    #[inline]
    #[must_use]
    pub fn failed(&self) -> bool {
        self.failed
    }
}

impl Default for OutSink {
    #[inline]
    fn default() -> Self {
        Self::new()
    }
}

impl core::fmt::Write for OutSink {
    #[inline]
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        self.put(s.as_bytes());
        // Always Ok: the latched `failed` flag, not this return, carries the
        // status, so the `write!` machinery never aborts a partial format.
        Ok(())
    }
}

/// Upper bound on a single `slurp`. Leaves headroom under the 4 MiB
/// userspace heap (`alloc::INITIAL_HEAP_SIZE`) for the caller's derived
/// working set (sort's line-ref vector, etc.), so an oversized input yields
/// a graceful `NoMemory` error instead of an allocator OOM-abort -- with
/// `panic = "abort"` an OOM kills the Proc, which for the session shell is a
/// logout. Streaming utilities should prefer chunked reads; `slurp` is only
/// for whole-input loads that genuinely need the bytes resident.
pub const SLURP_CAP: usize = 2 * 1024 * 1024;

/// Read all of `reader` into a fresh `Vec`, bounded by `SLURP_CAP`. The
/// "load the whole input" helper for `wc`/`sort`/`cut`/`uniq`/`grep`/`cmp`/
/// `tail`. Returns `NoMemory` if the input exceeds the cap.
pub fn slurp<R: Read + ?Sized>(reader: &mut R) -> Result<Vec<u8>> {
    slurp_capped(reader, SLURP_CAP)
}

/// Like `slurp` but with an explicit byte limit. Reads until EOF or `limit`
/// bytes consumed, returning `NoMemory` the moment the input would exceed
/// `limit` (the caller fails gracefully rather than OOM-aborting on an
/// unbounded input). Chunked, so it never over-allocates past the limit.
pub fn slurp_capped<R: Read + ?Sized>(reader: &mut R, limit: usize) -> Result<Vec<u8>> {
    let mut v = Vec::new();
    let mut buf = [0u8; 8 * 1024];
    loop {
        let n = reader.read(&mut buf)?;
        if n == 0 {
            break;
        }
        if v.len() + n > limit {
            return Err(Error::NoMemory);
        }
        v.extend_from_slice(&buf[..n]);
    }
    Ok(v)
}
