//! The ring ABI mirror of kernel/include/thylacine/loom.h. It depends on no crate,
//! so tools/check-loom-service-abi.py compiles it alone and checks its constants and
//! records against the layout table the kernel and libt headers answer to.

/// Opcodes -- the `p9_client_*` surface (`LOOM_OP_*`).
pub mod op {
    pub const NOP: u8 = 0;
    pub const WALK: u8 = 1;
    pub const LOPEN: u8 = 2;
    pub const LCREATE: u8 = 3;
    pub const READ: u8 = 4;
    pub const WRITE: u8 = 5;
    pub const GETATTR: u8 = 6;
    pub const SETATTR: u8 = 7;
    pub const READDIR: u8 = 8;
    pub const FSYNC: u8 = 9;
    pub const CLUNK: u8 = 10;
    pub const RENAMEAT: u8 = 11;
    pub const UNLINKAT: u8 = 12;
    pub const MKDIR: u8 = 13;
    pub const SYMLINK: u8 = 14;
    pub const LINK: u8 = 15;
    pub const MKNOD: u8 = 16;
    pub const READLINK: u8 = 17;
    pub const STATFS: u8 = 18;
}

/// Per-SQE flags (`LOOM_SQE_*`).
pub mod sqe_flag {
    pub const LINK: u8 = 1 << 0;
    pub const DRAIN: u8 = 1 << 1;
    pub const CQE_SKIP: u8 = 1 << 2;
    pub const MULTISHOT: u8 = 1 << 3;
}

/// `LOOM_CQE_MORE` -- set on every non-terminal shot of a multishot stream.
pub const CQE_MORE: u32 = 1 << 0;

/// `LOOM_SETUP_SQPOLL` -- start a kernel poll-thread (zero-syscall submission).
pub const SETUP_SQPOLL: u32 = 1 << 0;

/// `LOOM_SETUP_CQSIZE` -- a caller-chosen CQ size. `Ring::setup` does not offer it.
#[allow(dead_code)]
pub(crate) const SETUP_CQSIZE: u32 = 1 << 1;

/// `LOOM_ENTER_*` flags.
pub const ENTER_GETEVENTS: u32 = 1 << 0;
pub const ENTER_NONBLOCK: u32 = 1 << 1;

/// `LOOM_RING_SQ_NEED_WAKEUP` -- set by an idled SQPOLL kthread in the ring flags.
pub const RING_SQ_NEED_WAKEUP: u32 = 1 << 0;

/// `LOOM_HANDLE_RAW` -- a path-resolved-at-submit sentinel (reserved, not v1.0).
pub const HANDLE_RAW: u32 = 0xFFFF_FFFF;

/// Register sub-ops (`LOOM_REGISTER_*`).
pub(crate) const REGISTER_HANDLES: u32 = 0;
pub(crate) const REGISTER_BUFFERS: u32 = 1;

/// Ring-size + table bounds (`LOOM_MAX_*`).
pub const MAX_ENTRIES: u32 = 4096;
pub const MAX_REG_HANDLES: u32 = 64;
pub const MAX_REG_BUFFERS: u32 = 64;

/// One submission entry (`struct loom_sqe`, 64 bytes). Construct via the
/// per-opcode helpers ([`Sqe::nop`], [`Sqe::read`], ...) so the reserved fields
/// stay zeroed; the kernel reads `_resv1` for some ops.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct Sqe {
    pub opcode: u8,
    pub flags: u8,
    pub(crate) _resv0: u16,
    pub handle_idx: u32,
    pub offset: u64,
    pub len: u32,
    pub buf_idx_or_off: u32,
    pub user_data: u64,
    pub(crate) _resv1: [u64; 4],
}

