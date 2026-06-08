// libthyla-rs — Thylacine userspace runtime (Rust side).
//
// Mirror of usr/lib/libt/ on the Rust side. Provides:
//   - Syscall numbers (T_SYS_*) — kernel/include/thylacine/syscall.h.
//   - Right bits (T_RIGHT_*)    — kernel/include/thylacine/handle.h.
//   - Prot bits (T_PROT_*)      — kernel/include/thylacine/vma.h.
//   - SVC wrappers              — t_exits, t_puts, t_putstr, t_mmio_create,
//                                 t_mmio_map, t_irq_create, t_irq_wait.
//   - _start                    — global_asm; ENTRY(_start) in
//                                 usr/scripts/aarch64-userspace.ld keeps it
//                                 alive across the rlib boundary so the
//                                 binary doesn't need to redeclare it.
//   - #[panic_handler]          — default impl tail-calls t_exits(1). A
//                                 binary that wants a richer handler depends
//                                 on libthyla-rs with `default-features =
//                                 false` and provides its own (Phase 5+
//                                 hook).
//
// Binaries link libthyla-rs and define `#[no_mangle] pub extern "C" fn
// rs_main() -> i64` as their entry point. _start invokes rs_main and
// tail-calls SYS_EXITS with the return value.
//
// Extracted at P4-Ic4 from usr/hello-rs/src/main.rs (where these lived
// inline at P4-Ia2) to unblock P4-Ic5 (the virtio-blk driver crate).

#![no_std]

use core::arch::{asm, global_asm};
use core::panic::PanicInfo;

// =============================================================================
// libthyla-rs uplift — typed Rust modules (U-2a..U-2-test).
// =============================================================================
//
// Per docs/UTOPIA-SHELL-DESIGN.md §15, libthyla-rs is being uplifted
// from "raw SVC wrappers + constants" into an idiomatic Rust API
// covering every Thylacine kernel surface. Each U-2X sub-chunk lands
// one module; existing callers continue to use the bare wrappers
// + T_* constants below for backwards compatibility during the
// transition.
//
// U-2a: err + handle.
// U-2b: alloc.
// U-2c-path: fs::{Path, PathBuf, Components}.
// U-2c-io: io::{Read, Write, Seek, BufRead, BufReader, Cursor, SeekFrom} + fs::File.
// U-2c-fs: fs::{OpenOptions, Metadata} + free functions (metadata, exists, is_file, is_dir).
// U-2d: process::{Command, Child, ExitStatus, Stdio, pipe}.
// U-2e: notes + poll.
// U-2f: territory + cap.
// U-2g: torpor + time + rand + thread.
// U-2h-ninep: ninep (9P2000.L server-side codec, lifted from corvus).
// U-2h-hardware: hardware::{Mmio, Irq, Dma} (typed RAII over KObj_MMIO/IRQ/DMA).
//
// `extern crate alloc` brings the standard `alloc` crate (String,
// Vec, Box, Borrow, ToOwned) into libthyla-rs's namespace so
// internal modules (t::fs::path's PathBuf wraps String, etc.) can
// name them. Renamed to `alloc_crate` because libthyla-rs's own
// heap allocator module is `pub mod alloc`; the rename avoids
// resolution collision at every internal `alloc::` path.
//
// CONSEQUENCE FOR CONSUMERS: every binary that links libthyla-rs
// must declare a `#[global_allocator]`. The canonical choice is
// `libthyla_rs::alloc::ThylaAlloc` (one-liner; see the alloc-smoke
// header for the pattern). Sets the project-wide convention that
// every native Thylacine Rust binary opts in to ThylaAlloc unless
// it has a specific reason to differ (corvus uses a static-BSS
// allocator for its own pre-heap startup; the virtio-* drivers
// and hello-rs et al. use ThylaAlloc even when they never allocate
// -- the symbol just needs to resolve at link time).
extern crate alloc as alloc_crate;

pub mod err;
pub mod handle;
pub mod alloc;
pub mod cap;
pub mod env;
pub mod fs;
pub mod hardware;
pub mod io;
pub mod loom;
pub mod ninep;
pub mod notes;
pub mod poll;
pub mod process;
pub mod rand;
pub mod territory;
pub mod thread;
pub mod time;
pub mod torpor;

// =============================================================================
// Syscall numbers — MUST mirror kernel/include/thylacine/syscall.h.
// =============================================================================

pub const T_SYS_EXITS: u64           = 0;
pub const T_SYS_PUTS: u64            = 1;
pub const T_SYS_MMIO_CREATE: u64     = 2;
pub const T_SYS_IRQ_CREATE: u64      = 3;
pub const T_SYS_IRQ_WAIT: u64        = 4;
pub const T_SYS_MMIO_MAP: u64        = 5;
pub const T_SYS_DMA_CREATE: u64      = 6;
pub const T_SYS_DMA_MAP: u64         = 7;
// P5-fd-* family — byte I/O surface used by corvus's Spoor server loop.
pub const T_SYS_PIPE: u64            = 8;
pub const T_SYS_READ: u64            = 9;
pub const T_SYS_WRITE: u64           = 10;
pub const T_SYS_CLOSE: u64           = 11;
// P5-spawn-wait: reap one zombie child + return its (pid, status). Backs
// t::process::Child::wait (U-2d).
pub const T_SYS_WAIT_PID: u64        = 22;
// P6-pouch-stratumd-boot 16b-alpha: combined spawn with argv pass-through.
// Subsumes all earlier SYS_SPAWN_* surfaces; backs t::process::Command::spawn
// (U-2d). Takes a single user-VA pointer to a struct TSpawnArgs (80 bytes).
pub const T_SYS_SPAWN_FULL_ARGV: u64 = 49;
// P5-corvus-syscalls (kernel side at 0db0dcf/d10d4ee). v1.0 hardening
// syscalls used by /sbin/corvus startup.
pub const T_SYS_MLOCKALL: u64        = 16;
pub const T_SYS_SET_DUMPABLE: u64    = 17;
pub const T_SYS_SET_TRACEABLE: u64   = 18;
pub const T_SYS_EXPLICIT_BZERO: u64  = 19;
pub const T_SYS_GETRANDOM: u64       = 20;
pub const T_SYS_SPAWN_FULL: u64      = 25;
// /srv mechanism — kernel-owned 9P transport. 26 (post) + 30 (connect) are
// RETIRED (stalk-3c): posting is SYS_WALK_CREATE on a /srv dir (create=post),
// connecting is SYS_OPEN on /srv/<name> (open=connect); accept + peer remain.
pub const T_SYS_SRV_ACCEPT: u64      = 27;
pub const T_SYS_SRV_PEER: u64        = 28;
// P5-poll-a: the multi-fd wait/wake primitive. Backs corvus's main loop
// (a single thread serving N /srv/corvus connections) and the future
// musl `poll(2)` shim. ABI matches Linux event values for shim triviality.
pub const T_SYS_POLL: u64            = 29;
// P5-corvus-srv-impl-b3a: spawn_full + perm_flags (atomic kernel stamp
// of PROC_FLAG_* bits on the spawned child, gated on caller console-attach).
pub const T_SYS_SPAWN_WITH_PERMS: u64 = 31;
// P5-hostowner-b-b: register / redeem a pending cap grant via the `cap`
// device (CORVUS-DESIGN.md §5.5.1). Bridges the kernel cap-grant table
// to userspace until a generic t_open lands.
pub const T_SYS_CAP_GRANT: u64        = 32;
pub const T_SYS_CAP_USE: u64          = 33;

// P6-pouch-mem: anonymous-memory grow/shrink, the Tier-1 native memory
// primitive (ARCHITECTURE.md §6.5). Backs t::alloc's #[global_allocator]
// in libthyla-rs (U-2b) and every libt-equivalent userspace allocator.
pub const T_SYS_BURROW_ATTACH: u64    = 37;
pub const T_SYS_BURROW_DETACH: u64    = 38;

// P6-pouch-signals-impl (sub-chunk 13a): the note delivery primitive.
// Async-event mechanism, Plan 9-shaped, FD-first per NOVEL.md §3.1.
// Backs t::notes (U-2e). SYS_NOTIFY + SYS_NOTED are the async-handler
// opt-in path (libc-compat); at v1.0 libthyla-rs exposes the canonical
// fd-shaped path only (open self -> read records -> poll integration).
pub const T_SYS_NOTE_OPEN: u64        = 44;
pub const T_SYS_NOTIFY: u64           = 45;
pub const T_SYS_NOTED: u64            = 46;
pub const T_SYS_POSTNOTE: u64         = 47;
pub const T_SYS_NOTE_MASK: u64        = 48;

// P5-mount-syscall + P5-chroot-syscall + P6-16c-pivot_root: namespace
// composition syscalls. Plan 9 mount-table semantics adapted to fd-
// shaped Spoor sources. Backs t::territory (U-2f).
pub const T_SYS_MOUNT: u64            = 14;
pub const T_SYS_UNMOUNT: u64          = 15;
pub const T_SYS_CHROOT: u64           = 35;
pub const T_SYS_ATTACH_9P_SRV: u64    = 52;     // 16c: 9P attach over a byte-mode /srv conn
pub const T_SYS_PIVOT_ROOT: u64       = 53;

// P6-pouch-wait-addr (sub-chunk 8) + P6-pouch-threads (sub-chunk 9a):
// wait-on-address + kernel-level thread spawn/exit. Futex-style
// no-lost-wakeup primitive (the lock-acquire serializing event takes
// the kernel `torpor_lock`); thread spawn shares pgtable_root + ASID
// + handle table with the calling Proc. Backs t::torpor + t::thread
// (U-2g) and the pouch boundary-line patches.
pub const T_SYS_TORPOR_WAIT: u64      = 39;
pub const T_SYS_TORPOR_WAKE: u64      = 40;
pub const T_SYS_THREAD_SPAWN: u64     = 41;
pub const T_SYS_THREAD_EXIT: u64      = 42;
// P6-pouch-kernel-auxv: SYS_SET_TID_ADDRESS — installs the calling
// thread's "clear-child-tid" address. SYS_THREAD_EXIT stores 0 there
// + torpor_wakes UINT32_MAX (the canonical join-on-exit signal).
// Backs t::thread's join helpers + the U-2-test thread roundtrip.
pub const T_SYS_SET_TID_ADDRESS: u64  = 36;

