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
    struct loom_request_table t={.records=records,.capacity=2};
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
    RQ(!loom_service_node_reap_unused(&s,&n,scope) && loom_requests_empty(&t),"parent cleanup after all requests");
#undef RQ
    return NULL;
}
#endif
