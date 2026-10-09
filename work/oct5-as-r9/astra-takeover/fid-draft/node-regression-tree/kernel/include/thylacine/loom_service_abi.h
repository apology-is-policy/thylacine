// Approved private service Loom ABI, docs/ASYNC-SERVICE-ABI.md.
// Reservations only: this header does not add PRIVATE_SERVICE to the legacy
// valid setup mask or enable a handler. Mirror every field/offset in native
// C libt and Rust service_abi; tools/check-loom-service-abi.py compares bytes.
#ifndef THYLACINE_LOOM_SERVICE_ABI_H
#define THYLACINE_LOOM_SERVICE_ABI_H
#include <thylacine/types.h>

#define LOOM_SERVICE_ABI_VERSION 1u
#define LOOM_SETUP_PRIVATE_SERVICE 4u
#define LOOM_REGISTER_SERVICE_TARGET 2u
#define LOOM_REGISTER_SERVICE_SLOT 3u
#define LOOM_REGISTER_ABORT_SCOPE 4u
#define LOOM_REGISTER_QUERY_SERVICE_SLOT 5u
#define LOOM_REGISTER_REAP_SERVICE_SLOT 6u
#define LOOM_OP_SERVICE_CONNECT 20u
#define LOOM_SERVICE_SLOT_COUNT 64u
#define LOOM_SERVICE_TARGET 1u
#define LOOM_SERVICE_SCOPE 2u
#define LOOM_SERVICE_FID 3u
#define LOOM_SERVICE_EMPTY 0u
#define LOOM_SERVICE_RESERVED 1u
#define LOOM_SERVICE_ADMITTED 2u
#define LOOM_SERVICE_VERSION 3u
#define LOOM_SERVICE_ATTACH 4u
#define LOOM_SERVICE_READY 5u
#define LOOM_SERVICE_ABORTING 6u
#define LOOM_SERVICE_RETIRED 7u
#define LOOM_SERVICE_LOCAL_RETIRED 1u
#define LOOM_SERVICE_BYTES_SENT 2u

struct loom_service_ref {
    u32 slot;
    u32 reserved;
    u64 incarnation;
};
_Static_assert(sizeof(struct loom_service_ref) == 16, "loom_service_ref size");
_Static_assert(__builtin_offsetof(struct loom_service_ref, slot) == 0, "loom_service_ref.slot");
_Static_assert(__builtin_offsetof(struct loom_service_ref, reserved) == 4, "loom_service_ref.reserved");
_Static_assert(__builtin_offsetof(struct loom_service_ref, incarnation) == 8, "loom_service_ref.incarnation");

struct loom_service_target {
    u32 size;
    u16 version;
    u16 flags;
    s32 registry_fd;
    u32 name_len;
    u8 name[256];
    struct loom_service_ref result;
};
_Static_assert(sizeof(struct loom_service_target) == 288, "loom_service_target size");
_Static_assert(__builtin_offsetof(struct loom_service_target, size) == 0, "loom_service_target.size");
_Static_assert(__builtin_offsetof(struct loom_service_target, version) == 4, "loom_service_target.version");
_Static_assert(__builtin_offsetof(struct loom_service_target, flags) == 6, "loom_service_target.flags");
_Static_assert(__builtin_offsetof(struct loom_service_target, registry_fd) == 8, "loom_service_target.registry_fd");
_Static_assert(__builtin_offsetof(struct loom_service_target, name_len) == 12, "loom_service_target.name_len");
_Static_assert(__builtin_offsetof(struct loom_service_target, name) == 16, "loom_service_target.name");
_Static_assert(__builtin_offsetof(struct loom_service_target, result) == 272, "loom_service_target.result");

struct loom_service_reserve {
    u32 size;
    u16 version;
    u16 flags;
    u32 kind;
    u32 reserved;
    struct loom_service_ref scope;
    struct loom_service_ref result;
};
_Static_assert(sizeof(struct loom_service_reserve) == 48, "loom_service_reserve size");
_Static_assert(__builtin_offsetof(struct loom_service_reserve, size) == 0, "loom_service_reserve.size");
_Static_assert(__builtin_offsetof(struct loom_service_reserve, version) == 4, "loom_service_reserve.version");
_Static_assert(__builtin_offsetof(struct loom_service_reserve, flags) == 6, "loom_service_reserve.flags");
_Static_assert(__builtin_offsetof(struct loom_service_reserve, kind) == 8, "loom_service_reserve.kind");
_Static_assert(__builtin_offsetof(struct loom_service_reserve, reserved) == 12, "loom_service_reserve.reserved");
_Static_assert(__builtin_offsetof(struct loom_service_reserve, scope) == 16, "loom_service_reserve.scope");
_Static_assert(__builtin_offsetof(struct loom_service_reserve, result) == 32, "loom_service_reserve.result");

