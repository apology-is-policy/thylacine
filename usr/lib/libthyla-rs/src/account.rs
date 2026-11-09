//! The memory account ABI, docs/SHARED-MEMORY-ACCOUNTING.md section 13: the mirror of the
//! kernel's `thylacine/account.h`, held to one byte record by tools/test-account-abi.py.
//! Standalone and syscall-free, so that test can compile it alone.

pub const ACCOUNT_VERSION: u32 = 1;

/// In place of a handle, the caller's own account. No handle is negative.
pub const ACCOUNT_SELF: i32 = -1;

/// No ceiling of the account's own: section 4's default for the class bounds it.
pub const ACCOUNT_LIMIT_NONE: u64 = !0;

pub const ACCOUNT_QUERY: u32 = 1;
pub const ACCOUNT_CHILD: u32 = 2;
pub const ACCOUNT_SET_LIMITS: u32 = 3;
pub const VOUCHER_CREATE: u32 = 4;
pub const VOUCHER_CLOSE: u32 = 5;
pub const ALLOCATE_FUNDED: u32 = 6;

pub const CLASS_ALLOC: u32 = 0;
pub const CLASS_RETAIN: u32 = 1;
pub const CLASS_META: u32 = 2;
pub const CLASS_DEVICE: u32 = 3;
pub const ACCOUNT_CLASSES: usize = 4;

pub const ACCOUNT_NORMAL: u32 = 0;
pub const ACCOUNT_WARNING: u32 = 1;
pub const ACCOUNT_CRITICAL: u32 = 2;

pub const REASON_ACCOUNT_LIMIT: u32 = 1;
pub const REASON_RETENTION_LIMIT: u32 = 2;
pub const REASON_PHYSICAL_PRESSURE: u32 = 3;
pub const REASON_CONTIGUOUS_UNAVAILABLE: u32 = 4;
pub const REASON_DEVICE_BUDGET: u32 = 5;
pub const REASON_METADATA_LIMIT: u32 = 6;
pub const REASON_UNSUPPORTED_LAYOUT: u32 = 7;
pub const REASON_PERMISSION_DENIED: u32 = 8;
pub const REASON_STALE_GRANT: u32 = 9;

#[repr(C)]
#[derive(Copy, Clone, Debug, Default, PartialEq, Eq)]
pub struct AccountRecHdr {
    pub version: u32,
    pub size: u32,
    pub flags: u32,
    pub pad: u32,
    pub incarnation: u64,
}

#[repr(C)]
#[derive(Copy, Clone, Debug, Default, PartialEq, Eq)]
pub struct AccountClass {
    pub used: u64,
    pub reserved: u64,
    pub limit: u64,
}

#[repr(C)]
#[derive(Copy, Clone, Debug, Default, PartialEq, Eq)]
pub struct AccountQuery {
    pub h: AccountRecHdr,
    pub generation: u64,
    pub level: u32,
    pub limiting: u32,
    pub headroom: u64,
    pub reclaim_target: u64,
    pub retained_after_exit: u64,
    pub awaiting_device: u64,
    pub c: [AccountClass; ACCOUNT_CLASSES],
}

/// `ACCOUNT_CHILD`: `h.incarnation` is the account being subdivided. The call returns the
/// new account's control handle and `created` carries its incarnation.
#[repr(C)]
#[derive(Copy, Clone, Debug, Default, PartialEq, Eq)]
pub struct AccountChildRec {
    pub h: AccountRecHdr,
    pub created: u64,
}

/// `ACCOUNT_SET_LIMITS`: the whole vector, in bytes, rounded down to pages. Every class
/// changes or none does.
#[repr(C)]
#[derive(Copy, Clone, Debug, Default, PartialEq, Eq)]
pub struct AccountLimitsRec {
    pub h: AccountRecHdr,
    pub limit: [u64; ACCOUNT_CLASSES],
}

#[repr(C)]
#[derive(Copy, Clone, Debug, Default, PartialEq, Eq)]
pub struct AccountOutcome {
    pub request_id: u64,
    pub cls: u32,
    pub reason: u32,
    pub requested: u64,
    pub available: u64,
    pub limiting_account: u64,
}

const _: () = {
    use core::mem::{offset_of, size_of};
    assert!(size_of::<AccountRecHdr>() == 24);
    assert!(size_of::<AccountClass>() == 24);
    assert!(size_of::<AccountQuery>() == 168);
    assert!(offset_of!(AccountQuery, c) == 72);
    assert!(size_of::<AccountChildRec>() == 32);
    assert!(offset_of!(AccountChildRec, created) == 24);
    assert!(size_of::<AccountLimitsRec>() == 56);
    assert!(offset_of!(AccountLimitsRec, limit) == 24);
    assert!(size_of::<AccountOutcome>() == 40);
};