// SYS_TORPOR_WAIT timeout sentinels (mirror kernel/torpor.c).
//   < 0 = block indefinitely
//   = 0 = probe (returns -ETIMEDOUT immediately if value still matched)
//   > 0 = block at most `timeout_us` microseconds.
// TORPOR_MAX_TIMEOUT_US caps explicit values at 1 hour.
pub const T_TORPOR_TIMEOUT_INDEFINITE: i64 = -1;
pub const T_TORPOR_MAX_TIMEOUT_US: i64     = 3600 * 1_000_000; // 1 hour

// P5-stratumd-stub-bringup-e1: walk-and-open one path component from a
// Spoor (or from the territory root via T_WALK_OPEN_FROM_ROOT sentinel).
// Multi-component paths are walked per-component by the caller.
// P6-pouch-stratumd-boot sub-chunk 16b-γ-syscalls: lseek + fstat for
// POSIX file-position semantics. Backs t::fs::File (U-2c-io) and
// t::fs::Metadata (U-2c-fs).
pub const T_SYS_WALK_OPEN: u64        = 34;
pub const T_SYS_FSTAT: u64            = 50;
pub const T_SYS_LSEEK: u64            = 51;
// FS-mutation foundation (IDENTITY-DESIGN.md section 9.2): create-then-open,
// durability barrier, directory enumeration.
pub const T_SYS_WALK_CREATE: u64      = 54;
pub const T_SYS_FSYNC: u64            = 55;
pub const T_SYS_READDIR: u64          = 56;
// FS-gamma (IDENTITY-DESIGN.md section 9.3): atomic rename/move + remove.
pub const T_SYS_RENAME: u64           = 57;
pub const T_SYS_UNLINK: u64           = 58;
// A-2a (IDENTITY-DESIGN.md section 9.5): chmod/chown via Tsetattr.
pub const T_SYS_WSTAT: u64            = 59;
pub const T_SYS_EXIT_GROUP: u64       = 60;
pub const T_SYS_CAP_GRANT_CLEARANCE: u64 = 61;  // A-4a clearance grant-side bridge
pub const T_SYS_OPEN: u64             = 65;     // A-5b-0/stalk-1 multi-component open
// Loom -- the io_uring-inverted 9P ring transport (docs/LOOM.md). Backs the
// native t::loom::Ring API (Loom-6d). SETUP maps the SQ/CQ Burrow + reports
// geometry; REGISTER installs fixed handles / pins buffers; ENTER submits +
// reaps.
pub const T_SYS_LOOM_SETUP: u64       = 66;
pub const T_SYS_LOOM_REGISTER: u64    = 67;
pub const T_SYS_LOOM_ENTER: u64       = 68;
// SYS_UNLINK flags: rmdir an empty directory vs unlink a non-directory.
// Mirrors the kernel's SYS_UNLINK_REMOVEDIR / wire P9_UNLINK_AT_REMOVEDIR.
pub const T_UNLINK_REMOVEDIR: u32     = 0x200;

// SYS_WSTAT valid-mask bits (A-2a). Mirror the kernel T_WSTAT_* / wire
// P9_SETATTR_* bits. chmod sets T_WSTAT_MODE; chown sets T_WSTAT_UID|GID.
pub const T_WSTAT_MODE: u32           = 0x1;
pub const T_WSTAT_UID: u32            = 0x2;
pub const T_WSTAT_GID: u32            = 0x4;
pub const T_WSTAT_MODE_MASK: u32      = 0o777;

// SYS_WALK_OPEN omode bits — must mirror SYS_WALK_OPEN_OMODE_VALID in
// kernel/include/thylacine/syscall.h. Plan 9 OREAD/OWRITE/ORDWR/OEXEC
// in the low two bits; OTRUNC (truncate-on-open for write-shaped opens)
// in bit 4. Bits outside 0x13 are rejected by the kernel.
pub const T_OREAD: u32                = 0;
pub const T_OWRITE: u32               = 1;
pub const T_ORDWR: u32                = 2;
pub const T_OEXEC: u32                = 3;
pub const T_OTRUNC: u32               = 0x10;
// FS-delta (IDENTITY-DESIGN.md §9.4): walk-without-open (Linux O_PATH / Plan 9
// walk). SYS_WALK_OPEN with T_OPATH returns a NON-OPENED, walkable KObj_Spoor --
// the valid base for creating/walking/renaming/unlinking children + a valid
// SYS_CHROOT target (a normally-opened handle is not: 9P forbids Twalk from an
// opened fid). Access bits are ignored when set.
pub const T_OPATH: u32                = 0x80;

// SYS_WALK_CREATE perm: low 9 bits = POSIX mode; DMDIR selects a directory.
// Must mirror SYS_WALK_CREATE_PERM_VALID / SYS_WALK_CREATE_DMDIR in the kernel.
pub const T_WALK_CREATE_DMDIR: u32    = 0x8000_0000;
// DMSRVBYTE (stalk-3b/3c; STALK-DESIGN.md §5.3 / D6): on a SYS_WALK_CREATE
// against a /srv directory, this perm bit posts the new service in BYTE mode;
// its absence posts 9P mode. Mirrors SYS_WALK_CREATE_DMSRVBYTE in the kernel.
pub const T_WALK_CREATE_DMSRVBYTE: u32 = 0x0200_0000;

// SYS_WALK_OPEN sentinel for "walk from the calling Proc's territory
// root spoor" (P5-stratumd-stub-bringup-e2). Passed as spoor_fd when
// no source Spoor is held; the kernel substitutes the Territory's
// root_spoor.
pub const T_WALK_OPEN_FROM_ROOT: i64  = -1;

// Maximum single-component name length for SYS_WALK_OPEN. Multi-
// component paths split at '/' and call SYS_WALK_OPEN per component.
pub const T_WALK_OPEN_NAME_MAX: usize = 64;

// SYS_LSEEK whence values — must mirror kernel/include/thylacine/syscall.h.
pub const T_SEEK_SET: u32             = 0;
pub const T_SEEK_CUR: u32             = 1;
pub const T_SEEK_END: u32             = 2;

// POSIX-shaped mode bits returned in struct t_stat.mode — must mirror
// kernel/include/thylacine/syscall.h. Subset that v1.0 Devs populate
// (regular file / directory / character device). T_S_IFMT is the mask
// for extracting the type.
pub const T_S_IFMT:  u32              = 0o170000;
pub const T_S_IFREG: u32              = 0o100000;
pub const T_S_IFDIR: u32              = 0o040000;
pub const T_S_IFCHR: u32              = 0o020000;

// SYS_SPAWN_FULL_ARGV bounds — must mirror SYS_SPAWN_ARGV_MAX +
// SYS_SPAWN_ARGV_DATA_MAX in kernel/include/thylacine/syscall.h.
pub const T_SYS_SPAWN_ARGV_MAX: usize        = 16;
pub const T_SYS_SPAWN_ARGV_DATA_MAX: usize   = 4096;

// Maximum binary name length (mirror SYS_SPAWN_NAME_MAX).
pub const T_SPAWN_NAME_MAX: usize    = 64;

// Maximum inherited-fd count (mirror SYS_SPAWN_MAX_FDS).
pub const T_SPAWN_MAX_FDS: usize     = 16;

// (T_CAP_* constants are defined earlier in this file alongside the
// other rights/caps mirrors; not re-declared here.)

// SYS_SPAWN_FULL_ARGV argument record (80 bytes; ABI-pinned per
// kernel/include/thylacine/syscall.h + libt mirror). #[repr(C)] so the
// kernel reads our struct byte-for-byte. A-1a appended the identity block
// (56 -> 80); leave identity_flags == 0 to spawn an INHERIT child (setting
// T_SPAWN_IDENTITY_SET requires the caller to hold CAP_SET_IDENTITY).
#[repr(C)]
pub struct TSpawnArgs {
    pub name_va:        u64, // 0
    pub argv_data_va:   u64, // 8
    pub fd_list_va:     u64, // 16
    pub name_len:       u32, // 24
    pub argv_data_len:  u32, // 28
    pub argc:           u32, // 32
    pub fd_count:       u32, // 36
    pub perm_flags:     u32, // 40
    pub _pad_envp:      u32, // 44 — must be 0 at v1.0
    pub cap_mask:       u64, // 48
    pub principal_id:   u32, // 56 — A-1a (honored iff identity_flags & SET)
    pub primary_gid:    u32, // 60 — A-1a
    pub supp_gids_va:   u64, // 64 — A-1a (user-VA of supp_gid_count u32s)
    pub supp_gid_count: u32, // 72 — A-1a (0..15)
    pub identity_flags: u32, // 76 — A-1a (T_SPAWN_IDENTITY_SET)
}
// Compile-time pin matching kernel's _Static_assert(sizeof(...) == 80).
const _: () = assert!(core::mem::size_of::<TSpawnArgs>() == 80);

// A-1a: identity_flags bits (mirror SPAWN_IDENTITY_* in the kernel header).
pub const T_SPAWN_IDENTITY_SET: u32 = 1 << 0;

// A-1a: reserved principal-id / gid sentinels (mirror <thylacine/proc.h>).
// None is privileged (I-22); real ids are corvus-assigned in [1, 0xFFFFFFFD].
pub const T_PRINCIPAL_INVALID: u32 = 0;
pub const T_PRINCIPAL_SYSTEM:  u32 = 0xFFFF_FFFE;
pub const T_PRINCIPAL_NONE:    u32 = 0xFFFF_FFFF;
pub const T_GID_INVALID:       u32 = 0;
pub const T_GID_SYSTEM:        u32 = 0xFFFF_FFFE;
pub const T_GID_NONE:          u32 = 0xFFFF_FFFF;

// SYS_SPAWN_WITH_PERMS perm_flags — must mirror SPAWN_PERM_* in
// kernel/include/thylacine/syscall.h.
pub const T_SPAWN_PERM_MAY_POST_SERVICE: u64 = 1 << 0;
// A-4c-2: records the spawned child as the trusted login authority (the SAK
// re-grant target). Granting it requires the parent to be console-attached.
pub const T_SPAWN_PERM_CONSOLE_TRUSTED: u64 = 1 << 1;

// poll event bits — MUST mirror POLL* in kernel/include/thylacine/poll.h.
// Linux values; the future musl shim is a no-op.
pub const T_POLLIN: i16   = 0x001;
pub const T_POLLOUT: i16  = 0x004;
pub const T_POLLERR: i16  = 0x008;
pub const T_POLLHUP: i16  = 0x010;
pub const T_POLLNVAL: i16 = 0x020;