struct loom_service_control {
    u32 size;
    u16 version;
    u16 flags;
    struct loom_service_ref object;
    u64 reserved;
};
_Static_assert(sizeof(struct loom_service_control) == 32, "loom_service_control size");
_Static_assert(__builtin_offsetof(struct loom_service_control, size) == 0, "loom_service_control.size");
_Static_assert(__builtin_offsetof(struct loom_service_control, version) == 4, "loom_service_control.version");
_Static_assert(__builtin_offsetof(struct loom_service_control, flags) == 6, "loom_service_control.flags");
_Static_assert(__builtin_offsetof(struct loom_service_control, object) == 8, "loom_service_control.object");
_Static_assert(__builtin_offsetof(struct loom_service_control, reserved) == 24, "loom_service_control.reserved");

struct loom_service_snapshot {
    u32 size;
    u16 version;
    u16 flags;
    struct loom_service_ref object;
    u32 kind;
    u32 state;
    struct loom_service_ref scope;
    u32 active_ops;
    u32 pending_terminals;
    s32 reason;
    u32 status_flags;
};
_Static_assert(sizeof(struct loom_service_snapshot) == 64, "loom_service_snapshot size");
_Static_assert(__builtin_offsetof(struct loom_service_snapshot, size) == 0, "loom_service_snapshot.size");
_Static_assert(__builtin_offsetof(struct loom_service_snapshot, version) == 4, "loom_service_snapshot.version");
_Static_assert(__builtin_offsetof(struct loom_service_snapshot, flags) == 6, "loom_service_snapshot.flags");
_Static_assert(__builtin_offsetof(struct loom_service_snapshot, object) == 8, "loom_service_snapshot.object");
_Static_assert(__builtin_offsetof(struct loom_service_snapshot, kind) == 24, "loom_service_snapshot.kind");
_Static_assert(__builtin_offsetof(struct loom_service_snapshot, state) == 28, "loom_service_snapshot.state");
_Static_assert(__builtin_offsetof(struct loom_service_snapshot, scope) == 32, "loom_service_snapshot.scope");
_Static_assert(__builtin_offsetof(struct loom_service_snapshot, active_ops) == 48, "loom_service_snapshot.active_ops");
_Static_assert(__builtin_offsetof(struct loom_service_snapshot, pending_terminals) == 52, "loom_service_snapshot.pending_terminals");
_Static_assert(__builtin_offsetof(struct loom_service_snapshot, reason) == 56, "loom_service_snapshot.reason");
_Static_assert(__builtin_offsetof(struct loom_service_snapshot, status_flags) == 60, "loom_service_snapshot.status_flags");


// Provided-buffer reservations, scripture30695b43e and ASYNC-SERVICE-BUFFERS.md.
// No valid mask, handler, or runtime support is enabled by these declarations.
#define LOOM_SETUP_SERVICE_BUFFERS 8u
#define LOOM_REGISTER_SERVICE_POOL 7u
#define LOOM_REGISTER_RETURN_SERVICE_BUFFER 8u
#define LOOM_REGISTER_QUERY_SERVICE_POOL 9u
#define LOOM_SERVICE_POOL 4u
#define LOOM_SERVICE_POOL_MEMBERS 64u
#define LOOM_SQE_BUFFER_SELECT 16u
#define LOOM_CQE_SERVICE_BUFFER 4u

struct loom_service_pool_member {
    u32 buffer_index;
    u32 reserved;
    u64 offset;
    u64 length;
};
_Static_assert(sizeof(struct loom_service_pool_member) == 24, "loom_service_pool_member size");
_Static_assert(_Alignof(struct loom_service_pool_member) == 8, "loom_service_pool_member alignment");
_Static_assert(__builtin_offsetof(struct loom_service_pool_member, buffer_index) == 0, "loom_service_pool_member.buffer_index");
_Static_assert(__builtin_offsetof(struct loom_service_pool_member, reserved) == 4, "loom_service_pool_member.reserved");
_Static_assert(__builtin_offsetof(struct loom_service_pool_member, offset) == 8, "loom_service_pool_member.offset");
_Static_assert(__builtin_offsetof(struct loom_service_pool_member, length) == 16, "loom_service_pool_member.length");

