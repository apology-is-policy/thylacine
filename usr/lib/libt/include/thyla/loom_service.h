// Approved private service Loom ABI, docs/ASYNC-SERVICE-ABI.md.
// Reservations only: this header does not add PRIVATE_SERVICE to the legacy
// valid setup mask or enable a handler. Mirror every field/offset in native
// C libt and Rust service_abi; tools/check-loom-service-abi.py compares bytes.
#ifndef THYLA_LOOM_SERVICE_H
#define THYLA_LOOM_SERVICE_H
#include <stdint.h>

#define T_LOOM_SERVICE_ABI_VERSION 1u
#define T_LOOM_SETUP_PRIVATE_SERVICE 4u
#define T_LOOM_REGISTER_SERVICE_TARGET 2u
#define T_LOOM_REGISTER_SERVICE_SLOT 3u
#define T_LOOM_REGISTER_ABORT_SCOPE 4u
#define T_LOOM_REGISTER_QUERY_SERVICE_SLOT 5u
#define T_LOOM_REGISTER_REAP_SERVICE_SLOT 6u
#define T_LOOM_OP_SERVICE_CONNECT 20u
#define T_LOOM_SERVICE_SLOT_COUNT 64u
#define T_LOOM_SERVICE_TARGET 1u
#define T_LOOM_SERVICE_SCOPE 2u
#define T_LOOM_SERVICE_FID 3u
#define T_LOOM_SERVICE_EMPTY 0u
#define T_LOOM_SERVICE_RESERVED 1u
#define T_LOOM_SERVICE_ADMITTED 2u
#define T_LOOM_SERVICE_VERSION 3u
#define T_LOOM_SERVICE_ATTACH 4u
#define T_LOOM_SERVICE_READY 5u
#define T_LOOM_SERVICE_ABORTING 6u
#define T_LOOM_SERVICE_RETIRED 7u
#define T_LOOM_SERVICE_LOCAL_RETIRED 1u
#define T_LOOM_SERVICE_BYTES_SENT 2u

struct t_loom_service_ref {
    uint32_t slot;
    uint32_t reserved;
    uint64_t incarnation;
};
_Static_assert(sizeof(struct t_loom_service_ref) == 16, "loom_service_ref size");
_Static_assert(__builtin_offsetof(struct t_loom_service_ref, slot) == 0, "loom_service_ref.slot");
_Static_assert(__builtin_offsetof(struct t_loom_service_ref, reserved) == 4, "loom_service_ref.reserved");
_Static_assert(__builtin_offsetof(struct t_loom_service_ref, incarnation) == 8, "loom_service_ref.incarnation");

struct t_loom_service_target {
    uint32_t size;
    uint16_t version;
    uint16_t flags;
    int32_t registry_fd;
    uint32_t name_len;
    uint8_t name[256];
    struct t_loom_service_ref result;
};
_Static_assert(sizeof(struct t_loom_service_target) == 288, "loom_service_target size");
_Static_assert(__builtin_offsetof(struct t_loom_service_target, size) == 0, "loom_service_target.size");
_Static_assert(__builtin_offsetof(struct t_loom_service_target, version) == 4, "loom_service_target.version");
_Static_assert(__builtin_offsetof(struct t_loom_service_target, flags) == 6, "loom_service_target.flags");
_Static_assert(__builtin_offsetof(struct t_loom_service_target, registry_fd) == 8, "loom_service_target.registry_fd");
_Static_assert(__builtin_offsetof(struct t_loom_service_target, name_len) == 12, "loom_service_target.name_len");
_Static_assert(__builtin_offsetof(struct t_loom_service_target, name) == 16, "loom_service_target.name");
_Static_assert(__builtin_offsetof(struct t_loom_service_target, result) == 272, "loom_service_target.result");

