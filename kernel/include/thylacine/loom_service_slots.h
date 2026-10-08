// Internal shared 64-slot identity ledger. ASYNC-SERVICE-ABI.md owns policy.
#ifndef THYLACINE_LOOM_SERVICE_SLOTS_H
#define THYLACINE_LOOM_SERVICE_SLOTS_H
#include <thylacine/loom_service_abi.h>

enum loom_slot_phase { LSLOT_FREE, LSLOT_PROVISIONAL, LSLOT_LIVE, LSLOT_RETIRING };
struct loom_service_slot {
    u64 incarnation;
    u32 kind, phase, pins;
};
struct loom_slot_table {
    u64 next_incarnation;
    struct loom_service_slot slots[LOOM_SERVICE_SLOT_COUNT];
    bool closing;
};
_Static_assert(sizeof(struct loom_service_slot) == 24, "service slot identity ledger");
_Static_assert(sizeof(struct loom_slot_table) == 1552, "shared slot table ledger");
// Zero once with its owning ring. Every call holds that ring's lock; usercopy
// may run unlocked between reserve and publish/rollback calls, while
// PROVISIONAL consumes the slot. No allocation/callbacks. These phases are
// internal bookkeeping, not the protocol states exposed by QUERY.
// This ledger owns identities/borrows only. The engine still owes native target
// authority, parent-scope pins, terminal/local retirement and payload retention
// checks BEFORE retiring/reaping records, and resource destruction after unlock.
// It is not yet attached to Loom, charged, or reachable from a syscall.
int loom_slot_reserve(struct loom_slot_table *, u32 kind, struct loom_service_ref *);
int loom_slot_publish(struct loom_slot_table *, struct loom_service_ref);
int loom_slot_rollback(struct loom_slot_table *, struct loom_service_ref);
// Pin only a published, live record of the requested kind; caller must unpin
// exactly once. Pins held before close/retire may be released afterwards.
int loom_slot_pin(struct loom_slot_table *, struct loom_service_ref, u32 kind);
int loom_slot_unpin(struct loom_slot_table *, struct loom_service_ref);
// retire prevents new borrows, reap requires zero pins, then reuse burns a NEW
// incarnation. Provisional rollback also burns its incarnation, never rewinds.
int loom_slot_retire(struct loom_slot_table *, struct loom_service_ref);
int loom_slot_reap(struct loom_slot_table *, struct loom_service_ref);
// Freeze reserve/publish/pin. Does not discard records or existing borrows.
void loom_slots_close(struct loom_slot_table *);
#endif