// =============================================================================
// Mount flags ABI mirror — MUST match kernel/include/thylacine/territory.h.
// =============================================================================
//
// Plan 9 mount semantics. At v1.0 only MREPL has distinguished semantics
// in the kernel (MBEFORE/MAFTER are accepted but treated as additive at
// the C-API level; MCREATE is reserved). Backs t::territory (U-2f).

pub const T_MREPL:   u32              = 0x0001;
pub const T_MBEFORE: u32              = 0x0002;
pub const T_MAFTER:  u32              = 0x0004;
pub const T_MCREATE: u32              = 0x0008;

// path_id_t — abstract path token used by the mount/unmount/bind
// surface at v1.0. The kernel comment is the canonical reference:
// "abstract path token at v1.0 — the same numeric ID used by bind/
// unbind in the existing PgrpBind / PgrpMount C-API. String-path
// resolution lands with the fd-syscall walk subsystem in a later
// chunk." Userspace callers establish their own path_id <-> string
// convention until then.
pub type TPathId = u32;

// =============================================================================
// Notes ABI mirror — MUST match kernel/include/thylacine/notes.h.
// =============================================================================

// NOTE_NAME_MAX — bytes including the trailing NUL. The note name proper
// is bounded to NOTE_NAME_MAX-1 = 15 bytes; SYS_POSTNOTE rejects shorter
// non-printable + empty names.
pub const T_NOTE_NAME_MAX: usize  = 16;

// NOTE_BIT_* — bit position of each supported note in the per-Thread
// note_mask. Bit set = defer delivery of that note. Per-Thread, NOT
// per-fd (matches POSIX pthread_sigmask semantics).
pub const T_NOTE_BIT_INTERRUPT:  u8 = 0;
pub const T_NOTE_BIT_KILL:       u8 = 1;
pub const T_NOTE_BIT_PIPE:       u8 = 2;
pub const T_NOTE_BIT_CHILD_EXIT: u8 = 3;
pub const T_NOTE_BIT_SNARE:      u8 = 4;

// NOTE_MASK_SUPPORTED — the union of every NOTE_BIT_* the kernel knows
// about today. Setting bits outside this is tolerated (no-op) so future
// note additions don't break old userspace; SYS_POSTNOTE with an
// unsupported note NAME returns -1.
pub const T_NOTE_MASK_SUPPORTED: u64 = 0x1f;

// SYS_POSTNOTE sentinel for "send to my own Proc" (kernel maps pid == 0
// to the calling Proc's pid; matches POSIX kill(0, sig) "send to my
// process group" — Thylacine has no process groups at v1.0, so the
// nearest equivalent is "self").
pub const T_POSTNOTE_SELF_PID: i64   = 0;

// SYS_NOTED arg values — terminate a running note handler. Reserved for
// the async-handler path (libc-compat opt-in); libthyla-rs's t::notes
// wraps the fd-shaped path at v1.0 and does not surface NCONT / NDFLT
// directly. Constants are provided for callers that want to drive the
// handler path manually (kernel/syscall.c::sys_noted_handler).
pub const T_NOTED_NCONT: u64         = 0;
pub const T_NOTED_NDFLT: u64         = 1;

// TNoteRecord — userspace ABI of one entry read from the SYS_NOTE_OPEN
// fd. devnotes_read copies one of these per read() call. Mirrors
// kernel `struct note_record` (kernel/include/thylacine/notes.h)
// byte-for-byte; the kernel-side `_Static_assert(sizeof(...) == 32)`
// pins the layout on its end; the const-assert below pins ours.
#[repr(C)]
#[derive(Copy, Clone, Debug, Default)]
pub struct TNoteRecord {
    pub name:         [u8; T_NOTE_NAME_MAX],  //  0
    pub arg:          u32,                     // 16
    pub sender_pid:   u32,                     // 20
    pub timestamp_ns: u64,                     // 24
}
const _: () = assert!(core::mem::size_of::<TNoteRecord>() == 32);

// =============================================================================
// Caps — MUST mirror CAP_* bits in kernel/include/thylacine/caps.h.
// =============================================================================

pub const T_CAP_HW_CREATE: u64       = 1 << 0;
pub const T_CAP_LOCK_PAGES: u64      = 1 << 1;
pub const T_CAP_CSPRNG_READ: u64     = 1 << 2;
pub const T_CAP_HOSTOWNER: u64       = 1 << 3;   // elevation-only; not in CAP_ALL
pub const T_CAP_GRANT_HOSTOWNER: u64 = 1 << 4;   // fork-grantable; joey → corvus
pub const T_CAP_SET_IDENTITY: u64    = 1 << 5;   // A-1a; fork-grantable; gates SPAWN_IDENTITY_SET
// A-4a clearance/legate caps (mirror kernel/include/thylacine/caps.h).
pub const T_CAP_GRANT_CLEARANCE: u64 = 1 << 6;   // fork-grantable; corvus-only; register clearance grants
pub const T_CAP_DAC_OVERRIDE: u64    = 1 << 7;   // elevation-only; perm_check rwx bypass
pub const T_CAP_CHOWN: u64           = 1 << 8;   // elevation-only; chown/chgrp-to-any
pub const T_CAP_KILL: u64            = 1 << 9;   // elevation-only; cross-identity kill override

// =============================================================================
// Rights — MUST mirror RIGHT_* bits in kernel/include/thylacine/handle.h.
// =============================================================================

pub const T_RIGHT_READ: u32     = 1 << 0;
pub const T_RIGHT_WRITE: u32    = 1 << 1;
pub const T_RIGHT_MAP: u32      = 1 << 2;
pub const T_RIGHT_TRANSFER: u32 = 1 << 3;
pub const T_RIGHT_DMA: u32      = 1 << 4;
pub const T_RIGHT_SIGNAL: u32   = 1 << 5;
// MUST mirror the kernel-side RIGHT_ALL = 0x3f (six bits). A constant
// included here so consuming crates can assert their requested rights
// stay within the bound at compile time (Phase 5+ rights expansion).
pub const T_RIGHT_ALL: u32      = 0x3f;

// The right-bit subset that the kernel will accept on a fresh hw-handle
// (MMIO / IRQ / DMA). I-5 forbids transfer; the kernel rejects
// RIGHT_TRANSFER on hw handles at create time. Drivers should pass a
// subset of this when constructing hw handles.
pub const T_RIGHT_HW_ALLOWED: u32 =
    T_RIGHT_READ | T_RIGHT_WRITE | T_RIGHT_MAP | T_RIGHT_SIGNAL;

// =============================================================================
// Prot bits — MUST mirror VMA_PROT_* in kernel/include/thylacine/vma.h.
// =============================================================================

pub const T_PROT_READ: u32  = 1 << 0;
pub const T_PROT_WRITE: u32 = 1 << 1;
pub const T_PROT_EXEC: u32  = 1 << 2;

// =============================================================================
// SVC wrappers.
// =============================================================================

// t_exits — terminate the calling process with `status`. Never returns.
// status==0 ⇒ kernel exits("ok"); non-zero ⇒ exits("fail").
#[inline(always)]
pub unsafe fn t_exits(status: i64) -> ! {
    asm!(
        "svc #0",
        in("x0") status,
        in("x8") T_SYS_EXITS,
        options(noreturn, nostack)
    );
}

// t_exit_group — SYS_EXIT_GROUP (ARCH 7.9.1, I-24): terminate the WHOLE Proc
// (cascade peer-Thread termination), not just the calling Thread. Never
// returns. status==0 ⇒ "ok"; non-zero ⇒ "fail". POSIX exit_group(2): a
// single-thread Proc is equivalent to t_exits(status); a multi-thread Proc has
// every peer Thread self-exit at its EL0-return die-check. (No native v1.0
// consumer — the boundary is pouch's __NR_exit_group -> 60; exported for ABI
// completeness + a future native multi-thread program.)
#[inline(always)]
pub unsafe fn t_exit_group(status: i64) -> ! {
    asm!(
        "svc #0",
        in("x0") status,
        in("x8") T_SYS_EXIT_GROUP,
        options(noreturn, nostack)
    );
}

// t_puts — write `len` bytes from `buf` to the kernel diagnostic UART.
// Returns `len` on success, -1 on validation failure (NULL buf,
// oversized len, fault on user-VA copy).
//
// Safety: caller must ensure `buf` points to at least `len` readable
// bytes in valid user-VA memory.
#[inline(always)]
pub unsafe fn t_puts(buf: *const u8, len: usize) -> i64 {
    let mut x0: i64 = buf as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") len as u64,
        in("x8") T_SYS_PUTS,
        options(nostack)
    );
    x0
}

// t_putstr — convenience: write a `&str` via t_puts. Safe wrapper
// because `&str` carries length and points to valid memory.
#[inline]
pub fn t_putstr(s: &str) -> i64 {
    unsafe { t_puts(s.as_ptr(), s.len()) }
}

// t_mmio_create — create a KObj_MMIO handle for the PA range
// [pa, pa+size). Requires CAP_HW_CREATE in proc->caps. Returns a
// non-negative handle index on success, -1 on cap missing / overlap /
// alignment / IPS-bound / kernel-reserved-range rejection.
//
// Safety: caller must hold the capability + the PA range must be a
// real device range (not RAM owned by the kernel). The kernel-side
// kobj_mmio_create enforces every check; this wrapper just marshals
// args.
#[inline(always)]
pub unsafe fn t_mmio_create(pa: u64, size: u64, rights: u32) -> i64 {
    let mut x0: i64 = pa as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") size,
        in("x2") rights as u64,
        in("x8") T_SYS_MMIO_CREATE,
        options(nostack)
    );
    x0
}

// t_mmio_map — install user-VA mappings for a KObj_MMIO handle.
// `vaddr` must be page-aligned (4 KiB); `prot` must be non-zero, only
// R/W bits set, no EXEC (kernel rejects EXEC+device-memory per R10
// F157), and W-without-R is rejected (R10 F155 — AArch64 has no W-only
// AP encoding). Returns 0 on success, -1 on validation failure.
//
// Safety: handle must be valid + held by the caller.
#[inline(always)]
pub unsafe fn t_mmio_map(handle: i64, vaddr: u64, prot: u32) -> i64 {
    let mut x0: i64 = handle;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") vaddr,
        in("x2") prot as u64,
        in("x8") T_SYS_MMIO_MAP,
        options(nostack)
    );
    x0
}

// t_irq_create — create a KObj_IRQ handle for the given INTID.
// Requires CAP_HW_CREATE + the INTID must be SPI (32..1019; SGI/PPI
// are kernel-reserved per R9 F142 + F145). Returns a non-negative
// handle index on success, -1 on cap missing / out-of-range /
// already-claimed.
#[inline(always)]
pub unsafe fn t_irq_create(intid: u32, rights: u32) -> i64 {
    let mut x0: i64 = intid as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") rights as u64,
        in("x8") T_SYS_IRQ_CREATE,
        options(nostack)
    );
    x0
}

