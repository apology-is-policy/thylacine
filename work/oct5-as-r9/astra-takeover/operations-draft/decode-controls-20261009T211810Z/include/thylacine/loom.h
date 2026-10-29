#include <thylacine/loom_service_abi.h>
struct loom_sqe {
    u8  opcode;          // LOOM_OP_*
    u8  flags;           // LOOM_SQE_* (LINK / DRAIN / CQE_SKIP / MULTISHOT)
    u16 _resv0;
    u32 handle_idx;      // registered-handle index, or LOOM_HANDLE_RAW
    u64 offset;          // op offset (read / write / readdir)
    u32 len;             // op length / count
    u32 buf_idx_or_off;  // registered-buffer index or in-ring offset (Loom-6)
    u64 user_data;       // correlation token (echoed verbatim in the CQE)
    u64 _resv1[4];       // opcode-specific / future
};
_Static_assert(sizeof(struct loom_sqe)==64,"actual extracted SQE");
enum {
    LOOM_OP_NOP        = 0,    // no-op; completes with result 0 (Loom-3 smoke)
    LOOM_OP_WALK       = 1,
    LOOM_OP_LOPEN      = 2,
    LOOM_OP_LCREATE    = 3,
    LOOM_OP_READ       = 4,
    LOOM_OP_WRITE      = 5,
    LOOM_OP_GETATTR    = 6,
    LOOM_OP_SETATTR    = 7,
    LOOM_OP_READDIR    = 8,
    LOOM_OP_FSYNC      = 9,
    LOOM_OP_CLUNK      = 10,
    LOOM_OP_RENAMEAT   = 11,
    LOOM_OP_UNLINKAT   = 12,
    LOOM_OP_MKDIR      = 13,
    LOOM_OP_SYMLINK    = 14,
    LOOM_OP_LINK       = 15,
    LOOM_OP_MKNOD      = 16,
    LOOM_OP_READLINK   = 17,
    LOOM_OP_STATFS     = 18,
    LOOM_OP_WIRE_PASSTHROUGH = 19,  // RESERVED -- not implemented at v1.0
    LOOM_OP_COUNT      = 20,
};
#define LOOM_MAX_REG_BUFFERS    64u
#define LOOM_SQE_LINK       (1u << 0)   // the next SQE runs after this one
#define LOOM_SQE_DRAIN      (1u << 1)   // wait for all prior ops before this one
#define LOOM_SQE_CQE_SKIP   (1u << 2)   // suppress this op's CQE on success
#define LOOM_SQE_MULTISHOT  (1u << 3)   // one SQE -> many CQEs (accept loop)