struct loom_service_pool_create {
    u32 size;
    u16 version;
    u16 flags;
    u32 count;
    u32 reserved;
    struct loom_service_ref result;
    struct loom_service_pool_member members[64];
};
_Static_assert(sizeof(struct loom_service_pool_create) == 1568, "loom_service_pool_create size");
_Static_assert(_Alignof(struct loom_service_pool_create) == 8, "loom_service_pool_create alignment");
_Static_assert(__builtin_offsetof(struct loom_service_pool_create, size) == 0, "loom_service_pool_create.size");
_Static_assert(__builtin_offsetof(struct loom_service_pool_create, version) == 4, "loom_service_pool_create.version");
_Static_assert(__builtin_offsetof(struct loom_service_pool_create, flags) == 6, "loom_service_pool_create.flags");
_Static_assert(__builtin_offsetof(struct loom_service_pool_create, count) == 8, "loom_service_pool_create.count");
_Static_assert(__builtin_offsetof(struct loom_service_pool_create, reserved) == 12, "loom_service_pool_create.reserved");
_Static_assert(__builtin_offsetof(struct loom_service_pool_create, result) == 16, "loom_service_pool_create.result");
_Static_assert(__builtin_offsetof(struct loom_service_pool_create, members) == 32, "loom_service_pool_create.members");

struct loom_service_buffer_receipt {
    struct loom_service_ref pool;
    u32 member;
    u32 reserved;
    u64 lease;
};
_Static_assert(sizeof(struct loom_service_buffer_receipt) == 32, "loom_service_buffer_receipt size");
_Static_assert(_Alignof(struct loom_service_buffer_receipt) == 8, "loom_service_buffer_receipt alignment");
_Static_assert(__builtin_offsetof(struct loom_service_buffer_receipt, pool) == 0, "loom_service_buffer_receipt.pool");
_Static_assert(__builtin_offsetof(struct loom_service_buffer_receipt, member) == 16, "loom_service_buffer_receipt.member");
_Static_assert(__builtin_offsetof(struct loom_service_buffer_receipt, reserved) == 20, "loom_service_buffer_receipt.reserved");
_Static_assert(__builtin_offsetof(struct loom_service_buffer_receipt, lease) == 24, "loom_service_buffer_receipt.lease");

struct loom_service_buffer_return {
    u32 size;
    u16 version;
    u16 flags;
    struct loom_service_buffer_receipt receipt;
};
_Static_assert(sizeof(struct loom_service_buffer_return) == 40, "loom_service_buffer_return size");
_Static_assert(_Alignof(struct loom_service_buffer_return) == 8, "loom_service_buffer_return alignment");
_Static_assert(__builtin_offsetof(struct loom_service_buffer_return, size) == 0, "loom_service_buffer_return.size");
_Static_assert(__builtin_offsetof(struct loom_service_buffer_return, version) == 4, "loom_service_buffer_return.version");
_Static_assert(__builtin_offsetof(struct loom_service_buffer_return, flags) == 6, "loom_service_buffer_return.flags");
_Static_assert(__builtin_offsetof(struct loom_service_buffer_return, receipt) == 8, "loom_service_buffer_return.receipt");

struct loom_service_pool_snapshot {
    u32 size;
    u16 version;
    u16 flags;
    struct loom_service_ref pool;
    u32 members;
    u32 available;
    u32 busy;
    u32 pending;
    u32 leased;
    u32 streams;
    u64 reserved[2];
};
_Static_assert(sizeof(struct loom_service_pool_snapshot) == 64, "loom_service_pool_snapshot size");
_Static_assert(_Alignof(struct loom_service_pool_snapshot) == 8, "loom_service_pool_snapshot alignment");
_Static_assert(__builtin_offsetof(struct loom_service_pool_snapshot, size) == 0, "loom_service_pool_snapshot.size");
_Static_assert(__builtin_offsetof(struct loom_service_pool_snapshot, version) == 4, "loom_service_pool_snapshot.version");
_Static_assert(__builtin_offsetof(struct loom_service_pool_snapshot, flags) == 6, "loom_service_pool_snapshot.flags");
_Static_assert(__builtin_offsetof(struct loom_service_pool_snapshot, pool) == 8, "loom_service_pool_snapshot.pool");
_Static_assert(__builtin_offsetof(struct loom_service_pool_snapshot, members) == 24, "loom_service_pool_snapshot.members");
_Static_assert(__builtin_offsetof(struct loom_service_pool_snapshot, available) == 28, "loom_service_pool_snapshot.available");
_Static_assert(__builtin_offsetof(struct loom_service_pool_snapshot, busy) == 32, "loom_service_pool_snapshot.busy");
_Static_assert(__builtin_offsetof(struct loom_service_pool_snapshot, pending) == 36, "loom_service_pool_snapshot.pending");
_Static_assert(__builtin_offsetof(struct loom_service_pool_snapshot, leased) == 40, "loom_service_pool_snapshot.leased");
_Static_assert(__builtin_offsetof(struct loom_service_pool_snapshot, streams) == 44, "loom_service_pool_snapshot.streams");
_Static_assert(__builtin_offsetof(struct loom_service_pool_snapshot, reserved) == 48, "loom_service_pool_snapshot.reserved");

#endif