const _: () = assert!(core::mem::size_of::<Sqe>() == 64);
// Field offsets pinned to kernel loom.h. The size assert alone would miss a
// same-size field reorder (e.g. swapping two u32s) that silently shifts the
// byte-pinned Loom ABI -- the C side carries the matching `__builtin_offsetof`
// asserts (6c audit F4); this mirror must too (6d audit F1 / SA-2).
const _: () = assert!(core::mem::offset_of!(Sqe, opcode) == 0);
const _: () = assert!(core::mem::offset_of!(Sqe, flags) == 1);
const _: () = assert!(core::mem::offset_of!(Sqe, handle_idx) == 4);
const _: () = assert!(core::mem::offset_of!(Sqe, offset) == 8);
const _: () = assert!(core::mem::offset_of!(Sqe, len) == 16);
const _: () = assert!(core::mem::offset_of!(Sqe, buf_idx_or_off) == 20);
const _: () = assert!(core::mem::offset_of!(Sqe, user_data) == 24);
const _: () = assert!(core::mem::offset_of!(Sqe, _resv1) == 32);

/// One completion entry (`struct loom_cqe`, 16 bytes).
#[repr(C)]
#[derive(Copy, Clone, Debug)]
pub struct Cqe {
    /// Echoed verbatim from the submitting SQE's `user_data`.
    pub user_data: u64,
    /// `>= 0` = byte count / 0 / packed qid; `< 0` = `-errno` (Rlerror).
    pub result: i32,
    /// `LOOM_CQE_*` (e.g. [`CQE_MORE`]).
    pub flags: u32,
}

const _: () = assert!(core::mem::size_of::<Cqe>() == 16);
const _: () = assert!(core::mem::offset_of!(Cqe, user_data) == 0);
const _: () = assert!(core::mem::offset_of!(Cqe, result) == 8);
const _: () = assert!(core::mem::offset_of!(Cqe, flags) == 12);

/// One registered-buffer descriptor (`struct loom_buf_reg`, 16 bytes): a VA range
/// within one anonymous RW VMA the kernel pins for zero-copy payload.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct BufReg {
    pub va: u64,
    pub len: u64,
}

const _: () = assert!(core::mem::size_of::<BufReg>() == 16);
const _: () = assert!(core::mem::offset_of!(BufReg, va) == 0);
const _: () = assert!(core::mem::offset_of!(BufReg, len) == 8);

/// `SYS_LOOM_SETUP` in/out struct (`struct loom_params`, 88 bytes). `flags` is the
/// only IN field; the kernel fills the rest with the mapped ring geometry.
#[repr(C)]
#[derive(Copy, Clone)]
pub(crate) struct Params {
    pub(crate) flags: u32,
    pub(crate) sq_entries: u32,
    pub(crate) cq_entries: u32,
    pub(crate) ring_size: u32,
    pub(crate) ring_va: u64,
    pub(crate) hdr_off: u32,
    pub(crate) sq_array_off: u32,
    pub(crate) sqe_off: u32,
    pub(crate) cqe_off: u32,
    pub(crate) sq_array_size: u32,
    pub(crate) sqe_size: u32,
    pub(crate) cqe_size: u32,
    pub(crate) _resv0: u32,
    pub(crate) _resv1: [u64; 4],
}

const _: () = assert!(core::mem::size_of::<Params>() == 88);
const _: () = assert!(core::mem::offset_of!(Params, flags) == 0);
const _: () = assert!(core::mem::offset_of!(Params, ring_va) == 16);
const _: () = assert!(core::mem::offset_of!(Params, sqe_off) == 32);
const _: () = assert!(core::mem::offset_of!(Params, cqe_off) == 36);

/// `struct loom_ring_hdr` (64 bytes). `Ring` reads its words as atomics at these
/// fields' offsets and never as a value.
#[repr(C)]
#[allow(dead_code)]
pub(crate) struct RingHdr {
    pub(crate) sq_head: u32,
    pub(crate) sq_tail: u32,
    pub(crate) sq_mask: u32,
    pub(crate) sq_entries: u32,
    pub(crate) cq_head: u32,
    pub(crate) cq_tail: u32,
    pub(crate) cq_mask: u32,
    pub(crate) cq_entries: u32,
    pub(crate) flags: u32,
    pub(crate) dropped: u32,
    pub(crate) overflow: u32,
    pub(crate) _pad: [u32; 5],
}

const _: () = assert!(core::mem::size_of::<RingHdr>() == 64);
