// Real request/slot/node ledgers. Scope state transitions and CQ capacity are
// simulated here; owner/native integration separately proves allocation/posting.
#ifndef LOOM_REQUESTS_FIXTURE_H
#define LOOM_REQUESTS_FIXTURE_H
#include <thylacine/loom_service_requests.h>
#include <thylacine/errno.h>
static const char *loom_requests_fixture(void) {
    static struct loom_slot_table s;
    static struct loom_service_node_table n;
    static struct loom_service_request records[2];
    for (size_t i=0;i<sizeof(s);++i) ((u8 *)&s)[i]=0;
    for (size_t i=0;i<sizeof(n);++i) ((u8 *)&n)[i]=0;
    for (size_t i=0;i<sizeof(records);++i) ((u8 *)records)[i]=0;
    struct loom_request_table t={.records=records,.capacity=2,.admit=2};
    struct loom_service_ref scope,zero={0};
    struct loom_request_ref a,b,stale,out={.index=999,.identity=999};
    u64 data=999; s32 result=999;
#define RQ(c,m) do {if (!(c)) return m;} while (0)
    RQ(loom_requests_empty(&t),"empty request storage");
    RQ(!loom_service_node_prepare(&s,&n,LOOM_SERVICE_SCOPE,zero,&scope) &&
       !loom_service_node_publish(&s,&n,scope),"scope allocated");
    RQ(loom_request_prepare(&t,&s,&n,scope,7,10,10,&out)==-T_E_TIMEDOUT &&
       !t.occupied && !t.next_identity && !s.slots[scope.slot].pins,
       "expired request reserves nothing");
    RQ(!loom_request_prepare(&t,&s,&n,scope,7,20,10,&a) && t.occupied==1 &&
       s.slots[scope.slot].pins==1,"prepare reserves terminal and parent");
    RQ(loom_service_node_reap_unused(&s,&n,scope)==-T_E_BUSY,"request prevents scope reuse");
    RQ(loom_request_admit(&t,&s,&n,a,20)==-T_E_TIMEDOUT && records[a.index].phase==LREQ_PREPARED,
       "admission rechecks absolute deadline");
    stale=a;
    RQ(!loom_request_rollback(&t,&s,a) && !t.occupied && !s.slots[scope.slot].pins,
       "rollback releases capacity and pin");
    RQ(!loom_request_prepare(&t,&s,&n,scope,7,0,30,&a) && a.identity>stale.identity &&
       a.index==stale.index,"rollback burns request identity");
    RQ(loom_request_admit(&t,&s,&n,stale,30)==-T_E_NOENT &&
       loom_request_rollback(&t,&s,stale)==-T_E_NOENT,"stale request cannot mutate replacement");
    RQ(!loom_request_admit(&t,&s,&n,a,30) && !loom_request_borrow(&t,&s,&n,a),"admitted borrower");
    RQ(loom_request_finish(&t,&s,&n,a,5)==-T_E_BUSY &&
       loom_request_reap(&t,&s,a)==-T_E_BUSY,"live borrow blocks terminal and free");
    RQ(!loom_request_put(&t,a) && loom_request_put(&t,a)==-T_E_INVAL,"put exactly once");
    RQ(!loom_request_finish(&t,&s,&n,a,5),"terminal success commits");
    RQ(loom_request_finish(&t,&s,&n,a,9)==-T_E_BUSY,"duplicate terminal refused");
    // Same user_data is legal correlation, never the request identity.
    RQ(!loom_request_prepare(&t,&s,&n,scope,7,0,30,&b) && b.identity!=a.identity &&
       !loom_request_admit(&t,&s,&n,b,30),"same correlation gets distinct request identity");
    RQ(loom_request_prepare(&t,&s,&n,scope,8,0,30,&out)==-T_E_NOSPC &&
       out.index==999 && out.identity==999 && t.occupied==2 && s.slots[scope.slot].pins==2,
       "full terminal table refuses without leaks");
    n.nodes[scope.slot].state=LOOM_SERVICE_ABORTING;
    RQ(loom_request_finish(&t,&s,&n,b,0)==-T_E_CANCELED && records[b.index].phase==LREQ_ACTIVE,
       "abort forbids fresh success");
    RQ(loom_request_borrow(&t,&s,&n,b)==-T_E_CANCELED,"abort forbids new local borrow");
    RQ(!loom_request_finish(&t,&s,&n,b,-T_E_CANCELED),"abort terminal commits after borrows");
    RQ(!loom_request_peek(&t,a,&data,&result) && data==7 && result==5,
       "preabort committed success preserved");
    RQ(loom_request_reap(&t,&s,a)==-T_E_BUSY &&
       loom_request_discard(&t,a)==-T_E_BUSY,"undelivered terminal cannot be discarded or freed");
    // CQ stays full: repeated peek neither consumes nor rewrites a terminal.
    RQ(!loom_request_peek(&t,a,&data,&result) && t.occupied==2 && result==5,
       "backpressure retains terminal obligation");
    RQ(!loom_request_deliver(&t,a) && loom_request_deliver(&t,a)==-T_E_BUSY &&
       !loom_request_reap(&t,&s,a) && t.occupied==1 && s.slots[scope.slot].pins==1,
       "delivery then reap releases one parent pin");
    RQ(loom_requests_detach(&t,&s)==-T_E_BUSY,"live consumer cannot detach");
    loom_slots_close(&s);
    RQ(!loom_requests_detach(&t,&s) && loom_request_deliver(&t,b)==-T_E_CANCELED &&
       !loom_request_discard(&t,b) && !loom_request_reap(&t,&s,b) &&
       loom_requests_empty(&t) && !s.slots[scope.slot].pins,"detached consumer discards quiescent terminal");
    // Reopen only this test's synthetic ring for wrap and borrow-detach arms.
    s.closing=false;t.detached=false;n.nodes[scope.slot].state=LOOM_SERVICE_READY;
    t.next_identity=UINT64_MAX;
    RQ(loom_request_prepare(&t,&s,&n,scope,9,0,30,&out)==-T_E_NOSPC &&
       !t.occupied && !s.slots[scope.slot].pins,"request identity wrap refused");
    t.next_identity=10;
    RQ(!loom_request_prepare(&t,&s,&n,scope,9,0,30,&a) &&
       !loom_request_admit(&t,&s,&n,a,30) && !loom_request_borrow(&t,&s,&n,a),"close borrower setup");
    loom_slots_close(&s);
    RQ(loom_requests_detach(&t,&s)==-T_E_BUSY && !t.detached,"live borrows prevent consumer detach");
    RQ(!loom_request_put(&t,a) && !loom_request_finish(&t,&s,&n,a,-T_E_CANCELED) &&
       !loom_requests_detach(&t,&s) && !loom_request_discard(&t,a) &&
       !loom_request_reap(&t,&s,a),"close cleanup requires no peer");
    RQ(loom_request_prepare(&t,&s,&n,scope,0,0,30,&out)==-T_E_CANCELED,
       "close prevents request reservation");
    n.nodes[scope.slot].state=LOOM_SERVICE_RESERVED;
    s.closing=false;t.detached=false;
    RQ(!loom_request_prepare(&t,&s,&n,scope,7,0,30,&a),"rejected admission fixture setup");
    RQ(loom_request_reject(&t,&s,stale,-T_E_IO)==-T_E_NOENT &&
       loom_request_reject(&t,&s,a,0)==-T_E_INVAL && records[a.index].phase==LREQ_PREPARED,
       "rejected admission validates identity and negative result");
    records[a.index].borrows=1;
    RQ(loom_request_reject(&t,&s,a,-T_E_IO)==-T_E_BUSY && records[a.index].phase==LREQ_PREPARED,
       "rejected admission waits for local borrow");
    records[a.index].borrows=0;
    s.slots[scope.slot].pins=0;
    RQ(loom_request_reject(&t,&s,a,-T_E_IO)==-T_E_INVAL && records[a.index].phase==LREQ_PREPARED,
       "rejected admission retains exact scope pin");
    s.slots[scope.slot].pins=1;
    RQ(!loom_request_reject(&t,&s,a,-T_E_IO) && records[a.index].phase==LREQ_TERMINAL &&
       records[a.index].result==-T_E_IO && t.occupied==1 && s.slots[scope.slot].pins==1,
       "rejected admission reserves terminal until delivery");
    RQ(loom_request_reject(&t,&s,a,-T_E_CANCELED)==-T_E_BUSY && records[a.index].result==-T_E_IO,
       "rejected terminal is immutable");
    RQ(!loom_request_deliver(&t,a) && !loom_request_reap(&t,&s,a),"rejected terminal cleanup");
    RQ(!loom_service_node_reap_unused(&s,&n,scope) && loom_requests_empty(&t),"parent cleanup after all requests");
    // One admitted request at a time, as a book of one CQ slot allows, on a
    // table sized like an owner's: the delivered records stop counting.
    static struct loom_service_request wide[2u+LOOM_SERVICE_SLOT_COUNT];
    for (size_t i=0;i<sizeof(wide);++i) ((u8 *)wide)[i]=0;
    struct loom_request_table w={.records=wide,.capacity=loom_request_records(1),.admit=1};
    struct loom_request_ref held[2u+LOOM_SERVICE_SLOT_COUNT];
    RQ(sizeof(wide)/sizeof(wide[0])==w.capacity,"an owner's table holds admit twice and a slot's worth");
    RQ(!loom_service_node_prepare(&s,&n,LOOM_SERVICE_SCOPE,zero,&scope) &&
       !loom_service_node_publish(&s,&n,scope),"second scope allocated");
    RQ(!loom_request_prepare(&w,&s,&n,scope,1,0,30,&held[0]) &&
       loom_request_prepare(&w,&s,&n,scope,1,0,30,&out)==-T_E_NOSPC && w.occupied==1,
       "admission counts the requests awaiting a terminal");
    u32 made=0;
    for (;;) {
        struct loom_request_ref r=held[made];
        RQ(!loom_request_admit(&w,&s,&n,r,30) && !loom_request_finish(&w,&s,&n,r,0),
           "a request reaches its terminal");
        if (!loom_request_deliver_room(&w)) break;
        RQ(!loom_request_deliver(&w,r) && w.delivered==made+1u,"delivery counts the record");
        ++made;
        RQ(made<w.capacity && !loom_request_prepare(&w,&s,&n,scope,1,0,30,&held[made]) &&
           w.occupied==made+1u,"a delivered record no longer counts against admission");
    }
    RQ(made==w.admit+LOOM_SERVICE_SLOT_COUNT && w.occupied==made+1u && w.occupied<=w.capacity,
       "the delivered bound leaves a record for every admitted request");
    RQ(loom_request_deliver(&w,held[made])==-T_E_AGAIN && w.delivered==made &&
       wide[held[made].index].phase==LREQ_TERMINAL,"delivery waits at the delivered bound");
    RQ(!loom_request_reap(&w,&s,held[0]) && w.delivered==made-1u && loom_request_deliver_room(&w) &&
       !loom_request_deliver(&w,held[made]) && w.delivered==made,"a reaped record lets delivery resume");
    for (u32 i=1;i<=made;++i) RQ(!loom_request_reap(&w,&s,held[i]),"every delivered record reaps");
    RQ(loom_requests_empty(&w) && !w.delivered && !loom_service_node_reap_unused(&s,&n,scope),
       "the owner-sized table drains");
#undef RQ
    return NULL;
}
#endif
