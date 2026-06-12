//! libthyla-rs::identity — the calling Proc's own identity (LS-K).
//!
//! Thin safe wrappers over `SYS_GETPID` / `SYS_GETUID` / `SYS_GETGID`. Each
//! returns the calling Proc's durable field (A-1a): `pid`, `principal_id`, and
//! `primary_gid`. The values are read-only introspection -- no capability, no
//! mutation. All are `< 2^32` so the cast from the `i64` syscall return is
//! lossless. See ARCH §22.6.
//!
//! Names: `whoami` / `id` render the numeric `uid` / `gid` at v1.0; uid->name
//! resolution (a corvus `NAME_LOOKUP` verb or a kernel `principal_name`) and
//! `getgroups` (the supplementary-group set) are recorded v1.x seams.

use crate::{t_getgid, t_getpid, t_getuid};

/// The calling Proc's pid (always > 0).
pub fn pid() -> u32 {
    (unsafe { t_getpid() }) as u32
}

/// The calling Proc's user id (its durable `principal_id`).
pub fn uid() -> u32 {
    (unsafe { t_getuid() }) as u32
}

/// The calling Proc's primary group id (its durable `primary_gid`).
pub fn gid() -> u32 {
    (unsafe { t_getgid() }) as u32
}
