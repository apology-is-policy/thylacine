//! Terminal ownership ABI records; live client wrappers are separate.
//! See docs/HALCYON-INTERACTION-PTY-ABI.md. Native AArch64 is little-endian.
//! The compile-time assertions and tools/test-pty-interaction-abi.py pin all
//! three mirrors against literal records independently of syscall availability.

pub const T_PTY_INTERACTION_BIND: u64 = 16;
pub const T_PTY_INTERACTION_UNBIND: u64 = 17;
pub const T_PTY_INTERACTION_WATCH: u64 = 18;
pub const T_PTY_INTERACTION_STATE: u64 = 19;
pub const T_PTY_INTERACTION_ACK: u64 = 20;
pub const T_PTY_INTERACTION_CHECK: u64 = 21;
pub const T_PTY_INTERACTION_VERSION: u64 = 1;
pub const T_PTY_INTERACTION_STATE_BYTES: u64 = 80;
pub const T_PTY_INTERACTION_CHECK_BYTES: u64 = 24;
pub const T_PTY_INTERACTION_LIVE: u64 = 1;
pub const T_PTY_INTERACTION_ACKNOWLEDGED: u64 = 2;
pub const T_PTY_INTERACTION_ID_MAX: u64 = 0x7fff_ffff_ffff_ffff;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TPtyInteractionState {
    pub version: u32,
    pub flags: u32,
    pub binding_id: u64,
    pub pts_id: u64,
    pub foreground_epoch: u64,
    pub acknowledged_epoch: u64,
    pub revision: u64,
    pub controlling_sid: u32,
    pub foreground_pgid: u32,
    pub subject_stripes: u64,
    pub binder_stripes: u64,
    pub binder_pid: u32,
    pub reserved: u32,
}
const _: () = assert!(core::mem::size_of::<TPtyInteractionState>() == 80);
const _: () = assert!(core::mem::align_of::<TPtyInteractionState>() == 8);
const _: () = assert!(core::mem::offset_of!(TPtyInteractionState, version) == 0);
const _: () = assert!(core::mem::offset_of!(TPtyInteractionState, flags) == 4);
const _: () = assert!(core::mem::offset_of!(TPtyInteractionState, binding_id) == 8);
const _: () = assert!(core::mem::offset_of!(TPtyInteractionState, pts_id) == 16);
const _: () = assert!(core::mem::offset_of!(TPtyInteractionState, foreground_epoch) == 24);
const _: () = assert!(core::mem::offset_of!(TPtyInteractionState, acknowledged_epoch) == 32);
const _: () = assert!(core::mem::offset_of!(TPtyInteractionState, revision) == 40);
const _: () = assert!(core::mem::offset_of!(TPtyInteractionState, controlling_sid) == 48);
const _: () = assert!(core::mem::offset_of!(TPtyInteractionState, foreground_pgid) == 52);
const _: () = assert!(core::mem::offset_of!(TPtyInteractionState, subject_stripes) == 56);
const _: () = assert!(core::mem::offset_of!(TPtyInteractionState, binder_stripes) == 64);
const _: () = assert!(core::mem::offset_of!(TPtyInteractionState, binder_pid) == 72);
const _: () = assert!(core::mem::offset_of!(TPtyInteractionState, reserved) == 76);

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TPtyInteractionCheck {
    pub version: u32,
    pub size: u32,
    pub expected_epoch: u64,
    pub subject_stripes: u64,
}
const _: () = assert!(core::mem::size_of::<TPtyInteractionCheck>() == 24);
const _: () = assert!(core::mem::align_of::<TPtyInteractionCheck>() == 8);
const _: () = assert!(core::mem::offset_of!(TPtyInteractionCheck, version) == 0);
const _: () = assert!(core::mem::offset_of!(TPtyInteractionCheck, size) == 4);
const _: () = assert!(core::mem::offset_of!(TPtyInteractionCheck, expected_epoch) == 8);
const _: () = assert!(core::mem::offset_of!(TPtyInteractionCheck, subject_stripes) == 16);
