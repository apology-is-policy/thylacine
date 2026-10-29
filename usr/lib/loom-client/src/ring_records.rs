//! The adapter's copies of the ring records it writes or reads by offset: the SQE,
//! the ring header and the buffer registration. They depend on no other module,
//! so tools/check-loom-service-abi.py checks each against the kernel's layout table.
use core::mem::{offset_of, size_of};

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct Submission {
    pub opcode: u8,
    pub flags: u8,
    pub reserved: u16,
    pub handle_idx: u32,
    pub offset: u64,
    pub len: u32,
    pub buf_idx_or_off: u32,
    pub user_data: u64,
    /// Buffer offset, source incarnation, destination incarnation, deadline.
    pub args: [u64; 4],
}
const _: () = assert!(size_of::<Submission>() == 64);
const _: () = assert!(offset_of!(Submission, flags) == 1);
const _: () = assert!(offset_of!(Submission, reserved) == 2);
const _: () = assert!(offset_of!(Submission, handle_idx) == 4);
const _: () = assert!(offset_of!(Submission, offset) == 8);
const _: () = assert!(offset_of!(Submission, len) == 16);
const _: () = assert!(offset_of!(Submission, buf_idx_or_off) == 20);
const _: () = assert!(offset_of!(Submission, user_data) == 24);
const _: () = assert!(offset_of!(Submission, args) == 32);

/// Read only through atomics at these field offsets, never as a value.
#[repr(C)]
pub(crate) struct Header {
    pub sq_head: u32,
    pub sq_tail: u32,
    pub sq_mask: u32,
    pub sq_entries: u32,
    pub cq_head: u32,
    pub cq_tail: u32,
    pub cq_mask: u32,
    pub cq_entries: u32,
    pub flags: u32,
    pub dropped: u32,
    pub overflow: u32,
    pub pad: [u32; 5],
}
const _: () = assert!(size_of::<Header>() == 64);
pub(crate) const SQ_HEAD: usize = offset_of!(Header, sq_head);
pub(crate) const SQ_TAIL: usize = offset_of!(Header, sq_tail);
pub(crate) const CQ_HEAD: usize = offset_of!(Header, cq_head);
pub(crate) const CQ_TAIL: usize = offset_of!(Header, cq_tail);
pub(crate) const DROPPED: usize = offset_of!(Header, dropped);

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct BufferRange {
    pub va: u64,
    pub len: u64,
}
const _: () = assert!(size_of::<BufferRange>() == 16);
const _: () = assert!(offset_of!(BufferRange, len) == 8);

pub(crate) const OP_WALK: u8 = 1;
pub(crate) const OP_LOPEN: u8 = 2;
pub(crate) const OP_READ: u8 = 4;
pub(crate) const OP_WRITE: u8 = 5;
pub(crate) const OP_CLUNK: u8 = 10;
pub(crate) const SQE_MULTISHOT: u8 = 1 << 3;
pub(crate) const CQE_MORE: u32 = 1 << 0;
pub(crate) const ENTER_NONBLOCK: u32 = 1 << 1;
pub(crate) const REGISTER_BUFFERS: u32 = 1;
pub(crate) const MAX_REG_BUFFERS: u32 = 64;
