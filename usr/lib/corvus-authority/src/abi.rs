//! UA-0 wire reservations, not an enabled protocol or a bearer credential.
//! Byte-array layouts deliberately have alignment 1. All integers are LE.
//! C mirror: kernel/include/thylacine/authority_wire.h.
pub const AUTHORITY_VERSION: usize = 1;
pub const AUTHORITY_QUERY: usize = 21;
pub const AUTHORITY_PREPARE: usize = 22;
pub const AUTHORITY_REQUEST: usize = 23;
pub const AUTHORITY_STATUS: usize = 24;
pub const AUTHORITY_CANCEL: usize = 25;
pub const MANDATE_MAGIC: usize = 0x4d54444d;
pub const MANDATE_VERSION: usize = 1;
pub const MANDATE_HEADER_LEN: usize = 96;
pub const ENVELOPE_HEADER_LEN: usize = 32;
pub const MANDATE_MAX_LEN: usize = 896;
pub const KIND_USE: usize = 1;
pub const KIND_ACTIVATE: usize = 2;
pub const KIND_ADMIN: usize = 3;
pub const AUTH_SESSION: usize = 0;
pub const AUTH_DISTINCT_KEY: usize = 1;
pub const AUTH_FOUNDING: usize = 2;
pub const TERM_UNTIL_REVOKED: usize = 0;
pub const TERM_UNTIL_UTC: usize = 1;
pub const STATE_LIVE: usize = 1;
pub const STATE_REVOKING: usize = 2;
pub const STATE_REVOKED: usize = 3;
pub const MAX_MUTATION_PAYLOAD: usize = 4096;
pub const MAX_RESPONSE_PAGE: usize = 8192;
pub const MAX_PAGE_RECORDS: usize = 64;

#[repr(C)]
pub struct MandateHeader {
    pub magic: [u8; 4],
    pub version: [u8; 2],
    pub reserved: [u8; 2],
    pub total_len: [u8; 4],
    pub kind: [u8; 1],
    pub authentication: [u8; 1],
    pub state: [u8; 1],
    pub term_kind: [u8; 1],
    pub id: [u8; 8],
    pub revision: [u8; 8],
    pub subject: [u8; 4],
    pub issuer: [u8; 4],
    pub domain: [u8; 8],
    pub domain_generation: [u8; 8],
    pub actions: [u8; 8],
    pub term_end: [u8; 8],
    pub transaction: [u8; 16],
    pub subject_count: [u8; 2],
    pub resource_count: [u8; 2],
    pub support_count: [u8; 2],
    pub envelope_present: [u8; 1],
    pub reserved_tail: [u8; 1],
}
const _: () = assert!(core::mem::size_of::<MandateHeader>() == 96);
const _: () = assert!(core::mem::offset_of!(MandateHeader, magic) == 0);
const _: () = assert!(core::mem::offset_of!(MandateHeader, version) == 4);
const _: () = assert!(core::mem::offset_of!(MandateHeader, reserved) == 6);
const _: () = assert!(core::mem::offset_of!(MandateHeader, total_len) == 8);
const _: () = assert!(core::mem::offset_of!(MandateHeader, kind) == 12);
const _: () = assert!(core::mem::offset_of!(MandateHeader, authentication) == 13);
const _: () = assert!(core::mem::offset_of!(MandateHeader, state) == 14);
const _: () = assert!(core::mem::offset_of!(MandateHeader, term_kind) == 15);
const _: () = assert!(core::mem::offset_of!(MandateHeader, id) == 16);
const _: () = assert!(core::mem::offset_of!(MandateHeader, revision) == 24);
const _: () = assert!(core::mem::offset_of!(MandateHeader, subject) == 32);
const _: () = assert!(core::mem::offset_of!(MandateHeader, issuer) == 36);
const _: () = assert!(core::mem::offset_of!(MandateHeader, domain) == 40);
const _: () = assert!(core::mem::offset_of!(MandateHeader, domain_generation) == 48);
const _: () = assert!(core::mem::offset_of!(MandateHeader, actions) == 56);
const _: () = assert!(core::mem::offset_of!(MandateHeader, term_end) == 64);
const _: () = assert!(core::mem::offset_of!(MandateHeader, transaction) == 72);
const _: () = assert!(core::mem::offset_of!(MandateHeader, subject_count) == 88);
const _: () = assert!(core::mem::offset_of!(MandateHeader, resource_count) == 90);
const _: () = assert!(core::mem::offset_of!(MandateHeader, support_count) == 92);
const _: () = assert!(core::mem::offset_of!(MandateHeader, envelope_present) == 94);
const _: () = assert!(core::mem::offset_of!(MandateHeader, reserved_tail) == 95);

#[repr(C)]
pub struct EnvelopeHeader {
    pub domain: [u8; 8],
    pub actions: [u8; 8],
    pub term_end: [u8; 8],
    pub kinds: [u8; 1],
    pub auth_floor: [u8; 1],
    pub delegation_depth: [u8; 1],
    pub term_kind: [u8; 1],
    pub subject_count: [u8; 2],
    pub resource_count: [u8; 2],
}
const _: () = assert!(core::mem::size_of::<EnvelopeHeader>() == 32);
const _: () = assert!(core::mem::offset_of!(EnvelopeHeader, domain) == 0);
const _: () = assert!(core::mem::offset_of!(EnvelopeHeader, actions) == 8);
const _: () = assert!(core::mem::offset_of!(EnvelopeHeader, term_end) == 16);
const _: () = assert!(core::mem::offset_of!(EnvelopeHeader, kinds) == 24);
const _: () = assert!(core::mem::offset_of!(EnvelopeHeader, auth_floor) == 25);
const _: () = assert!(core::mem::offset_of!(EnvelopeHeader, delegation_depth) == 26);
const _: () = assert!(core::mem::offset_of!(EnvelopeHeader, term_kind) == 27);
const _: () = assert!(core::mem::offset_of!(EnvelopeHeader, subject_count) == 28);
const _: () = assert!(core::mem::offset_of!(EnvelopeHeader, resource_count) == 30);

pub const ACTION_ENROLL: u64 = 0x1;
pub const ACTION_PROFILE: u64 = 0x2;
pub const ACTION_SUSPEND: u64 = 0x4;
pub const ACTION_RESUME: u64 = 0x8;
pub const ACTION_RETIRE: u64 = 0x10;
pub const ACTION_GROUP_CREATE: u64 = 0x20;
pub const ACTION_GROUP_MEMBERSHIP: u64 = 0x40;
pub const ACTION_GRANT: u64 = 0x80;
pub const ACTION_REVOKE: u64 = 0x100;
pub const ACTION_DELEGATE: u64 = 0x200;
pub const ACTION_CLEARANCE_ENROLL: u64 = 0x400;
pub const ACTION_KEY_RESET: u64 = 0x800;
pub const ACTION_ROTATE_DOMAIN: u64 = 0x1000;
pub const ACTION_FLOOR_DEFINE: u64 = 0x2000;
pub const ACTION_AUDIT_READ: u64 = 0x4000;
pub const ACTION_FS_READ: u64 = 0x100000000;
pub const ACTION_FS_WRITE: u64 = 0x200000000;
pub const ACTION_FS_CHOWN: u64 = 0x400000000;
pub const ACTION_NET_CONNECT: u64 = 0x800000000;
pub const ACTION_NET_LISTEN: u64 = 0x1000000000;
pub const ACTION_SIGNAL: u64 = 0x2000000000;
pub const ACTION_POST_SERVICE: u64 = 0x4000000000;
