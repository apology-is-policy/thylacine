// Shared private-service slot identity/admission ledger; see the header contract.
// Under the owning ring lock, FREE -> PROVISIONAL burns a ring-global identity.
// Copyout failure releases capacity but never rewinds that counter. Exact refs
// plus a LIVE/kind check precede borrowing, and RETIRING prevents new borrows.
// Reap can free an identity only after its final borrower leaves; the caller
// separately proves protocol terminality, local retirement and payload release.
// Close serializes against reserve/publish/pin, while rollback/unpin/reap still
// progress without a peer response. No pointer escapes and no callback runs.
// Mirrors the identity/borrow subset of specs/loom_service.tla, not its complete
// request/terminal/accounting machine. Private owner storage is charged, while
// service handlers and protocol admission remain disabled.
#include <thylacine/loom_service_slots.h>
#include <thylacine/errno.h>

static bool slot_kind(u32 kind) {
    return kind == LOOM_SERVICE_TARGET || kind == LOOM_SERVICE_SCOPE ||
           kind == LOOM_SERVICE_FID || kind == LOOM_SERVICE_POOL;
}
static struct loom_service_slot *slot_exact(struct loom_slot_table *t,
                                            struct loom_service_ref ref) {
    if (!t || ref.reserved || !ref.incarnation || ref.slot >= LOOM_SERVICE_SLOT_COUNT)
        return NULL;
    struct loom_service_slot *s = &t->slots[ref.slot];
    if (s->phase == LSLOT_FREE || s->incarnation != ref.incarnation) return NULL;
    return s;
}
int loom_slot_reserve(struct loom_slot_table *t, u32 kind, struct loom_service_ref *out) {
    if (!t || !out || !slot_kind(kind)) return -T_E_INVAL;
    if (t->closing) return -T_E_CANCELED;
    
    for (u32 i = 0; i < LOOM_SERVICE_SLOT_COUNT; ++i) {
        struct loom_service_slot *s = &t->slots[i];
        if (s->phase != LSLOT_FREE) continue;
        *s = (struct loom_service_slot){ .incarnation = ++t->next_incarnation,
            .kind = kind, .phase = LSLOT_PROVISIONAL };
        *out = (struct loom_service_ref){ .slot = i, .incarnation = s->incarnation };
        return 0;
    }
    return -T_E_NOSPC;
}
int loom_slot_publish(struct loom_slot_table *t, struct loom_service_ref ref) {
    struct loom_service_slot *s = slot_exact(t, ref);
    if (!s) return -T_E_NOENT;
    if (t->closing) return -T_E_CANCELED;
    if (s->phase != LSLOT_PROVISIONAL) return -T_E_BUSY;
    s->phase = LSLOT_LIVE;
    return 0;
}
int loom_slot_rollback(struct loom_slot_table *t, struct loom_service_ref ref) {
    struct loom_service_slot *s = slot_exact(t, ref);
    if (!s) return -T_E_NOENT;
    if (s->phase != LSLOT_PROVISIONAL || s->pins) return -T_E_BUSY;
    *s = (struct loom_service_slot){0};
    return 0;
}
int loom_slot_pin(struct loom_slot_table *t, struct loom_service_ref ref, u32 kind) {
    struct loom_service_slot *s = slot_exact(t, ref);
    if (!s) return -T_E_NOENT;
    if (t->closing) return -T_E_CANCELED;
    if (s->phase != LSLOT_LIVE || s->kind != kind) return -T_E_NOENT;
    if (s->pins == UINT32_MAX) return -T_E_NOSPC;
    ++s->pins;
    return 0;
}
int loom_slot_unpin(struct loom_slot_table *t, struct loom_service_ref ref) {
    struct loom_service_slot *s = slot_exact(t, ref);
    if (!s) return -T_E_NOENT;
    if ((s->phase != LSLOT_LIVE && s->phase != LSLOT_RETIRING) || !s->pins)
        return -T_E_INVAL;
    --s->pins;
    return 0;
}
int loom_slot_retire(struct loom_slot_table *t, struct loom_service_ref ref) {
    struct loom_service_slot *s = slot_exact(t, ref);
    if (!s) return -T_E_NOENT;
    if (s->phase != LSLOT_LIVE && s->phase != LSLOT_RETIRING) return -T_E_BUSY;
    s->phase = LSLOT_RETIRING;
    return 0;
}
int loom_slot_reap(struct loom_slot_table *t, struct loom_service_ref ref) {
    struct loom_service_slot *s = slot_exact(t, ref);
    if (!s) return -T_E_NOENT;
    if (s->phase != LSLOT_RETIRING || s->pins) return -T_E_BUSY;
    *s = (struct loom_service_slot){0};
    return 0;
}
void loom_slots_close(struct loom_slot_table *t) { if (t) t->closing = true; }