// t_irq_wait — block until the KObj_IRQ has fired one or more times.
// Returns the (collapsed) pending count consumed (always ≥ 1 on the
// success path — `kobj_irq_wait` blocks on a rendez until pending_count
// strictly exceeds zero, then atomically reads-and-clears under the
// rendez lock); -1 on validation failure (bad handle, missing
// RIGHT_SIGNAL).
//
// Edge-triggered: multiple fires while the waiter is blocked collapse
// to a single counter increment per actual GIC dispatch, but the
// returned value reflects the count seen at wake time.
#[inline(always)]
pub unsafe fn t_irq_wait(handle: i64) -> i64 {
    let mut x0: i64 = handle;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x8") T_SYS_IRQ_WAIT,
        options(nostack)
    );
    x0
}

// t_dma_create — create a KObj_DMA handle backed by `size` bytes of
// kernel-allocated contiguous pinned memory. Requires CAP_HW_CREATE in
// proc->caps. Size must be > 0 and ≤ 1 MiB at v1.0; kernel rounds up to
// the next page boundary. Returns a non-negative handle index on
// success, -1 on cap missing / size out of range / OOM.
//
// The DMA buffer's PA is chosen by the kernel and is stable for the
// handle's lifetime. Use t_dma_map to install it in your address space
// and obtain the PA for use in device-visible descriptors.
#[inline(always)]
pub unsafe fn t_dma_create(size: u64, rights: u32) -> i64 {
    let mut x0: i64 = size as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") rights as u64,
        in("x8") T_SYS_DMA_CREATE,
        options(nostack)
    );
    x0
}

// t_dma_map — install user-VA mappings for a KObj_DMA handle and return
// the underlying PA. `vaddr` must be page-aligned (4 KiB); `prot` must be
// non-zero, only R/W bits set (EXEC rejected per W^X), no W-without-R.
//
// Returns the buffer's PA on success (always non-negative since PA fits
// in 40 bits at v1.0), -1 on validation failure. Driver embeds the PA
// into device-visible descriptors (VirtIO virtqueue rings, etc.).
//
// Safety: handle must be valid + held by the caller.
#[inline(always)]
pub unsafe fn t_dma_map(handle: i64, vaddr: u64, prot: u32) -> i64 {
    let mut x0: i64 = handle;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") vaddr,
        in("x2") prot as u64,
        in("x8") T_SYS_DMA_MAP,
        options(nostack)
    );
    x0
}

// =============================================================================
// P5-fd-* family — byte I/O surface.
// =============================================================================
//
// Mirror of usr/lib/libt/include/thyla/syscall.h::t_pipe / t_read /
// t_write / t_close. The kernel-side handlers landed at P5-fd-pipe
// (0f66f5c/65293f5), P5-fd-rw (54d900b/a134782), P5-fd-syscalls
// (5fd72f6/3948bd4).

// t_pipe — create a connected Spoor pair. On success, returns the
// (rd_fd, wr_fd) tuple. On failure (table full / OOM), returns
// (-1, 0).
//
// Both fds are KOBJ_SPOOR handles installed in the caller's table.
// The 4 KiB ring buffer is shared between them; spoor_clunk on either
// side propagates EOF to the other.
#[inline(always)]
pub unsafe fn t_pipe() -> (i64, i64) {
    let mut x0: i64;
    let mut x1: i64;
    asm!(
        "svc #0",
        out("x0") x0,
        out("x1") x1,
        in("x8") T_SYS_PIPE,
        options(nostack)
    );
    (x0, x1)
}

// t_read — read up to `len` bytes from `fd` into `buf`. Returns:
//   > 0  : bytes actually read (may be < len; caller loops for full reads)
//   = 0  : EOF (peer closed write side)
//   < 0  : error (-1 on invalid fd / bad buf / fault)
//
// Per-call cap is 4 KiB (kernel-side SYS_RW_MAX); userspace loops for
// larger transfers.
//
// Safety: caller must ensure `buf` points to at least `len` writable
// bytes in valid user-VA memory.
#[inline(always)]
pub unsafe fn t_read(fd: i64, buf: *mut u8, len: usize) -> i64 {
    let mut x0: i64 = fd;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") buf as u64,
        in("x2") len as u64,
        in("x8") T_SYS_READ,
        options(nostack)
    );
    x0
}

// t_write — write up to `len` bytes from `buf` to `fd`. Returns:
//   > 0  : bytes actually written (may be < len)
//   = 0  : peer's read side closed (EOF on write)
//   < 0  : error (-1)
//
// Safety: caller must ensure `buf` points to at least `len` readable
// bytes in valid user-VA memory.
#[inline(always)]
pub unsafe fn t_write(fd: i64, buf: *const u8, len: usize) -> i64 {
    let mut x0: i64 = fd;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") buf as u64,
        in("x2") len as u64,
        in("x8") T_SYS_WRITE,
        options(nostack)
    );
    x0
}

// pollfd — userspace ABI of SYS_POLL. The kernel struct pollfd is
// `{ i32 fd; i16 events; i16 revents }` (8 bytes). `#[repr(C)]` pins
// the layout; the static asserts in <thylacine/poll.h> pin the kernel
// side. fd is an i32 handle index; events is a bitmask of T_POLL*;
// revents is kernel-filled.
#[repr(C)]
#[derive(Copy, Clone, Default, Debug)]
pub struct TPollFd {
    pub fd: i32,
    pub events: i16,
    pub revents: i16,
}

// t_poll — block until at least one of `fds` (a slice of `TPollFd`)
// becomes ready, or `timeout_ms` elapses. `timeout_ms`:
//   < 0 → block indefinitely (poll(-1));
//   = 0 → return immediately after the first scan (non-blocking probe);
//   > 0 → block for at most `timeout_ms` milliseconds.
//
// Returns the number of pollfds with `revents != 0` (≥ 0), or -1 on
// error (nfds == 0 or > 64, or fds points outside user-VA).
//
// `nfds` is bounded by the kernel at PROC_HANDLE_MAX (64). The kernel
// writes `revents` back to each pollfd in `fds` in place.
//
// Safety: `fds` must point to `nfds` writable `TPollFd` records in
// valid user-VA memory.
#[inline(always)]
pub unsafe fn t_poll(fds: *mut TPollFd, nfds: usize, timeout_ms: i32) -> i64 {
    let mut x0: i64 = fds as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") nfds as u64,
        in("x2") timeout_ms as i64 as u64,
        in("x8") T_SYS_POLL,
        options(nostack)
    );
    x0
}

// t_note_open — mint a fresh handle on the calling Proc's note Spoor.
// Idempotent: every call returns a new fd against the same per-Proc
// queue. Reads from the fd yield TNoteRecord (32 bytes each); poll
// integrates via POLLIN iff the queue holds at least one entry the
// calling Thread's mask permits.
//
// Returns the new fd (>= 0) on success, -1 on handle-table-full or
// devnotes_open failure.
#[inline(always)]
pub unsafe fn t_note_open() -> i64 {
    let mut x0: i64;
    asm!(
        "svc #0",
        lateout("x0") x0,
        in("x8") T_SYS_NOTE_OPEN,
        options(nostack)
    );
    x0
}

// t_postnote — post a note to a Proc. `pid` is the target's pid, OR
// `T_POSTNOTE_SELF_PID` (== 0) for self-post; the kernel rewrites
// pid == 0 to the caller's own pid. `name_va` is a user-VA pointer to
// `name_len` printable [0x20..0x7e] bytes; NUL terminator is NOT
// required (`name_len` is authoritative). Permission gate: caller
// must be self OR the target's parent.
//
// Returns 0 on success, -1 on bad name / bad target / queue full /
// permission denied / not-in-supported-set / `snare:`-prefixed name
// (reserved for kernel-synthetic posters).
//
// Safety: `name_va` must point to `name_len` readable bytes in user-
// VA memory.
#[inline(always)]
pub unsafe fn t_postnote(pid: i64, name_va: *const u8, name_len: usize) -> i64 {
    let mut x0: i64 = pid;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") name_va as u64,
        in("x2") name_len as u64,
        in("x8") T_SYS_POSTNOTE,
        options(nostack)
    );
    x0
}

// t_note_mask — swap the calling Thread's note_mask. `new_mask` is the
// new mask (bit set = defer that note); if `old_mask_out_va` is
// non-null, the previous mask is written there. Bits outside
// `T_NOTE_MASK_SUPPORTED` are tolerated (set but unused).
//
// Returns 0 on success, -1 on `old_mask_out_va` non-null but
// outside user-VA / unmapped at store time.
//
// Safety: `old_mask_out_va` is either null OR points to a writable u64
// in user-VA memory.
#[inline(always)]
pub unsafe fn t_note_mask(new_mask: u64, old_mask_out_va: *mut u64) -> i64 {
    let mut x0: i64 = new_mask as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") old_mask_out_va as u64,
        in("x8") T_SYS_NOTE_MASK,
        options(nostack)
    );
    x0
}

// t_mount — graft `source_spoor_fd`'s tree onto the mount-point directory named
// by the absolute `path` (`path_len` bytes) in the calling Proc's territory
// (stalk-2: path-keyed; was an abstract target_path_id). The kernel `stalk`s
// `path` to the mount point's (dc, devno, qid.path) identity. `flags` is a
// bitmask of T_MREPL / T_MBEFORE / T_MAFTER / T_MCREATE; bits outside that union
// are rejected. The mount point MUST EXIST as a walkable directory.
//
// Returns 0 on success, -1 on:
//   - path absent / empty / too long / not resolvable
//   - source_spoor_fd not a KOBJ_SPOOR or out-of-range
//   - missing RIGHT_READ on source
//   - flags has bits outside the supported set
//   - territory mount table full
#[inline(always)]
pub unsafe fn t_mount(path: *const u8, path_len: usize,
                      source_spoor_fd: i64, flags: u32) -> i64 {
    let mut x0: i64 = path as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") path_len as u64,
        in("x2") source_spoor_fd,
        in("x3") flags as u64,
        in("x8") T_SYS_MOUNT,
        options(nostack)
    );
    x0
}

