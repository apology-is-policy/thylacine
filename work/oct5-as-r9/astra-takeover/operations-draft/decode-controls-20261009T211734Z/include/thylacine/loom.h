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
#define LOOM_MAX_REG_BUFFERS    64u
#define LOOM_SQE_LINK       (1u << 0)   // the next SQE runs after this one
#define LOOM_SQE_DRAIN      (1u << 1)   // wait for all prior ops before this one
#define LOOM_SQE_CQE_SKIP   (1u << 2)   // suppress this op's CQE on success
#define LOOM_SQE_MULTISHOT  (1u << 3)   // one SQE -> many CQEs (accept loop)
