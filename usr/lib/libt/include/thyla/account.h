// The memory account ABI, docs/SHARED-MEMORY-ACCOUNTING.md section 13: the mirror of the
// kernel's thylacine/account.h, held to one byte record by tools/test-account-abi.py.
#ifndef THYLA_ACCOUNT_H
#define THYLA_ACCOUNT_H

#define T_ACCOUNT_VERSION 1u

// In place of a handle, the caller's own account. No handle is negative.
#define T_ACCOUNT_SELF (-1)

enum {
    T_ACCOUNT_QUERY = 1,
    T_ACCOUNT_CHILD = 2,
    T_ACCOUNT_SET_LIMITS = 3,
    T_VOUCHER_CREATE = 4,
    T_VOUCHER_CLOSE = 5,
    T_ALLOCATE_FUNDED = 6,
};

enum {
    T_CLASS_ALLOC = 0,
    T_CLASS_RETAIN = 1,
    T_CLASS_META = 2,
    T_CLASS_DEVICE = 3,
    T_ACCOUNT_CLASSES = 4,
};

enum {
    T_ACCOUNT_NORMAL = 0,
    T_ACCOUNT_WARNING = 1,
    T_ACCOUNT_CRITICAL = 2,
};

enum {
    T_REASON_ACCOUNT_LIMIT = 1,
    T_REASON_RETENTION_LIMIT = 2,
    T_REASON_PHYSICAL_PRESSURE = 3,
    T_REASON_CONTIGUOUS_UNAVAILABLE = 4,
    T_REASON_DEVICE_BUDGET = 5,
    T_REASON_METADATA_LIMIT = 6,
    T_REASON_UNSUPPORTED_LAYOUT = 7,
    T_REASON_PERMISSION_DENIED = 8,
    T_REASON_STALE_GRANT = 9,
};

struct t_account_rec_hdr {
    unsigned int version;
    unsigned int size;
    unsigned int flags;
    unsigned int pad;
    unsigned long long incarnation;
};

struct t_account_class {
    unsigned long long used;
    unsigned long long reserved;
    unsigned long long limit;
};

struct t_account_query {
    struct t_account_rec_hdr h;
    unsigned long long generation;
    unsigned int level;
    unsigned int limiting;
    unsigned long long headroom;
    unsigned long long reclaim_target;
    unsigned long long retained_after_exit;
    unsigned long long awaiting_device;
    struct t_account_class c[T_ACCOUNT_CLASSES];
};

struct t_account_outcome {
    unsigned long long request_id;
    unsigned int cls;
    unsigned int reason;
    unsigned long long requested;
    unsigned long long available;
    unsigned long long limiting_account;
};

_Static_assert(sizeof(struct t_account_rec_hdr) == 24, "t_account_rec_hdr ABI: 24 bytes");
_Static_assert(sizeof(struct t_account_class) == 24, "t_account_class ABI: 24 bytes");
_Static_assert(sizeof(struct t_account_query) == 168, "t_account_query ABI: 168 bytes");
_Static_assert(__builtin_offsetof(struct t_account_query, c) == 72, "t_account_query ABI: c@72");
_Static_assert(sizeof(struct t_account_outcome) == 40, "t_account_outcome ABI: 40 bytes");

#endif
