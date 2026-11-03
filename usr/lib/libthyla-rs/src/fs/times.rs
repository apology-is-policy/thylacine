// libthyla-rs::fs::times — FileTimes, the std::fs::FileTimes mirror over
// SYS_WSTAT's time bits (G12).
//
// Each time is left alone, set to the file server's clock, or set to a given
// instant. std offers only the first and the last. The server's clock is a
// separate choice because it is a separate permission: POSIX lets a caller
// with write permission, not just the owner, set BOTH times to now (the
// null-times touch), while any explicit time -- `SystemTime::now()` included --
// needs the owner. `touch` relies on the difference.
//
// The kernel's stat carries whole seconds, so a time read back through
// `Metadata` has lost the nanoseconds a server may keep.

use crate::err::{Error, Result};
use crate::time::{SystemTime, TimeSpec};
use crate::{t_wstat_times, T_WSTAT_ATIME, T_WSTAT_ATIME_SET, T_WSTAT_MTIME, T_WSTAT_MTIME_SET};

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
enum Stamp {
    Keep,
    ServerNow,
    At(SystemTime),
}

/// The access and modification times to set on a file: `File::set_times`,
/// `fs::set_times`. A time not set is left unchanged.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct FileTimes {
    accessed: Stamp,
    modified: Stamp,
}

impl Default for FileTimes {
    fn default() -> Self {
        Self::new()
    }
}

impl FileTimes {
    /// Both times left unchanged.
    #[must_use]
    pub const fn new() -> Self {
        FileTimes { accessed: Stamp::Keep, modified: Stamp::Keep }
    }

    /// Set the access time to `t`. Needs the file's owner.
    #[must_use]
    pub const fn set_accessed(mut self, t: SystemTime) -> Self {
        self.accessed = Stamp::At(t);
        self
    }

    /// Set the modification time to `t`. Needs the file's owner.
    #[must_use]
    pub const fn set_modified(mut self, t: SystemTime) -> Self {
        self.modified = Stamp::At(t);
        self
    }

    /// Set the access time to the file server's clock. With
    /// `set_modified_now` as well, write permission on the file suffices;
    /// alone, it needs the owner.
    #[must_use]
    pub const fn set_accessed_now(mut self) -> Self {
        self.accessed = Stamp::ServerNow;
        self
    }

    /// Set the modification time to the file server's clock. The same
    /// permission rule as `set_accessed_now`.
    #[must_use]
    pub const fn set_modified_now(mut self) -> Self {
        self.modified = Stamp::ServerNow;
        self
    }
}

// The SYS_WSTAT mask and the two records x6 points at. A Keep entry adds no
// bit and leaves its record zero (the kernel reads only _SET entries).
fn encode(t: FileTimes) -> Result<(u32, [TimeSpec; 2])> {
    let mut valid = 0;
    let mut ts = [TimeSpec { tv_sec: 0, tv_nsec: 0 }; 2];
    let slots = [
        (t.accessed, T_WSTAT_ATIME, T_WSTAT_ATIME_SET),
        (t.modified, T_WSTAT_MTIME, T_WSTAT_MTIME_SET),
    ];
    for (i, (stamp, bit, set)) in slots.into_iter().enumerate() {
        match stamp {
            Stamp::Keep => {}
            Stamp::ServerNow => valid |= bit,
            Stamp::At(at) => {
                let d = at.since_epoch();
                // The kernel's tv_sec is signed; a SystemTime past i64::MAX
                // seconds has no encoding.
                let sec = i64::try_from(d.as_secs()).map_err(|_| Error::InvalidArgument)?;
                ts[i] = TimeSpec { tv_sec: sec, tv_nsec: i64::from(d.subsec_nanos()) };
                valid |= bit | set;
            }
        }
    }
    Ok((valid, ts))
}

pub(crate) fn set_times_fd(fd: i32, t: FileTimes) -> Result<()> {
    let (valid, ts) = encode(t)?;
    // Nothing to change succeeds without a syscall, as POSIX utimensat does
    // for two UTIME_OMIT; SYS_WSTAT itself refuses an empty mask.
    if valid == 0 {
        return Ok(());
    }
    // SAFETY: t_wstat_times is the SYS_WSTAT SVC wrapper; `ts` is a live
    // 32-byte local the kernel only reads, and `fd` is the caller's open handle.
    let rc = unsafe { t_wstat_times(i64::from(fd), valid, ts.as_ptr() as u64) };
    Error::from_syscall_return(rc).map(|_| ())
}