// t_unmount — remove the mount entry whose mount-point identity matches the
// absolute `path` (`path_len` bytes) in the calling Proc's territory. Returns
// 0 on success, -1 if no entry matches (or the path does not resolve). Drops
// the per-entry Spoor refcount; the Spoor's Dev close runs if this was the
// last ref.
#[inline(always)]
pub unsafe fn t_unmount(path: *const u8, path_len: usize) -> i64 {
    let mut x0: i64 = path as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") path_len as u64,
        in("x8") T_SYS_UNMOUNT,
        options(nostack)
    );
    x0
}

// t_chroot — replace the calling Proc's Territory root_spoor with
// `spoor_fd`'s tree (atomic spoor_ref / spoor_clunk-displaced under
// the territory lock). Plan 9 chroot semantics; intended for the
// initial chroot. Idempotent on same-spoor.
//
// Returns 0 on success, -1 on:
//   - spoor_fd not a KOBJ_SPOOR or out-of-range
//   - missing RIGHT_READ on the source
#[inline(always)]
pub unsafe fn t_chroot(spoor_fd: i64) -> i64 {
    let mut x0: i64 = spoor_fd;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x8") T_SYS_CHROOT,
        options(nostack)
    );
    x0
}

// t_pivot_root — long-running-Proc root swap. Mirrors Linux
// pivot_root(2) minus the put_old argument: under the Proc's territory
// lock, swap root_spoor to `new_root_fd`'s tree and unref the
// displaced root. Distinct from t_chroot for audit-trackability and
// semantic clarity (joey uses this post-mount-bind to flip from
// devramfs to the disk-backed Stratum FS).
//
// Returns 0 on success, -1 on:
//   - new_root_fd not a KOBJ_SPOOR / out-of-range
//   - missing RIGHT_READ on the source
#[inline(always)]
pub unsafe fn t_pivot_root(new_root_fd: i64) -> i64 {
    let mut x0: i64 = new_root_fd;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x8") T_SYS_PIVOT_ROOT,
        options(nostack)
    );
    x0
}

/// t_attach_9p_srv -- drive a 9P attach over a byte-mode `/srv` connection
/// (16c; SYS_ATTACH_9P_SRV). `srv_fd` is a KOBJ_SPOOR CLIENT byte-conn from
/// open=connect on a byte-mode service (must carry R+W; the kernel 9P client
/// reads + writes the rings). The kernel runs Tversion + Tattach (substituting
/// the caller's principal_id for `n_uname`) and returns a KOBJ_SPOOR rooting
/// the attached tree (R|W|TRANSFER). `aname` is the server-side path /
/// capability string (<= SYS_ATTACH_ANAME_MAX; pass NULL+0 for the default
/// root). After a successful attach the `srv_fd` handle may be closed -- the
/// attach holds its own ref and the rings are kernel_attached. Returns the
/// new fd (>= 0) or -1.
#[inline(always)]
pub unsafe fn t_attach_9p_srv(srv_fd: i64, aname: *const u8, aname_len: usize,
                              n_uname: u64) -> i64 {
    let mut x0: i64 = srv_fd;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") aname as u64,
        in("x2") aname_len as u64,
        in("x3") n_uname,
        in("x8") T_SYS_ATTACH_9P_SRV,
        options(nostack)
    );
    x0
}

// t_torpor_wait — futex-style wait-on-address.
//   addr_va    user-VA pointer to a 4-byte-aligned u32 (the futex word)
//   expected   compared under the kernel `torpor_lock` -- mismatch =>
//              return 0 immediately (value already changed)
//   timeout_us see T_TORPOR_TIMEOUT_INDEFINITE / T_TORPOR_MAX_TIMEOUT_US
//
// Returns 0 on success (woken OR value mismatch), or a negative
// errno per the kernel documentation:
//   -22 EINVAL    bad alignment / outside user VA / null Proc /
//                 timeout > TORPOR_MAX_TIMEOUT_US
//   -14 EFAULT    addr_va unmapped / permission-denied at load
//   -110 ETIMEDOUT timeout lapsed with no wake
//
// Safety: addr_va must point to a 4-byte-aligned writable u32 in
// user-VA memory.
#[inline(always)]
pub unsafe fn t_torpor_wait(addr_va: *const u32, expected: u32, timeout_us: i64) -> i64 {
    let mut x0: i64 = addr_va as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") expected as u64,
        in("x2") timeout_us as u64,
        in("x8") T_SYS_TORPOR_WAIT,
        options(nostack)
    );
    x0
}

// t_torpor_wake — wake up to `count` waiters on `addr_va`. Does NOT
// load the word; only hashes the address and walks the queue. count =
// u32::MAX is the wake-all pattern.
//
// Returns the number of waiters actually woken (>= 0), or -22 EINVAL
// on bad alignment / outside user VA.
//
// Safety: addr_va must point to a 4-byte-aligned writable u32 in
// user-VA memory.
#[inline(always)]
pub unsafe fn t_torpor_wake(addr_va: *const u32, count: u32) -> i64 {
    let mut x0: i64 = addr_va as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") count as u64,
        in("x8") T_SYS_TORPOR_WAKE,
        options(nostack)
    );
    x0
}

// t_thread_spawn — kernel-level thread spawn. The new Thread shares
// pgtable_root + ASID + handle table + Territory with the calling
// Proc; only the entry / stack / x0 arg / TPIDR_EL0 are fresh.
//
//   entry_va   EL0 PC where the new thread starts
//   sp_va      stack TOP (must be 16-byte aligned per AAPCS64)
//   arg        the new thread's x0 on first entry
//   tls_va     loaded into TPIDR_EL0 before eret (0 OK; the entry
//              function then has to set up its own TLS)
//
// Returns the new tid (positive int) on success, or a negative
// errno:
//   -22 EINVAL  bad alignment / out-of-bound entry_va / sp_va / tls_va
//   -12 ENOMEM  kstack alloc fail / Thread cache alloc fail
//
// Safety: The new thread runs in the calling Proc's address space.
// `entry_va`, `sp_va`, `tls_va` must point to user-VA memory the
// caller controls (the caller owns the stack lifetime; libthyla-rs
// does not allocate or free the stack).
#[inline(always)]
pub unsafe fn t_thread_spawn(entry_va: u64, sp_va: u64, arg: u64, tls_va: u64) -> i64 {
    let mut x0: i64 = entry_va as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") sp_va,
        in("x2") arg,
        in("x3") tls_va,
        in("x8") T_SYS_THREAD_SPAWN,
        options(nostack)
    );
    x0
}

// t_set_tid_address — install `tidptr` as the calling thread's
// clear-child-tid address. SYS_THREAD_EXIT will uaccess_store_u32(0)
// to `tidptr` + torpor_wake(UINT32_MAX). Returns the calling thread's
// tid (positive); never fails for a userspace caller. A null tidptr
// clears the registration (no clear-on-exit).
//
// Safety: `tidptr` must be either null OR point to a 4-byte-aligned
// writable u32 in user-VA memory that remains valid until the
// calling thread exits.
#[inline(always)]
pub unsafe fn t_set_tid_address(tidptr: *mut u32) -> i64 {
    let mut x0: i64 = tidptr as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x8") T_SYS_SET_TID_ADDRESS,
        options(nostack)
    );
    x0
}

// t_thread_exit — terminate the calling Thread. NEVER returns.
//
// Atomically: clear_child_tid hand-off (best-effort), THREAD_EXITING
// transition, last-thread-also-exits-the-Proc collapse, and yield.
// Userspace must treat any apparent return as a kernel bug.
#[inline(always)]
pub unsafe fn t_thread_exit() -> ! {
    asm!(
        "svc #0",
        in("x8") T_SYS_THREAD_EXIT,
        options(nostack, noreturn)
    );
}

// t_close — release the handle at `fd`. For KOBJ_SPOOR handles the
// kernel's release path routes through spoor_clunk (atomic refcount
// drop; sets pipe EOF + wakes the other side per P5-pipe-blocking).
// Returns 0 on success, -1 on invalid fd.
#[inline(always)]
pub unsafe fn t_close(fd: i64) -> i64 {
    let mut x0: i64 = fd;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x8") T_SYS_CLOSE,
        options(nostack)
    );
    x0
}

// =============================================================================
// P5-corvus-syscalls — v1.0 hardening syscalls.
// =============================================================================
//
// Implemented at P5-corvus-syscalls (commit 0db0dcf/d10d4ee). These five
// wrappers are the Rust mirror of the C-side stubs in
// usr/lib/libt/include/thyla/syscall.h. Used by /sbin/corvus (and any
// future security-sensitive daemon) at startup.

// t_mlockall — pin all currently-mapped and future-mapped pages so they
// cannot be evicted to swap or any future paging tier. `flags` is
// reserved at v1.0 (must be 0). Sets PROC_FLAG_MLOCKED on the Proc.
//
// Requires CAP_LOCK_PAGES in proc->caps. Returns 0 on success, -1 on
// missing cap or non-zero flags. Once set, the flag is permanent for
// the Proc's lifetime — there's no t_munlockall at v1.0.
#[inline(always)]
pub unsafe fn t_mlockall(flags: u64) -> i64 {
    let mut x0: i64 = flags as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x8") T_SYS_MLOCKALL,
        options(nostack)
    );
    x0
}

// t_set_dumpable — control core-dump permission for the calling Proc.
// One-way to 0: t_set_dumpable(0) sets PROC_FLAG_NODUMP (permanent);
// t_set_dumpable(1) on a Proc that already has the flag is REFUSED
// (kernel returns -1). Returns 0 on first successful set-to-0; -1 on
// any other input or attempted re-enable.
//
// Core dumps don't exist at v1.0 — the flag is forward-compat
// scaffolding. When core dumps land, the kernel-side dump path must
// check this flag and refuse to dump a Proc with NODUMP set.
#[inline(always)]
pub unsafe fn t_set_dumpable(dumpable: u64) -> i64 {
    let mut x0: i64 = dumpable as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x8") T_SYS_SET_DUMPABLE,
        options(nostack)
    );
    x0
}

// t_set_traceable — control debug-Spoor attach permission. Same
// one-way-to-0 semantics as t_set_dumpable. Sets PROC_FLAG_NOTRACE.
//
// Debug Spoors don't exist at v1.0 — the flag is forward-compat
// scaffolding. When debug-Spoor attach lands, the kernel-side attach
// path must check this flag and refuse to attach to a Proc with
// NOTRACE set.
#[inline(always)]
pub unsafe fn t_set_traceable(traceable: u64) -> i64 {
    let mut x0: i64 = traceable as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x8") T_SYS_SET_TRACEABLE,
        options(nostack)
    );
    x0
}

