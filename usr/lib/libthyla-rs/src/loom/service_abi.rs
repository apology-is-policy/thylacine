//! Approved Loom private-service ABI reservations; no runtime support yet.
//! Keep in sync with kernel/loom_service_abi.h and libt/thyla/loom_service.h.
//! See docs/ASYNC-SERVICE-ABI.md; no syscall wrappers are enabled here.

pub const SERVICE_ABI_VERSION: u32 = 1;
pub const SETUP_PRIVATE_SERVICE: u32 = 4;
pub const REGISTER_SERVICE_TARGET: u32 = 2;
pub const REGISTER_SERVICE_SLOT: u32 = 3;
pub const REGISTER_ABORT_SCOPE: u32 = 4;
pub const REGISTER_QUERY_SERVICE_SLOT: u32 = 5;
pub const REGISTER_REAP_SERVICE_SLOT: u32 = 6;
pub const OP_SERVICE_CONNECT: u32 = 20;
pub const SERVICE_SLOT_COUNT: u32 = 64;
pub const SERVICE_TARGET: u32 = 1;
pub const SERVICE_SCOPE: u32 = 2;
pub const SERVICE_FID: u32 = 3;
pub const SERVICE_EMPTY: u32 = 0;
pub const SERVICE_RESERVED: u32 = 1;
pub const SERVICE_ADMITTED: u32 = 2;
pub const SERVICE_VERSION: u32 = 3;
pub const SERVICE_ATTACH: u32 = 4;
pub const SERVICE_READY: u32 = 5;
pub const SERVICE_ABORTING: u32 = 6;
pub const SERVICE_RETIRED: u32 = 7;
pub const SERVICE_LOCAL_RETIRED: u32 = 1;
pub const SERVICE_BYTES_SENT: u32 = 2;

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SlotRef {
    pub slot: u32,
    pub reserved: u32,
    pub incarnation: u64,
}
const _: () = assert!(core::mem::size_of::<SlotRef>() == 16);
const _: () = assert!(core::mem::offset_of!(SlotRef, slot) == 0);
const _: () = assert!(core::mem::offset_of!(SlotRef, reserved) == 4);
const _: () = assert!(core::mem::offset_of!(SlotRef, incarnation) == 8);

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Target {
    pub size: u32,
    pub version: u16,
    pub flags: u16,
    pub registry_fd: i32,
    pub name_len: u32,
    pub name: [u8; 256],
    pub result: SlotRef,
}
const _: () = assert!(core::mem::size_of::<Target>() == 288);
const _: () = assert!(core::mem::offset_of!(Target, size) == 0);
const _: () = assert!(core::mem::offset_of!(Target, version) == 4);
const _: () = assert!(core::mem::offset_of!(Target, flags) == 6);
const _: () = assert!(core::mem::offset_of!(Target, registry_fd) == 8);
const _: () = assert!(core::mem::offset_of!(Target, name_len) == 12);
const _: () = assert!(core::mem::offset_of!(Target, name) == 16);
const _: () = assert!(core::mem::offset_of!(Target, result) == 272);

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Reservation {
    pub size: u32,
    pub version: u16,
    pub flags: u16,
    pub kind: u32,
    pub reserved: u32,
    pub scope: SlotRef,
    pub result: SlotRef,
}
const _: () = assert!(core::mem::size_of::<Reservation>() == 48);
const _: () = assert!(core::mem::offset_of!(Reservation, size) == 0);
const _: () = assert!(core::mem::offset_of!(Reservation, version) == 4);
const _: () = assert!(core::mem::offset_of!(Reservation, flags) == 6);
const _: () = assert!(core::mem::offset_of!(Reservation, kind) == 8);
const _: () = assert!(core::mem::offset_of!(Reservation, reserved) == 12);
const _: () = assert!(core::mem::offset_of!(Reservation, scope) == 16);
const _: () = assert!(core::mem::offset_of!(Reservation, result) == 32);

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Control {
    pub size: u32,
    pub version: u16,
    pub flags: u16,
    pub object: SlotRef,
    pub reserved: u64,
}
const _: () = assert!(core::mem::size_of::<Control>() == 32);
const _: () = assert!(core::mem::offset_of!(Control, size) == 0);
const _: () = assert!(core::mem::offset_of!(Control, version) == 4);
const _: () = assert!(core::mem::offset_of!(Control, flags) == 6);
const _: () = assert!(core::mem::offset_of!(Control, object) == 8);
const _: () = assert!(core::mem::offset_of!(Control, reserved) == 24);

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Snapshot {
    pub size: u32,
    pub version: u16,
    pub flags: u16,
    pub object: SlotRef,
    pub kind: u32,
    pub state: u32,
    pub scope: SlotRef,
    pub active_ops: u32,
    pub pending_terminals: u32,
    pub reason: i32,
    pub status_flags: u32,
}
const _: () = assert!(core::mem::size_of::<Snapshot>() == 64);
const _: () = assert!(core::mem::offset_of!(Snapshot, size) == 0);
const _: () = assert!(core::mem::offset_of!(Snapshot, version) == 4);
const _: () = assert!(core::mem::offset_of!(Snapshot, flags) == 6);
const _: () = assert!(core::mem::offset_of!(Snapshot, object) == 8);
const _: () = assert!(core::mem::offset_of!(Snapshot, kind) == 24);
const _: () = assert!(core::mem::offset_of!(Snapshot, state) == 28);
const _: () = assert!(core::mem::offset_of!(Snapshot, scope) == 32);
const _: () = assert!(core::mem::offset_of!(Snapshot, active_ops) == 48);
const _: () = assert!(core::mem::offset_of!(Snapshot, pending_terminals) == 52);
const _: () = assert!(core::mem::offset_of!(Snapshot, reason) == 56);
const _: () = assert!(core::mem::offset_of!(Snapshot, status_flags) == 60);


