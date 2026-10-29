// Shared native/host fixture, real slot ledger; no allocator or lock doubles.
#ifndef LOOM_SLOTS_FIXTURE_H
#define LOOM_SLOTS_FIXTURE_H
#include <thylacine/loom_service_slots.h>
#include <thylacine/errno.h>
#define LS_CHECK(c, msg) do { if (!(c)) return msg; } while (0)
// Match the existing freestanding fixture convention: a large aggregate
// assignment lowers to an unavailable libc memset, even with -fno-builtin.
static void ls_zero(void *p, size_t n) {
    for (size_t i = 0; i < n; ++i) ((u8 *)p)[i] = 0;
}
static const char *loom_slots_fixture(void) {
    // Static to avoid spending 1552 bytes of the kernel test thread's stack.
    static struct loom_slot_table t;
    ls_zero(&t, sizeof(t));
    static struct loom_service_ref refs[LOOM_SERVICE_SLOT_COUNT];
    struct loom_service_ref a, b;
    LS_CHECK(loom_slot_reserve(&t, 0, &a) == -T_E_INVAL && !t.next_incarnation,
             "invalid kind does not consume an identity");
    LS_CHECK(!loom_slot_reserve(&t, LOOM_SERVICE_TARGET, &a), "target reserved");
    LS_CHECK(loom_slot_pin(&t, a, LOOM_SERVICE_TARGET) == -T_E_NOENT,
             "provisional target is not usable");
    LS_CHECK(!loom_slot_reserve(&t, LOOM_SERVICE_SCOPE, &b) && a.slot != b.slot,
             "provisional slot consumes shared capacity");
    LS_CHECK(!loom_slot_rollback(&t, a) && !loom_slot_rollback(&t, b), "rollback releases capacity");
    LS_CHECK(!loom_slot_reserve(&t, LOOM_SERVICE_FID, &b) && b.slot == a.slot &&
             b.incarnation > a.incarnation, "rollback identity never repeats");
    LS_CHECK(!loom_slot_publish(&t, b), "fid publication");
    LS_CHECK(loom_slot_pin(&t, a, LOOM_SERVICE_FID) == -T_E_NOENT,
             "stale incarnation cannot borrow reused slot");
    a = b; a.reserved = 1;
    LS_CHECK(loom_slot_pin(&t, a, LOOM_SERVICE_FID) == -T_E_NOENT, "reserved reference refused");
    a = b; a.slot = LOOM_SERVICE_SLOT_COUNT;
    LS_CHECK(loom_slot_pin(&t, a, LOOM_SERVICE_FID) == -T_E_NOENT, "slot range checked");
    LS_CHECK(loom_slot_pin(&t, b, LOOM_SERVICE_SCOPE) == -T_E_NOENT, "wrong kind cannot borrow");
    LS_CHECK(loom_slot_reap(&t, b) == -T_E_BUSY, "live slot cannot be reaped");
    LS_CHECK(loom_slot_rollback(&t, b) == -T_E_BUSY, "published slot cannot roll back");
    LS_CHECK(!loom_slot_pin(&t, b, LOOM_SERVICE_FID) && !loom_slot_retire(&t, b), "borrow retained at retirement");
    LS_CHECK(loom_slot_pin(&t, b, LOOM_SERVICE_FID) == -T_E_NOENT, "retirement stops new borrows");
    LS_CHECK(loom_slot_reap(&t, b) == -T_E_BUSY, "borrow prevents early reuse");
    LS_CHECK(!loom_slot_unpin(&t, b) && !loom_slot_reap(&t, b), "last borrow permits reap");
    LS_CHECK(loom_slot_unpin(&t, b) == -T_E_NOENT, "reaped reference cannot unpin");
    for (u32 i = 0; i < LOOM_SERVICE_SLOT_COUNT; ++i) {
        LS_CHECK(!loom_slot_reserve(&t, 1u + i % 4u, &refs[i]), "mixed table reservation");
        LS_CHECK(!loom_slot_publish(&t, refs[i]), "mixed table publication");
    }
    u64 before = t.next_incarnation;
    a = (struct loom_service_ref){.slot=999,.incarnation=777};
    LS_CHECK(loom_slot_reserve(&t, LOOM_SERVICE_POOL, &a) == -T_E_NOSPC &&
             t.next_incarnation == before && a.slot == 999 && a.incarnation == 777,
             "all four kinds share the exact 64 slot bound");
    for (u32 i = 0; i < LOOM_SERVICE_SLOT_COUNT; ++i)
        LS_CHECK(!loom_slot_retire(&t, refs[i]) && !loom_slot_reap(&t, refs[i]), "mixed table reap");
    t.next_incarnation = UINT64_MAX - 1;
    LS_CHECK(!loom_slot_reserve(&t, LOOM_SERVICE_TARGET, &a) && a.incarnation == UINT64_MAX,
             "last nonzero incarnation usable");
    LS_CHECK(!loom_slot_rollback(&t, a), "last incarnation rollback");
    LS_CHECK(loom_slot_reserve(&t, LOOM_SERVICE_TARGET, &a) == -T_E_NOSPC &&
             t.next_incarnation == UINT64_MAX, "incarnation exhaustion never wraps");
    // New table = different owning ring, never a reset of a live ring's ledger.
    ls_zero(&t, sizeof(t));
    LS_CHECK(!loom_slot_reserve(&t, LOOM_SERVICE_SCOPE, &a) && !loom_slot_publish(&t, a), "close control live slot");
    LS_CHECK(!loom_slot_reserve(&t, LOOM_SERVICE_POOL, &b), "close control pending copyout");
    t.slots[a.slot].pins = UINT32_MAX;
    LS_CHECK(loom_slot_pin(&t, a, LOOM_SERVICE_SCOPE) == -T_E_NOSPC &&
             t.slots[a.slot].pins == UINT32_MAX, "borrow saturation never wraps");
    t.slots[a.slot].pins = 0;
    LS_CHECK(loom_slot_unpin(&t, a) == -T_E_INVAL, "borrow underflow refused");
    LS_CHECK(!loom_slot_pin(&t, a, LOOM_SERVICE_SCOPE), "preclose borrow");
    loom_slots_close(&t);
    LS_CHECK(loom_slot_reserve(&t, LOOM_SERVICE_FID, &refs[0]) == -T_E_CANCELED,
             "close freezes reservations");
    LS_CHECK(loom_slot_publish(&t, b) == -T_E_CANCELED, "close wins against output publication");
    LS_CHECK(loom_slot_pin(&t, a, LOOM_SERVICE_SCOPE) == -T_E_CANCELED, "close freezes borrows");
    LS_CHECK(!loom_slot_rollback(&t, b), "close still permits rollback");
    LS_CHECK(!loom_slot_retire(&t, a) && loom_slot_reap(&t, a) == -T_E_BUSY,
             "close preserves preexisting borrows");
    LS_CHECK(!loom_slot_unpin(&t, a) && !loom_slot_reap(&t, a), "close permits local retirement");
    return NULL;
}
#undef LS_CHECK
#endif