// t_explicit_bzero — compiler-barrier'd memset to zero of `len` bytes at
// `buf`. The kernel performs a per-byte uaccess_store_u8 loop which the
// optimizer cannot elide. Returns 0 on success, -1 on validation
// failure (buf in kernel-VA, len > SYS_RW_MAX, mid-stream fault).
//
// Use this for in-RAM secrets immediately after they're consumed —
// passphrase buffers, derived KEKs, unwrapped DEKs. Without it, the
// compiler's dead-store elimination can remove a plain `*buf = 0; *buf
// = 0; ...` loop entirely.
//
// Safety: caller must ensure `buf` points to at least `len` writable
// bytes in valid user-VA memory.
#[inline(always)]
pub unsafe fn t_explicit_bzero(buf: *mut u8, len: usize) -> i64 {
    let mut x0: i64 = buf as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") len as u64,
        in("x8") T_SYS_EXPLICIT_BZERO,
        options(nostack)
    );
    x0
}

// t_getrandom — read `len` random bytes into `buf` from the kernel
// CSPRNG. `flags` is reserved at v1.0 (must be 0). Caller must hold
// CAP_CSPRNG_READ. Per-call cap is SYS_RW_MAX (4 KiB) at v1.0.
//
// Returns `len` on success, -1 on cap missing / non-zero flags /
// oversized len / mid-stream uaccess fault. The kernel CSPRNG is
// seeded from ARM RNDR at boot; if RNDR is unavailable or returns
// failure, getrandom returns -1 (caller must NOT proceed with
// guessable entropy).
//
// On mid-stream uaccess failure (kernel/syscall.c::sys_getrandom_handler
// per R15-d F237 close), the kernel best-effort zeros the partial range
// before returning -1, so a caller seeing -1 should still NOT trust the
// buffer's prior contents.
//
// Safety: caller must ensure `buf` points to at least `len` writable
// bytes in valid user-VA memory + hold CAP_CSPRNG_READ.
#[inline(always)]
pub unsafe fn t_getrandom(buf: *mut u8, len: usize, flags: u64) -> i64 {
    let mut x0: i64 = buf as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") len as u64,
        in("x2") flags,
        in("x8") T_SYS_GETRANDOM,
        options(nostack)
    );
    x0
}

// =============================================================================
// P5-corvus-srv-impl-b3b — /srv service syscall wrappers.
// =============================================================================
//
// Used by corvus (server side) and joey (client side) to consume the
// kernel-owned /srv transport landed at P5-corvus-srv-impl-a/-b2.

// TSrvPeerInfo — SYS_SRV_PEER's writeback buffer. Layout pinned by
// kernel-side struct srv_peer_info (kernel/include/thylacine/syscall.h)
// to a fixed 40-byte ABI record (A-1a appended the identity block after
// `alive`). Repr-C + the kernel's static asserts pin both sides.
//   principal_id : peer's durable identity; PRINCIPAL_NONE when alive == 0
//   primary_gid  : peer's primary group; GID_NONE when alive == 0
//   flags        : reserved, 0 at v1.0
#[repr(C)]
#[derive(Copy, Clone, Default, Debug)]
pub struct TSrvPeerInfo {
    pub stripes: u64,
    pub caps: u64,
    pub console: u32,
    pub alive: u32,
    pub principal_id: u32,
    pub primary_gid: u32,
    pub flags: u32,
    pub _reserved: u32,
}
const _: () = assert!(core::mem::size_of::<TSrvPeerInfo>() == 40);

// t_srv_accept — block until a client connects, return the server-side
// endpoint as a KObj_Spoor handle (byte I/O — plain t_read/t_write). The
// handshake (Tversion + Tattach + Twalk + Tlopen) was driven by the
// kernel before the client's SYS_SRV_CONNECT returned, so the very first
// t_read on the returned handle observes a fully-attached client.
//
// Each accepted connection has its own kernel-stamped peer identity;
// fetch it via t_srv_peer on the returned handle. The Spoor returned is
// transferable (KObj_Spoor) — corvus can hand it to a worker thread once
// concurrency lands at v1.x. At v1.0 corvus serves all conns single-
// threaded via t_poll.
//
// Returns the connection Spoor fd (>=0) on success, -1 on:
//   - service_handle is not a KObj_Srv listener
//   - listener torn down (poster exited / unposted)
//   - blocked accept woken by signal (v1.x — at v1.0 there are none)
#[inline(always)]
pub unsafe fn t_srv_accept(service_handle: i64) -> i64 {
    let mut x0: i64 = service_handle;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x8") T_SYS_SRV_ACCEPT,
        options(nostack)
    );
    x0
}

// t_srv_peer — read the kernel-stamped peer identity of a /srv
// connection into `out`. The fields are:
//   - stripes : the peer Proc's identity tag (immutable by-value capture
//               at mint; 0 means "the peer never had stripes assigned" —
//               unreachable at v1.0).
//   - caps    : the peer's LIVE capability set (looked up under the
//               proc table lock at call time). If the peer Proc has
//               exited/been reaped (`alive == 0`), caps is 0.
//   - console : 1 iff the peer was console-attached at mint (immutable).
//   - alive   : 1 iff an ALIVE Proc still carries this stripes value at
//               call time, else 0. Use this to decide whether to honor
//               caps; an alive==0 peer's caps is fail-closed.
//
// Refuses any caller whose stripes differ from the connection's poster
// (the gate prevents a non-server Proc from reading peer identity off a
// connection it just SYS_SRV_ACCEPT'd onto someone else's listener —
// which can't happen at v1.0 but is structurally pinned).
//
// Returns 0 on success, -1 on bad handle / wrong kind / SrvConn
// gate violation / bad user-VA.
//
// Safety: `out` must point to a writable TSrvPeerInfo (40 bytes) in
// valid user-VA memory.
#[inline(always)]
pub unsafe fn t_srv_peer(connection_handle: i64, out: *mut TSrvPeerInfo) -> i64 {
    let mut x0: i64 = connection_handle;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") out as u64,
        in("x8") T_SYS_SRV_PEER,
        options(nostack)
    );
    x0
}

// =============================================================================
// P5-corvus-srv-impl-b3a — t_spawn_with_perms.
// =============================================================================
//
// SYS_SPAWN_WITH_PERMS extends SYS_SPAWN_FULL with a sixth argument
// `perm_flags` carrying T_SPAWN_PERM_* bits. The kernel stamps each set
// bit as the corresponding PROC_FLAG_* on the spawned child atomically
// inside the spawn thunk (BEFORE the child's exec_setup), so the child's
// very first user-mode instruction observes the final proc_flags.
//
// Any nonzero perm_flags bit requires the calling Proc to be console-
// attached. Designed for joey (the v1.0 console anchor): joey grants
// /sbin/corvus T_SPAWN_PERM_MAY_POST_SERVICE so corvus may call
// SYS_POST_SERVICE("corvus"). Bits are NOT propagated by rfork (the
// design's "kernel-stamped" gate, not a cap).
//
// Returns the child PID (>0) on success, -1 on:
//   - perm_flags has unknown bits
//   - any perm_flags bit set AND caller is not console-attached
//   - any condition that t_spawn_full would return -1 on
//
// Safety: `name` must point to `name_len` readable bytes; `fds` (if
// fd_count > 0) must point to `fd_count` u32 entries; all in valid
// user-VA.
#[inline(always)]
pub unsafe fn t_spawn_with_perms(name: *const u8, name_len: usize,
                                 fds: *const u32, fd_count: usize,
                                 cap_mask: u64, perm_flags: u64) -> i64 {
    let mut x0: i64 = name as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") name_len as u64,
        in("x2") fds as u64,
        in("x3") fd_count as u64,
        in("x4") cap_mask,
        in("x5") perm_flags,
        in("x8") T_SYS_SPAWN_WITH_PERMS,
        options(nostack)
    );
    x0
}

// =============================================================================
// P5-hostowner-b-b — t_cap_grant / t_cap_use.
// =============================================================================
//
// Userspace bridges to the kernel `cap` elevation device
// (CORVUS-DESIGN.md §5.5.1). The Dev write op (devcap_write) is the
// eventual production path via a future namespace-aware open; at v1.0
// we lack t_open, so the two writers reach the cores directly.
//
// t_cap_grant — register a pending grant for `target_stripes`. Caller
// must hold CAP_GRANT_HOSTOWNER. Returns 0 on success, -1 on gate fail /
// bad args / table full. At v1.0 only CAP_HOSTOWNER is grantable.
#[inline(always)]
pub unsafe fn t_cap_grant(cap_mask: u64, target_stripes: u64) -> i64 {
    let mut x0: i64 = cap_mask as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") target_stripes,
        in("x8") T_SYS_CAP_GRANT,
        options(nostack)
    );
    x0
}

// t_cap_use — redeem a pending grant for the caller's own stripes.
// Caller must hold PROC_FLAG_CONSOLE_ATTACHED and have a non-expired
// pending grant with matching cap_mask. On success, caller's caps gains
// `cap_mask`; the grant is consumed (one-shot). Returns 0 on success,
// -1 on gate fail / no pending grant / mismatch.
#[inline(always)]
pub unsafe fn t_cap_use(cap_mask: u64) -> i64 {
    let mut x0: i64 = cap_mask as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x8") T_SYS_CAP_USE,
        options(nostack)
    );
    x0
}

// t_cap_grant_clearance — register a pending CLEARANCE grant for
// `target_stripes` (the A-4a legate grant-side bridge; the analog of
// t_cap_grant). Caller must hold CAP_GRANT_CLEARANCE (corvus). `cap_mask` is a
// non-empty subset of CAP_GRANTABLE_CLEARANCE; `valid_for_ns` is the legate
// lifetime duration (0 = no time bound); `session_id` is corvus's audit tag
// (nonzero, fits u32). The target redeems via t_cap_use (SYS_CAP_USE) -> it
// becomes a legate root. Returns 0 on success, -1 on gate fail / bad args /
// table full.
#[inline(always)]
pub unsafe fn t_cap_grant_clearance(
    cap_mask: u64,
    target_stripes: u64,
    valid_for_ns: u64,
    session_id: u64,
) -> i64 {
    let mut x0: i64 = cap_mask as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") target_stripes,
        in("x2") valid_for_ns,
        in("x3") session_id,
        in("x8") T_SYS_CAP_GRANT_CLEARANCE,
        options(nostack)
    );
    x0
}

