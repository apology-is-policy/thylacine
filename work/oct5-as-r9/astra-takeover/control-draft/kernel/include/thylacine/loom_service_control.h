// Internal creator-authorized controls over copied ABI records. No syscall
// dispatch/private activation. Caller retains Loom and a stable creator image;
// no user pointer or Proc escapes. Query writes output only on success.
#ifndef THYLACINE_LOOM_SERVICE_CONTROL_H
#define THYLACINE_LOOM_SERVICE_CONTROL_H
#include <thylacine/loom_service_abi.h>
struct Loom;
struct Proc;
int loom_private_service_query(struct Loom *, struct Proc *,
    const struct loom_service_snapshot *, struct loom_service_snapshot *);
int loom_private_service_abort(struct Loom *, struct Proc *,
    const struct loom_service_control *);
int loom_private_service_reap(struct Loom *, struct Proc *,
    const struct loom_service_control *);
#endif
