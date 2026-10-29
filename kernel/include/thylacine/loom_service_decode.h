#ifndef THYLACINE_LOOM_SERVICE_DECODE_H
#define THYLACINE_LOOM_SERVICE_DECODE_H
#include <thylacine/types.h>
struct loom_sqe;
// Input is the caller's immutable kernel copy, never a shared SQE. Checks only
// ABI shape: caller still resolves/pins exact creator, scope/fid/destination,
// fixed extent or pool, checks DAC/deadline and reserves terminal ownership.
// No usercopy, allocation, transport access or table mutation.
int loom_private_sqe_shape(const struct loom_sqe *, bool pools_enabled);
// Input is a kernel-owned copy of the already resolved and pinned WALK slice.
// Empty clone has no slice; admission must serialize result with scope abort.
int loom_private_walk_component(const u8 *, u32);
#endif
