// Private ring target storage, charged once with the owner; no user ABI.
#ifndef THYLACINE_LOOM_SERVICE_TARGETS_H
#define THYLACINE_LOOM_SERVICE_TARGETS_H
#include <thylacine/devsrv.h>
#include <thylacine/loom_service_abi.h>
struct loom_service_target_table {
    struct SrvServiceTarget targets[LOOM_SERVICE_SLOT_COUNT];
};
_Static_assert(sizeof(struct SrvServiceTarget) == 64, "native target storage ledger");
_Static_assert(sizeof(struct loom_service_target_table) == 4096, "target table ledger");
#endif