// t_burrow_attach — request `length` bytes of anonymous, demand-zero,
// read-write memory and have the kernel install it in the calling
// Proc's address space. Returns the page-aligned base user-VA on
// success (always >= EXEC_USER_BURROW_BASE = 0x0000_0001_0000_0000),
// -1 on:
//   - length == 0 or length > BURROW_ATTACH_MAX (= 256 MiB)
//   - no free gap of round_up(length) in the burrow window
//   - burrow_create_anon / burrow_map OOM
//
// The kernel rounds `length` up to a page (4 KiB) and chooses the
// virtual address (first-fit in EXEC_USER_BURROW_BASE..TOP). Pages
// install on demand via the user-fault path.
//
// Tier-1 native memory primitive (ARCHITECTURE.md §6.5). Backs
// libthyla-rs's t::alloc global heap (U-2b).
#[inline(always)]
pub unsafe fn t_burrow_attach(length: u64) -> i64 {
    let mut x0: i64 = length as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x8") T_SYS_BURROW_ATTACH,
        options(nostack)
    );
    x0
}

// t_burrow_detach — release a region previously attached by
// t_burrow_attach. The (vaddr, page-rounded length) must match an
// installed VMA exactly — no partial detach at v1.0 (mirrors the
// kernel-side burrow_unmap constraint). Returns 0 on success, -1 on:
//   - length == 0 or length > BURROW_ATTACH_MAX
//   - vaddr not page-aligned
//   - no VMA matches [vaddr, vaddr + round_up(length)) exactly
//
// `length` may be the original request OR any value that page-rounds
// to the same span; the kernel matches on the rounded range.
#[inline(always)]
pub unsafe fn t_burrow_detach(vaddr: u64, length: u64) -> i64 {
    let mut x0: i64 = vaddr as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") length,
        in("x8") T_SYS_BURROW_DETACH,
        options(nostack)
    );
    x0
}

// t_loom_setup — create a Loom ring with `entries` SQ slots; the kernel maps the
// SQ/CQ Burrow into this Proc and fills the `struct loom_params` at `params_va`
// with the ring geometry (ring_va + region offsets/sizes). Returns the loom_fd
// (>= 0) or -1. Backs t::loom::Ring::setup (Loom-6d).
#[inline(always)]
pub unsafe fn t_loom_setup(entries: u64, params_va: u64) -> i64 {
    let mut x0: i64 = entries as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") params_va,
        in("x8") T_SYS_LOOM_SETUP,
        options(nostack)
    );
    x0
}

// t_loom_register — install fixed handles (op = 0) or pin buffers (op = 1) for a
// ring. `arg_va` points at `nargs` elements (u32 fd indices, or struct
// loom_buf_reg). Returns 0 / -1. Backs t::loom::Ring::register_*.
#[inline(always)]
pub unsafe fn t_loom_register(loom_fd: u64, op: u64, arg_va: u64, nargs: u64) -> i64 {
    let mut x0: i64 = loom_fd as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") op,
        in("x2") arg_va,
        in("x3") nargs,
        in("x8") T_SYS_LOOM_REGISTER,
        options(nostack)
    );
    x0
}

// t_loom_enter — consume up to `to_submit` queued SQEs, then block until at least
// `min_complete` CQEs are available (unless LOOM_ENTER_NONBLOCK in `flags`).
// Returns the SQEs consumed (>= 0) or -1. Backs t::loom::Ring::enter.
#[inline(always)]
pub unsafe fn t_loom_enter(loom_fd: u64, to_submit: u64, min_complete: u64, flags: u64) -> i64 {
    let mut x0: i64 = loom_fd as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") to_submit,
        in("x2") min_complete,
        in("x3") flags,
        in("x8") T_SYS_LOOM_ENTER,
        options(nostack)
    );
    x0
}

// t_walk_open — walk one path component from `spoor_fd` (or from the
// caller's Territory root if `spoor_fd == T_WALK_OPEN_FROM_ROOT`) and
// open the resulting Spoor with `omode`. The single-step walk reflects
// the kernel surface: multi-component paths are walked per-component
// in userspace (t::fs::File does this internally).
//
// Returns:
//   >= 0  — opened KOBJ_SPOOR fd, rights = READ | WRITE | TRANSFER.
//           The underlying fid's omode is what the server actually
//           enforces; a write to an OREAD-opened fid gets the server's
//           Rlerror via SYS_WRITE.
//   -1    — bad spoor_fd / missing RIGHT_READ / name out of bounds /
//           name contains '/' or '\0' / omode invalid / walk-or-open
//           failure / handle table full.
//
// Backs t::fs::File::open (U-2c-io).
#[inline(always)]
pub unsafe fn t_walk_open(spoor_fd: i64, name: *const u8, name_len: usize, omode: u32) -> i64 {
    let mut x0: i64 = spoor_fd;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") name as u64,
        in("x2") name_len as u64,
        in("x3") omode as u64,
        in("x8") T_SYS_WALK_OPEN,
        options(nostack)
    );
    x0
}

/// t_open -- open a (possibly multi-component) path via the kernel `stalk`
/// resolver (A-5b-0; SYS_OPEN; docs/STALK-DESIGN.md). `start_fd` is a
/// KOBJ_SPOOR handle or T_WALK_OPEN_FROM_ROOT; `path` is '/'-separated;
/// `omode` is as for t_walk_open (+ T_OPATH for a walk-only handle).
/// Returns the opened (or O_PATH) fd (>= 0) or -1. Supersedes t_walk_open
/// for paths of more than one component.
#[inline(always)]
pub unsafe fn t_open(start_fd: i64, path: *const u8, path_len: usize, omode: u32) -> i64 {
    let mut x0: i64 = start_fd;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") path as u64,
        in("x2") path_len as u64,
        in("x3") omode as u64,
        in("x8") T_SYS_OPEN,
        options(nostack)
    );
    x0
}

// t_walk_create — create-then-open the single component `name` inside the
// directory `parent_fd` (KOBJ_SPOOR with RIGHT_WRITE, or T_WALK_OPEN_FROM_ROOT
// for the Territory root) and return a new opened KOBJ_SPOOR fd. `perm`'s low 9
// bits are the POSIX mode; T_WALK_CREATE_DMDIR selects a directory. The created
// object's group is the caller's primary_gid. `omode` is the open mode for the
// returned fd (a directory is opened OREAD). FS-mutation foundation
// (IDENTITY-DESIGN.md section 9.2). Returns the new fd (>= 0) or -1.
#[inline(always)]
pub unsafe fn t_walk_create(parent_fd: i64, name: *const u8, name_len: usize,
                            omode: u32, perm: u32) -> i64 {
    let mut x0: i64 = parent_fd;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") name as u64,
        in("x2") name_len as u64,
        in("x3") omode as u64,
        in("x4") perm as u64,
        in("x8") T_SYS_WALK_CREATE,
        options(nostack)
    );
    x0
}

// t_fsync — durability barrier on `fd` (KOBJ_SPOOR, RIGHT_WRITE). datasync 0 =
// full, non-zero = data only. Returns 0 / -1. FS-mutation foundation (§9.2).
#[inline(always)]
pub unsafe fn t_fsync(fd: i64, datasync: u32) -> i64 {
    let mut x0: i64 = fd;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") datasync as u64,
        in("x8") T_SYS_FSYNC,
        options(nostack)
    );
    x0
}

// t_readdir — read the next run of 9P2000.L dirents from directory `fd`
// (KOBJ_SPOOR, RIGHT_READ) into `buf` (<= SYS_RW_MAX), advancing the Spoor's
// offset. Returns bytes (>= 0; 0 = end-of-directory), -1 on error. Each entry:
// qid(13) + offset(8 LE) + type(1) + name_len(2 LE) + name. FS-mutation
// foundation (§9.2).
#[inline(always)]
pub unsafe fn t_readdir(fd: i64, buf: *mut u8, buf_len: usize) -> i64 {
    let mut x0: i64 = fd;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") buf as u64,
        in("x2") buf_len as u64,
        in("x8") T_SYS_READDIR,
        options(nostack)
    );
    x0
}

// t_rename — atomically rename/move the single component `oldname` in directory
// `olddir_fd` to `newname` in `newdir_fd` (both KOBJ_SPOOR dirs, RIGHT_WRITE, or
// T_WALK_OPEN_FROM_ROOT). POSIX rename / 9P Trenameat: an existing destination is
// atomically replaced. olddir_fd + newdir_fd must be on the same Dev/session.
// Returns 0 / -1. FS-gamma (IDENTITY-DESIGN.md section 9.3).
#[inline(always)]
pub unsafe fn t_rename(olddir_fd: i64, oldname: *const u8, oldname_len: usize,
                       newdir_fd: i64, newname: *const u8, newname_len: usize) -> i64 {
    let mut x0: i64 = olddir_fd;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") oldname as u64,
        in("x2") oldname_len as u64,
        in("x3") newdir_fd,
        in("x4") newname as u64,
        in("x5") newname_len as u64,
        in("x8") T_SYS_RENAME,
        options(nostack)
    );
    x0
}

// t_unlink — remove the single component `name` from directory `parent_fd`
// (KOBJ_SPOOR, RIGHT_WRITE, or T_WALK_OPEN_FROM_ROOT). flags 0 = unlink a
// non-directory; T_UNLINK_REMOVEDIR = rmdir an empty directory. Returns 0 / -1.
// FS-gamma (IDENTITY-DESIGN.md section 9.3).
#[inline(always)]
pub unsafe fn t_unlink(parent_fd: i64, name: *const u8, name_len: usize, flags: u32) -> i64 {
    let mut x0: i64 = parent_fd;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") name as u64,
        in("x2") name_len as u64,
        in("x3") flags as u64,
        in("x8") T_SYS_UNLINK,
        options(nostack)
    );
    x0
}

// t_lseek — reposition the byte offset of an open Spoor. Mirrors
// POSIX lseek(2): whence is T_SEEK_SET (absolute), T_SEEK_CUR (relative),
// or T_SEEK_END (relative to file size). Returns the new offset on
// success, -1 on bad fd / invalid whence / new_offset < 0 / SEEK_END on
// a Dev without stat_native. Backs t::io::Seek for t::fs::File (U-2c-io).
#[inline(always)]
pub unsafe fn t_lseek(spoor_fd: i64, offset: i64, whence: u32) -> i64 {
    let mut x0: i64 = spoor_fd;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") offset,
        in("x2") whence as u64,
        in("x8") T_SYS_LSEEK,
        options(nostack)
    );
    x0
}

