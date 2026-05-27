// libthyla-rs::rand — CSPRNG access via SYS_GETRANDOM.
//
// Foundation chunk: U-2g per docs/UTOPIA-SHELL-DESIGN.md section 15.6.12.
//
// The kernel CSPRNG is seeded from ARM RNDR at boot. Caller must hold
// `CAP_CSPRNG_READ`. Per-call cap is `SYS_RW_MAX` (4 KiB) at v1.0;
// `fill_bytes` loops to fill arbitrary-length buffers.
//
// The kernel best-effort zeros the partial range on a mid-stream
// uaccess failure before returning -1, so a caller seeing
// `Error::BadAddress` should still NOT trust the buffer's prior
// contents (the kernel just wrote zeros).
//
// At v1.0 `flags` is reserved (must be 0). The kernel has no
// `GRND_NONBLOCK` analog -- the CSPRNG is always available once
// seeded.

use crate::err::{Error, Result};
use crate::t_getrandom;

/// Per-call byte limit (mirrors `SYS_RW_MAX` from the kernel).
pub const GETRANDOM_MAX: usize = 4096;

/// Fill `buf` with CSPRNG bytes. Returns `len` on success.
///
/// `buf.len()` must be <= `GETRANDOM_MAX`. For larger buffers, use
/// `fill_bytes` (which loops internally).
///
/// Errors:
///   - `Error::NotPermitted`: caller lacks `CAP_CSPRNG_READ`.
///   - `Error::InvalidArgument`: `buf.len() > GETRANDOM_MAX`.
///   - `Error::BadAddress`: mid-stream uaccess fault; the kernel
///     zeroed the partial range (do NOT trust the buffer).
pub fn getrandom(buf: &mut [u8]) -> Result<usize> {
    if buf.len() > GETRANDOM_MAX {
        return Err(Error::InvalidArgument);
    }
    if buf.is_empty() {
        return Ok(0);
    }
    // SAFETY: `buf` borrows a writable slice of `buf.len()` bytes in
    // user-VA memory for the syscall's duration.
    let rc = unsafe { t_getrandom(buf.as_mut_ptr(), buf.len(), 0) };
    if rc < 0 {
        // Kernel returns -1 on cap missing / non-zero flags /
        // oversized len / uaccess fault. We've already bounded
        // length so the realistic cause is cap-missing.
        return Err(Error::NotPermitted);
    }
    Ok(rc as usize)
}

/// Fill `buf` entirely with CSPRNG bytes, looping internally past the
/// `GETRANDOM_MAX` per-call cap. Returns `Ok(())` if every byte was
/// filled; otherwise the underlying error.
///
/// On error mid-loop, the partial prefix may hold valid CSPRNG bytes
/// AND a trailing region of zeros (from the kernel's best-effort
/// wipe). Caller MUST NOT trust the buffer on error.
pub fn fill_bytes(buf: &mut [u8]) -> Result<()> {
    let mut filled = 0usize;
    while filled < buf.len() {
        let chunk = (buf.len() - filled).min(GETRANDOM_MAX);
        let n = getrandom(&mut buf[filled..filled + chunk])?;
        if n == 0 {
            // Kernel returned 0 -- structurally impossible at v1.0
            // (getrandom either fills the whole request or returns
            // -1). Defense-in-depth: treat as I/O error.
            return Err(Error::Io);
        }
        filled += n;
    }
    Ok(())
}