struct t_loom_service_reserve {
    uint32_t size;
    uint16_t version;
    uint16_t flags;
    uint32_t kind;
    uint32_t reserved;
    struct t_loom_service_ref scope;
    struct t_loom_service_ref result;
};
_Static_assert(sizeof(struct t_loom_service_reserve) == 48, "loom_service_reserve size");
_Static_assert(__builtin_offsetof(struct t_loom_service_reserve, size) == 0, "loom_service_reserve.size");
_Static_assert(__builtin_offsetof(struct t_loom_service_reserve, version) == 4, "loom_service_reserve.version");
_Static_assert(__builtin_offsetof(struct t_loom_service_reserve, flags) == 6, "loom_service_reserve.flags");
_Static_assert(__builtin_offsetof(struct t_loom_service_reserve, kind) == 8, "loom_service_reserve.kind");
_Static_assert(__builtin_offsetof(struct t_loom_service_reserve, reserved) == 12, "loom_service_reserve.reserved");
_Static_assert(__builtin_offsetof(struct t_loom_service_reserve, scope) == 16, "loom_service_reserve.scope");
_Static_assert(__builtin_offsetof(struct t_loom_service_reserve, result) == 32, "loom_service_reserve.result");

struct t_loom_service_control {
    uint32_t size;
    uint16_t version;
    uint16_t flags;
    struct t_loom_service_ref object;
    uint64_t reserved;
};
_Static_assert(sizeof(struct t_loom_service_control) == 32, "loom_service_control size");
_Static_assert(__builtin_offsetof(struct t_loom_service_control, size) == 0, "loom_service_control.size");
_Static_assert(__builtin_offsetof(struct t_loom_service_control, version) == 4, "loom_service_control.version");
_Static_assert(__builtin_offsetof(struct t_loom_service_control, flags) == 6, "loom_service_control.flags");
_Static_assert(__builtin_offsetof(struct t_loom_service_control, object) == 8, "loom_service_control.object");
_Static_assert(__builtin_offsetof(struct t_loom_service_control, reserved) == 24, "loom_service_control.reserved");

struct t_loom_service_snapshot {
    uint32_t size;
    uint16_t version;
    uint16_t flags;
    struct t_loom_service_ref object;
    uint32_t kind;
    uint32_t state;
    struct t_loom_service_ref scope;
    uint32_t active_ops;
    uint32_t pending_terminals;
    int32_t reason;
    uint32_t status_flags;
};
_Static_assert(sizeof(struct t_loom_service_snapshot) == 64, "loom_service_snapshot size");
_Static_assert(__builtin_offsetof(struct t_loom_service_snapshot, size) == 0, "loom_service_snapshot.size");
_Static_assert(__builtin_offsetof(struct t_loom_service_snapshot, version) == 4, "loom_service_snapshot.version");
_Static_assert(__builtin_offsetof(struct t_loom_service_snapshot, flags) == 6, "loom_service_snapshot.flags");
_Static_assert(__builtin_offsetof(struct t_loom_service_snapshot, object) == 8, "loom_service_snapshot.object");
_Static_assert(__builtin_offsetof(struct t_loom_service_snapshot, kind) == 24, "loom_service_snapshot.kind");
_Static_assert(__builtin_offsetof(struct t_loom_service_snapshot, state) == 28, "loom_service_snapshot.state");
_Static_assert(__builtin_offsetof(struct t_loom_service_snapshot, scope) == 32, "loom_service_snapshot.scope");
_Static_assert(__builtin_offsetof(struct t_loom_service_snapshot, active_ops) == 48, "loom_service_snapshot.active_ops");
_Static_assert(__builtin_offsetof(struct t_loom_service_snapshot, pending_terminals) == 52, "loom_service_snapshot.pending_terminals");
_Static_assert(__builtin_offsetof(struct t_loom_service_snapshot, reason) == 56, "loom_service_snapshot.reason");
_Static_assert(__builtin_offsetof(struct t_loom_service_snapshot, status_flags) == 60, "loom_service_snapshot.status_flags");

#endif