// Provided-buffer reservations, scripture30695b43e. No runtime activation.
pub const SETUP_SERVICE_BUFFERS: u32 = 8;
pub const REGISTER_SERVICE_POOL: u32 = 7;
pub const REGISTER_RETURN_SERVICE_BUFFER: u32 = 8;
pub const REGISTER_QUERY_SERVICE_POOL: u32 = 9;
pub const SERVICE_POOL: u32 = 4;
pub const SERVICE_POOL_MEMBERS: u32 = 64;
pub const SQE_BUFFER_SELECT: u32 = 16;
pub const CQE_SERVICE_BUFFER: u32 = 4;

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PoolMember {
    pub buffer_index: u32,
    pub reserved: u32,
    pub offset: u64,
    pub length: u64,
}
const _: () = assert!(core::mem::size_of::<PoolMember>() == 24);
const _: () = assert!(core::mem::align_of::<PoolMember>() == 8);
const _: () = assert!(core::mem::offset_of!(PoolMember, buffer_index) == 0);
const _: () = assert!(core::mem::offset_of!(PoolMember, reserved) == 4);
const _: () = assert!(core::mem::offset_of!(PoolMember, offset) == 8);
const _: () = assert!(core::mem::offset_of!(PoolMember, length) == 16);

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PoolCreate {
    pub size: u32,
    pub version: u16,
    pub flags: u16,
    pub count: u32,
    pub reserved: u32,
    pub result: SlotRef,
    pub members: [PoolMember; 64],
}
const _: () = assert!(core::mem::size_of::<PoolCreate>() == 1568);
const _: () = assert!(core::mem::align_of::<PoolCreate>() == 8);
const _: () = assert!(core::mem::offset_of!(PoolCreate, size) == 0);
const _: () = assert!(core::mem::offset_of!(PoolCreate, version) == 4);
const _: () = assert!(core::mem::offset_of!(PoolCreate, flags) == 6);
const _: () = assert!(core::mem::offset_of!(PoolCreate, count) == 8);
const _: () = assert!(core::mem::offset_of!(PoolCreate, reserved) == 12);
const _: () = assert!(core::mem::offset_of!(PoolCreate, result) == 16);
const _: () = assert!(core::mem::offset_of!(PoolCreate, members) == 32);

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BufferReceipt {
    pub pool: SlotRef,
    pub member: u32,
    pub reserved: u32,
    pub lease: u64,
}
const _: () = assert!(core::mem::size_of::<BufferReceipt>() == 32);
const _: () = assert!(core::mem::align_of::<BufferReceipt>() == 8);
const _: () = assert!(core::mem::offset_of!(BufferReceipt, pool) == 0);
const _: () = assert!(core::mem::offset_of!(BufferReceipt, member) == 16);
const _: () = assert!(core::mem::offset_of!(BufferReceipt, reserved) == 20);
const _: () = assert!(core::mem::offset_of!(BufferReceipt, lease) == 24);

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BufferReturn {
    pub size: u32,
    pub version: u16,
    pub flags: u16,
    pub receipt: BufferReceipt,
}
const _: () = assert!(core::mem::size_of::<BufferReturn>() == 40);
const _: () = assert!(core::mem::align_of::<BufferReturn>() == 8);
const _: () = assert!(core::mem::offset_of!(BufferReturn, size) == 0);
const _: () = assert!(core::mem::offset_of!(BufferReturn, version) == 4);
const _: () = assert!(core::mem::offset_of!(BufferReturn, flags) == 6);
const _: () = assert!(core::mem::offset_of!(BufferReturn, receipt) == 8);

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PoolSnapshot {
    pub size: u32,
    pub version: u16,
    pub flags: u16,
    pub pool: SlotRef,
    pub members: u32,
    pub available: u32,
    pub busy: u32,
    pub pending: u32,
    pub leased: u32,
    pub streams: u32,
    pub reserved: [u64; 2],
}
const _: () = assert!(core::mem::size_of::<PoolSnapshot>() == 64);
const _: () = assert!(core::mem::align_of::<PoolSnapshot>() == 8);
const _: () = assert!(core::mem::offset_of!(PoolSnapshot, size) == 0);
const _: () = assert!(core::mem::offset_of!(PoolSnapshot, version) == 4);
const _: () = assert!(core::mem::offset_of!(PoolSnapshot, flags) == 6);
const _: () = assert!(core::mem::offset_of!(PoolSnapshot, pool) == 8);
const _: () = assert!(core::mem::offset_of!(PoolSnapshot, members) == 24);
const _: () = assert!(core::mem::offset_of!(PoolSnapshot, available) == 28);
const _: () = assert!(core::mem::offset_of!(PoolSnapshot, busy) == 32);
const _: () = assert!(core::mem::offset_of!(PoolSnapshot, pending) == 36);
const _: () = assert!(core::mem::offset_of!(PoolSnapshot, leased) == 40);
const _: () = assert!(core::mem::offset_of!(PoolSnapshot, streams) == 44);
const _: () = assert!(core::mem::offset_of!(PoolSnapshot, reserved) == 48);
