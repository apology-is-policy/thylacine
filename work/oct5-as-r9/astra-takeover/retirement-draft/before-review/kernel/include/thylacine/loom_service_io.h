#ifndef THYLACINE_LOOM_SERVICE_IO_H
#define THYLACINE_LOOM_SERVICE_IO_H
#include <thylacine/loom_service_admission.h>
struct Proc;
struct loom_sqe;
// Immutable kernel SQE snapshot, retained private Loom and its actual creator
// Proc in a stable syscall image (not a worker borrowing a Proc). No wire I/O.
// Prepares exact storage/identity, fixed slice or copied component, and program.
// Failed output untouched; all provisional resources physically released.
// Caller must publish with a FRESH time or release the exclusive prepared owner.
// CONNECT and provided-buffer forms are owned by separate frontends.
int loom_private_io_prepare(struct Loom *,struct Proc *,const struct loom_sqe *,
                            u64 now,struct loom_service_operation **);
// Success transfers ownership to the indexed engine and wakes it. Do not use
// the operation afterward; it may already be consumed. Refusal retains caller
// ownership for release_prepared. No user/CQ dispatch is enabled by these APIs.
int loom_private_io_publish(struct loom_service_operation *,u64 now);
int loom_private_io_release_prepared(struct loom_service_operation *);
// Sole driver, only SETTLING after real local parser/TX users end; or exclusive
// NEW preparation cleanup. Release actual backing outside Ll before any terminal.
int loom_private_io_drop_fixed(struct loom_service_operation *);
#endif
