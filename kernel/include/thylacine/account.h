// The memory account ABI, docs/SHARED-MEMORY-ACCOUNTING.md section 13. Reserved at
// MM-0: no kernel code is behind it, and SYS_ACCOUNT answers as an unknown number until
// MM-1 brings the accounts. tools/test-account-abi.py holds this header, libt's and
// libthyla-rs's to one frozen byte record.
#ifndef THYLACINE_ACCOUNT_H
#define THYLACINE_ACCOUNT_H

#include <thylacine/types.h>

#define ACCOUNT_VERSION 1u

// In place of a handle, the caller's own account. No handle is negative.
#define ACCOUNT_SELF (-1)

enum {
    ACCOUNT_QUERY = 1,
    ACCOUNT_CHILD = 2,
    ACCOUNT_SET_LIMITS = 3,
    VOUCHER_CREATE = 4,
    VOUCHER_CLOSE = 5,
    ALLOCATE_FUNDED = 6,
};

enum {
    CLASS_ALLOC = 0,
    CLASS_RETAIN = 1,
    CLASS_META = 2,
    CLASS_DEVICE = 3,
    ACCOUNT_CLASSES = 4,
};

// Section 8's pressure levels.
enum {
    ACCOUNT_NORMAL = 0,
    ACCOUNT_WARNING = 1,
    ACCOUNT_CRITICAL = 2,
};

// Section 9's outcomes. Zero is no value, so a record never filled in names no reason.
enum {
    REASON_ACCOUNT_LIMIT = 1,
    REASON_RETENTION_LIMIT = 2,
    REASON_PHYSICAL_PRESSURE = 3,
    REASON_CONTIGUOUS_UNAVAILABLE = 4,
    REASON_DEVICE_BUDGET = 5,
    REASON_METADATA_LIMIT = 6,
    REASON_UNSUPPORTED_LAYOUT = 7,
    REASON_PERMISSION_DENIED = 8,
    REASON_STALE_GRANT = 9,
};

// flags and pad are zero. incarnation is the expected account or object, 0 for self.
struct account_rec_hdr {
    u32 version;
    u32 size;
    u32 flags;
    u32 pad;
    u64 incarnation;
};

struct account_class {
    u64 used;
    u64 reserved;
    u64 limit;
};

struct account_query {
    struct account_rec_hdr h;
    u64 generation;
    u32 level;
    u32 limiting;
    u64 headroom;
    u64 reclaim_target;
    u64 retained_after_exit;
    u64 awaiting_device;
    struct account_class c[ACCOUNT_CLASSES];
};

struct account_outcome {
    u64 request_id;
    u32 cls;
    u32 reason;
    u64 requested;
    u64 available;
    u64 limiting_account;
};

_Static_assert(sizeof(struct account_rec_hdr) == 24, "account_rec_hdr ABI: 24 bytes");
_Static_assert(__builtin_offsetof(struct account_rec_hdr, incarnation) == 16,
               "account_rec_hdr ABI: incarnation@16");
_Static_assert(sizeof(struct account_class) == 24, "account_class ABI: 24 bytes");
_Static_assert(sizeof(struct account_query) == 168, "account_query ABI: 168 bytes");
_Static_assert(__builtin_offsetof(struct account_query, generation) == 24,
               "account_query ABI: generation@24");
_Static_assert(__builtin_offsetof(struct account_query, level) == 32,
               "account_query ABI: level@32");
_Static_assert(__builtin_offsetof(struct account_query, headroom) == 40,
               "account_query ABI: headroom@40");
_Static_assert(__builtin_offsetof(struct account_query, awaiting_device) == 64,
               "account_query ABI: awaiting_device@64");
_Static_assert(__builtin_offsetof(struct account_query, c) == 72, "account_query ABI: c@72");
_Static_assert(sizeof(struct account_outcome) == 40, "account_outcome ABI: 40 bytes");
_Static_assert(__builtin_offsetof(struct account_outcome, reason) == 12,
               "account_outcome ABI: reason@12");
_Static_assert(__builtin_offsetof(struct account_outcome, limiting_account) == 32,
               "account_outcome ABI: limiting_account@32");

#endif
