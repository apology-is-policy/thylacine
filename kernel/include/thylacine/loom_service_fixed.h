#ifndef THYLACINE_LOOM_SERVICE_FIXED_H
#define THYLACINE_LOOM_SERVICE_FIXED_H
#include <thylacine/loom_service_pool.h>
struct Loom;
// Embedded in a precharged private request, never user-addressable or copied.
// The request owns this node exclusively until release returns. It survives
// cancellation, held dispatch and completion until ALL local readers/writers
// end. A linked node owns an independent Burrow pin and Loom reference.
struct loom_private_fixed {
    struct loom_private_fixed *next;
    struct Loom *owner;
    struct loom_pool_extent extent;
};
_Static_assert(sizeof(struct loom_private_fixed) == 40, "fixed extent borrow ledger");

#endif