// t_fstat — fill a user-VA 80-byte `struct t_stat` (laid out per
// kernel/include/thylacine/syscall.h's ABI pins) with the open Spoor's
// metadata. Returns 0 on success, -1 on bad fd / unaligned stat_va /
// stat_va outside user VA / Dev without stat_native vtable op.
//
// `stat_va` must point to at least 80 bytes of writable, 8-byte-aligned
// user memory. Backs t::fs::Metadata (U-2c-fs).
#[inline(always)]
pub unsafe fn t_fstat(spoor_fd: i64, stat_va: *mut u8) -> i64 {
    let mut x0: i64 = spoor_fd;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") stat_va as u64,
        in("x8") T_SYS_FSTAT,
        options(nostack)
    );
    x0
}

// t_wstat — chmod/chown an open Spoor `fd` (KOBJ_SPOOR, RIGHT_WRITE) via
// Tsetattr. `valid` selects which of (mode, uid, gid) to apply (T_WSTAT_*
// bits; at least one). `mode` is the 9 rwx bits only (setuid/setgid/sticky
// rejected). Returns 0 on success, -1 on bad fd / bad mask / out-of-range
// value / Dev without wstat_native / server error. A-2a (the mechanism;
// the per-file permission policy is A-2d). Backs t::fs metadata mutation.
#[inline(always)]
pub unsafe fn t_wstat(fd: i64, valid: u32, mode: u32, uid: u32, gid: u32) -> i64 {
    let mut x0: i64 = fd;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") valid as u64,
        in("x2") mode as u64,
        in("x3") uid as u64,
        in("x4") gid as u64,
        in("x8") T_SYS_WSTAT,
        options(nostack)
    );
    x0
}

// t_spawn_full_argv — combined-spawn primitive with argv pass-through.
// Subsumes every earlier SYS_SPAWN_* surface. Reads the calling Proc's
// 80-byte `TSpawnArgs` struct at `req_va` and rfork-execs a fresh child.
// Returns the child's pid (positive) on success, -1 on any rejection
// (see SYS_SPAWN_FULL_ARGV kernel header for the full list).
//
// Backs t::process::Command::spawn (U-2d).
#[inline(always)]
pub unsafe fn t_spawn_full_argv(req_va: *const TSpawnArgs) -> i64 {
    let mut x0: i64 = req_va as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x8") T_SYS_SPAWN_FULL_ARGV,
        options(nostack)
    );
    x0
}

// t_wait_pid — block until one of the caller's children enters ZOMBIE,
// then reap it. On success returns the child's pid (positive) and
// populates `*status_out` with the child's exit status. On failure
// (no children / unmapped status pointer) returns -1.
//
// Backs t::process::Child::wait (U-2d).
#[inline(always)]
pub unsafe fn t_wait_pid(status_out: *mut i32) -> i64 {
    let mut x0: i64 = status_out as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x8") T_SYS_WAIT_PID,
        options(nostack)
    );
    x0
}

// =============================================================================
// VIRTIO memory-ordering helpers.
// =============================================================================
//
// virtio_rmb — read-side barrier for the driver's view of the used ring.
// VIRTIO 1.2 §2.7.13.2 mandates the driver execute a barrier of this
// shape between observing `used.idx` advance and reading `used.ring[k]`
// or the data buffer the descriptor pointed at. Without it, an
// out-of-order ARM core may speculatively issue the data-buffer reads
// before the used.idx load, returning stale (pre-advance) bytes.
//
// `dmb ishld` is the LoadLoad barrier scoped to the Inner Shareable
// domain — the minimum sufficient for guest-CPU-vs-guest-CPU ordering of
// Normal-WB memory backing the virtqueue. Matches what Linux's
// `virtio_rmb()` compiles to on AArch64.
//
// Today on QEMU TCG (in-order execution) this is a no-op in practice,
// but emitting it preserves correctness on real ARM cores (the v1.0
// deployment target). The cost is one barrier instruction per used.idx
// read.
#[inline(always)]
pub fn virtio_rmb() {
    unsafe { asm!("dmb ishld", options(nostack, preserves_flags)) }
}

// =============================================================================
// _start — entry point.
// =============================================================================
//
// The kernel's userland_enter delivers control here with sp pointing at the
// System V startup frame's argc word (EXEC_USER_STACK_TOP - frame_size;
// kernel/include/thylacine/exec.h "Shape A / Shape B"). _start captures the
// argv frame for env::args() (DOC-GAP G03) BEFORE any prologue runs — a
// function prologue moves sp, so the capture has to be the very first thing
// _start does — then calls the app's rs_main() through a tiny Rust shim that
// stashes (argc, argv) into process-global statics. Defined in asm for full
// control over the BTI marker + the pre-prologue sp read; mirrors
// usr/lib/libt/src/start.S (the C side's _start).
//
// Both startup shapes are handled by the SAME capture: argc lives at [sp]
// and argv[] begins at sp + 8 in both Shape A (argc = 0; env::args() then
// reports no operands) and Shape B (real argc + argv).
//
// Flow:
//   bti c                       — BTI landing pad for indirect entry.
//   ldr x0, [sp]                — x0 := argc       (startup frame[0])
//   add x1, sp, #8              — x1 := &argv[0]   (startup frame + 8)
//   bl __libthyla_rt_start      — stash (argc, argv); x0 := rs_main()
//   mov x8, T_SYS_EXITS         — syscall number
//   svc #0                      — never returns
//   wfe + b 1b                  — defensive: SYS_EXITS doesn't return, but
//                                 if the kernel were to deliver us back
//                                 (impossible at v1.0), park forever.
//
// ENTRY(_start) in usr/scripts/aarch64-userspace.ld keeps _start as a
// liveness root, so the linker pulls this symbol from libthyla-rs.rlib
// even though no Rust code references it directly.
global_asm!(
    ".section .text._start, \"ax\"",
    ".globl _start",
    ".type _start, %function",
    "_start:",
    "    bti     c",                       // BTI landing pad for indirect entry
    "    ldr     x0, [sp]",                // x0 := argc        (startup frame[0])
    "    add     x1, sp, #8",              // x1 := &argv[0]    (startup frame + 8)
    "    bl      __libthyla_rt_start",     // stash (argc, argv); x0 := rs_main()
    "    mov     x8, #0",                  // T_SYS_EXITS
    "    svc     #0",                      // never returns
    "1:  wfe",                             // defensive; SYS_EXITS doesn't return
    "    b       1b",
    ".size _start, .-_start",
);

// =============================================================================
// Native runtime argv capture (DOC-GAP G03).
// =============================================================================
//
// _start hands (argc, &argv[0]) to this shim, which records them for
// env::args() and then calls the app's rs_main(). A non-naked fn is correct
// here because _start already captured argc/argv into x0/x1 before this
// shim's prologue could move sp; the values arrive as ordinary C arguments.
//
// The stores are Release and rt_raw_args' loads are Acquire so a peer thread
// (which gets its own stack but shares this address space) observes the
// values; in practice they are written exactly once at startup before any
// thread is spawned, but the ordering makes the cross-thread read
// unimpeachable rather than relying on the spawn syscall's barrier.

use core::sync::atomic::{AtomicPtr, AtomicUsize, Ordering};

static RT_ARGC: AtomicUsize = AtomicUsize::new(0);
static RT_ARGV: AtomicPtr<*const u8> = AtomicPtr::new(core::ptr::null_mut());

extern "C" {
    fn rs_main() -> i64;
}

#[no_mangle]
unsafe extern "C" fn __libthyla_rt_start(argc: usize, argv: *const *const u8) -> i64 {
    RT_ARGC.store(argc, Ordering::Release);
    RT_ARGV.store(argv as *mut *const u8, Ordering::Release);
    rs_main()
}

/// The captured (argc, &argv[0]) from process startup. Returns `(0, null)`
/// shaped values only before `_start` runs (never observable from app code).
/// Backs `env::args()`.
#[inline]
pub(crate) fn rt_raw_args() -> (usize, *const *const u8) {
    let argc = RT_ARGC.load(Ordering::Acquire);
    let argv = RT_ARGV.load(Ordering::Acquire) as *const *const u8;
    (argc, argv)
}

// =============================================================================
// Panic handler.
// =============================================================================
//
// Required by no_std. v1.0 P4-Ic4 response: SYS_EXITS(1). Phase 5+ can
// add a richer panic path (panic message → t_puts) via a Cargo feature
// flag once the runtime crate matures.
#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    unsafe { t_exits(1) }
}

// =============================================================================
// Best-effort stdout/stderr formatting macros (DOC-GAP G05).
// =============================================================================
//
// `print!`/`println!`/`eprint!`/`eprintln!` format to fd 1 / fd 2 via the
// io::{Stdout, Stderr} handles. They are BEST-EFFORT: a write error (e.g. an
// unwired fd 1 on a standalone run -- DOC-GAP G06) is swallowed, so a program
// whose output is optional does not spuriously fail. A program that must know
// whether output reached its sink uses `io::stdout().write_all(...)?` directly.
//
// Exported at the crate root, so callers write `libthyla_rs::println!(...)`.
// `$crate` keeps the expansion hygienic without the caller importing the io
// traits.

/// Format to standard output (fd 1), best-effort. See the module note.
#[macro_export]
macro_rules! print {
    ($($arg:tt)*) => {{
        let _ = <$crate::io::Stdout as $crate::io::Write>::write_fmt(
            &mut $crate::io::stdout(), core::format_args!($($arg)*));
    }};
}

/// Format to standard output (fd 1) followed by a newline, best-effort.
#[macro_export]
macro_rules! println {
    () => {{
        let _ = <$crate::io::Stdout as $crate::io::Write>::write_all(
            &mut $crate::io::stdout(), b"\n");
    }};
    ($($arg:tt)*) => {{
        $crate::print!($($arg)*);
        let _ = <$crate::io::Stdout as $crate::io::Write>::write_all(
            &mut $crate::io::stdout(), b"\n");
    }};
}

/// Format to standard error (fd 2), best-effort.
#[macro_export]
macro_rules! eprint {
    ($($arg:tt)*) => {{
        let _ = <$crate::io::Stderr as $crate::io::Write>::write_fmt(
            &mut $crate::io::stderr(), core::format_args!($($arg)*));
    }};
}

/// Format to standard error (fd 2) followed by a newline, best-effort.
#[macro_export]
macro_rules! eprintln {
    () => {{
        let _ = <$crate::io::Stderr as $crate::io::Write>::write_all(
            &mut $crate::io::stderr(), b"\n");
    }};
    ($($arg:tt)*) => {{
        $crate::eprint!($($arg)*);
        let _ = <$crate::io::Stderr as $crate::io::Write>::write_all(
            &mut $crate::io::stderr(), b"\n");
    }};
}
