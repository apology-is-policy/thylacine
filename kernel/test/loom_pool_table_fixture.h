// Same real transaction modules on host and guest; caller lock/pins excluded.
#ifndef LOOM_POOL_TABLE_FIXTURE_H
#define LOOM_POOL_TABLE_FIXTURE_H
#include <thylacine/loom_service_pool_table.h>
#include <thylacine/errno.h>
#define PT_CHECK(c,msg) do { if (!(c)) return msg; } while (0)
static const char *loom_pool_table_fixture(void) {
    static struct loom_slot_table slots;
    static struct loom_service_pool_table pools;
    static struct loom_service_ref targets[LOOM_SERVICE_SLOT_COUNT];
    static u8 backing[8192];
    for (size_t i = 0; i < sizeof(slots); ++i) ((u8 *)&slots)[i] = 0;
    for (size_t i = 0; i < sizeof(pools); ++i) ((u8 *)&pools)[i] = 0;
    struct loom_service_ref a, b, c, failed = {.slot=999,.incarnation=999};
    struct loom_pool_extent e = {backing,0,64};
    PT_CHECK(loom_service_pools_empty(&pools), "new pool table empty");
    PT_CHECK(!loom_service_pool_prepare(&slots,&pools,&e,1,&a), "prepare joint reservation");
    PT_CHECK(loom_slot_pin(&slots,a,LOOM_SERVICE_POOL)==-T_E_NOENT &&
             loom_pool_conflicts(&pools.bank,e), "provisional hidden but excludes extent");
    PT_CHECK(loom_service_pool_prepare(&slots,&pools,&e,1,&failed)==-T_E_BUSY &&
             failed.slot==999 && failed.incarnation==999 &&
             slots.slots[1].phase==LSLOT_FREE && pools.pools[1].phase==LPOOL_EMPTY,
             "failed member prepare leaves no hidden slot or output");
    e.offset=128;
    PT_CHECK(!loom_service_pool_prepare(&slots,&pools,&e,1,&b) &&
             !loom_service_pool_publish(&slots,&pools,b), "second pool commits");
    PT_CHECK(!loom_service_pool_rollback(&slots,&pools,a), "copyout rollback");
    e.offset=0;
    PT_CHECK(!loom_pool_conflicts(&pools.bank,e) && slots.slots[a.slot].phase==LSLOT_FREE,
             "rollback releases both exclusions");
    PT_CHECK(loom_service_pool_publish(&slots,&pools,a)==-T_E_NOENT,
             "rolled back output cannot publish");
    PT_CHECK(!loom_service_pool_prepare(&slots,&pools,&e,1,&c) &&
             c.slot==a.slot && c.incarnation!=a.incarnation &&
             !loom_service_pool_publish(&slots,&pools,c), "slot reuse burns new identity");
    PT_CHECK(loom_service_pool_reap(&slots,&pools,a)==-T_E_NOENT,
             "stale pool reference cannot reap replacement");
    a=c;
    for (u32 i=0;i<LOOM_SERVICE_SLOT_COUNT-2;++i) {
        PT_CHECK(!loom_slot_reserve(&slots,LOOM_SERVICE_TARGET,&targets[i]) &&
                 !loom_slot_publish(&slots,targets[i]), "pools share target capacity");
    }
    u64 gen=slots.next_incarnation;
    PT_CHECK(loom_service_pool_prepare(&slots,&pools,&e,1,&failed)==-T_E_NOSPC &&
             slots.next_incarnation==gen, "full shared table refuses before member mutation");
    for (u32 i=0;i<LOOM_SERVICE_SLOT_COUNT-2;++i)
        PT_CHECK(!loom_slot_retire(&slots,targets[i]) && !loom_slot_reap(&slots,targets[i]),
                 "target cleanup");
    PT_CHECK(!loom_slot_pin(&slots,a,LOOM_SERVICE_POOL), "pool metadata borrow");
    PT_CHECK(loom_service_pool_reap(&slots,&pools,a)==-T_E_BUSY &&
             slots.slots[a.slot].phase==LSLOT_LIVE &&
             pools.pools[a.slot].phase==LPOOL_LIVE && loom_pool_conflicts(&pools.bank,e),
             "borrow refusal preserves both ledgers");
    PT_CHECK(!loom_slot_unpin(&slots,a), "pool metadata unpin");
    struct loom_pool *p=&pools.pools[a.slot];
    PT_CHECK(!loom_pool_stream_get(p), "pool stream admitted");
    PT_CHECK(loom_service_pool_reap(&slots,&pools,a)==-T_E_BUSY &&
             slots.slots[a.slot].phase==LSLOT_LIVE && p->phase==LPOOL_LIVE,
             "stream refusal preserves slot");
    struct loom_service_buffer_receipt receipt;
    PT_CHECK(!loom_pool_claim(&pools.bank,p,16,&receipt) &&
             !loom_pool_commit(&pools.bank,p,&receipt,8,123,false) &&
             !loom_pool_stream_put(p), "committed payload outlives stream");
    PT_CHECK(loom_service_pool_reap(&slots,&pools,a)==-T_E_BUSY &&
             slots.slots[a.slot].phase==LSLOT_LIVE, "pending completion retains slot");
    PT_CHECK(!loom_pool_deliver(&pools.bank,p,&receipt), "receipt delivered");
    PT_CHECK(loom_service_pool_reap(&slots,&pools,a)==-T_E_BUSY,
             "delivered payload lease retains slot");
    struct loom_service_buffer_receipt stale=receipt;stale.lease++;
    PT_CHECK(loom_pool_return(&pools.bank,p,&stale)==-T_E_NOENT &&
             loom_service_pool_reap(&slots,&pools,a)==-T_E_BUSY,
             "wrong payload return cannot free slot");
    PT_CHECK(!loom_pool_return(&pools.bank,p,&receipt) &&
             !loom_service_pool_reap(&slots,&pools,a) &&
             slots.slots[a.slot].phase==LSLOT_FREE && !loom_pool_conflicts(&pools.bank,e),
             "exact return permits both ledgers to reap");
    u64 nonce=pools.bank.next_nonce;
    PT_CHECK(nonce && !loom_service_pool_prepare(&slots,&pools,&e,1,&c),
             "new copyout reservation before close");
    loom_slots_close(&slots);
    PT_CHECK(loom_service_pool_publish(&slots,&pools,c)==-T_E_CANCELED &&
             slots.slots[c.slot].phase==LSLOT_PROVISIONAL &&
             pools.pools[c.slot].phase==LPOOL_RESERVED,
             "close rejects joint publication without half commit");
    PT_CHECK(loom_service_pool_prepare(&slots,&pools,&e,1,&failed)==-T_E_CANCELED,
             "close refuses fresh pool reservation");
    PT_CHECK(!loom_service_pool_rollback(&slots,&pools,c) &&
             !loom_service_pool_reap(&slots,&pools,b) && loom_service_pools_empty(&pools) &&
             pools.bank.next_nonce==nonce, "closed ring cleanup retains nonce history");
    return NULL;
}
#undef PT_CHECK
#endif
