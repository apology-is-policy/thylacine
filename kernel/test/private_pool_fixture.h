// Shared native/host pool fixture. Production C is linked directly. This
// checks transitions and byte retention, not caller locks or Burrow pinning.
#ifndef PRIVATE_POOL_FIXTURE_H
#define PRIVATE_POOL_FIXTURE_H
#include <thylacine/loom_service_pool.h>
#include <thylacine/errno.h>
#define CHECK(x,msg) do { if (!(x)) return msg; } while (0)
#define LP_TRY(x) do { const char *err = (x); if (err) return err; } while (0)
static void lp_set(void *v, int byte, size_t n) {
    for (size_t i=0;i<n;i++) ((u8 *)v)[i]=(u8)byte;
}
static int lp_compare(const void *a,const void *b,size_t n) {
    for(size_t i=0;i<n;i++) if(((const u8 *)a)[i]!=((const u8 *)b)[i]) return 1;
    return 0;
}
static unsigned char lp_backing[8192];
static struct loom_service_ref lp_ref(u32 slot,u64 gen) {
    return (struct loom_service_ref){.slot=slot,.incarnation=gen};
}
static struct loom_pool_extent lp_extent(u64 off,u64 len) {
    return (struct loom_pool_extent){lp_backing,off,len};
}
static struct loom_pool_bank lp_bank;
static struct loom_pool lp_pool;
static void lp_clear(void) { lp_set(&lp_bank,0,sizeof lp_bank); lp_set(&lp_pool,0,sizeof lp_pool); }
static const char *lp_create(u32 n) {
    struct loom_pool_extent e[64];
    for(u32 i=0;i<n;i++) e[i]=lp_extent(i*64,64);
    CHECK(!loom_pool_prepare(&lp_bank,&lp_pool,lp_ref(3,7),e,n),"prepare valid pool");
    CHECK(!loom_pool_publish(&lp_pool),"publish pool");
    CHECK(!loom_pool_stream_get(&lp_pool),"stream ref");
    return NULL;
}
static const char *lp_snapshot(u32 a,u32 b,u32 p,u32 l) {
    struct loom_service_pool_snapshot s;
    lp_set(&s,0xff,sizeof s);
    CHECK(!loom_pool_snapshot(&lp_bank,&lp_pool,&s),"snapshot available");
    CHECK(s.available==a && s.busy==b && s.pending==p && s.leased==l &&
          s.members==a+b+p+l,"snapshot conserves members");
    CHECK(s.size==64 && s.version==1 && !s.flags && !s.reserved[0] && !s.reserved[1],"snapshot zero fields");
    return NULL;
}
static const char *lp_payload(struct loom_service_buffer_receipt r,u8 byte,bool more) {
    lp_set(lp_backing+r.member*64,byte,19);
    CHECK(!loom_pool_commit(&lp_bank,&lp_pool,&r,19,0x123456789abcdef0ULL,more),"payload commit");
    struct loom_pool_result got;
    CHECK(!loom_pool_peek(&lp_bank,&lp_pool,r.member,&got),"peek pending payload");
    CHECK(got.length==19 && got.user_data==0x123456789abcdef0ULL && got.more==more &&
          !lp_compare(&got.receipt,&r,sizeof r),"receipt correlation intact");
    CHECK(loom_pool_return(&lp_bank,&lp_pool,&r)==-T_E_NOENT,"pending cannot return");
    CHECK(loom_pool_release_busy(&lp_bank,&lp_pool,&r)==-T_E_NOENT,"commit cannot be cancelled as BUSY");
    CHECK(!loom_pool_deliver(&lp_bank,&lp_pool,&r),"deliver committed receipt");
    return NULL;
}
static const char *lp_registration(void) {
    lp_clear(); struct loom_pool_extent e[65];
    for(u32 i=0;i<65;i++)e[i]=lp_extent(i*64,64);
    CHECK(loom_pool_prepare(&lp_bank,&lp_pool,lp_ref(0,1),e,0)==-T_E_INVAL,"zero members rejected");
    CHECK(loom_pool_prepare(&lp_bank,&lp_pool,lp_ref(0,1),e,65)==-T_E_INVAL,"oversized pool rejected");
    e[1]=lp_extent(32,64);
    CHECK(loom_pool_prepare(&lp_bank,&lp_pool,lp_ref(0,1),e,2)==-T_E_INVAL,"canonical aliases rejected");
    static const struct loom_pool_bank empty={0};
    CHECK(!lp_compare(&lp_bank,&empty,sizeof lp_bank),"failed registration atomic");
    e[1]=lp_extent(UINT64_MAX-1,2);
    CHECK(loom_pool_prepare(&lp_bank,&lp_pool,lp_ref(0,1),e,2)==-T_E_INVAL,"extent overflow rejected");
    e[1]=lp_extent(64,64);
    CHECK(!loom_pool_prepare(&lp_bank,&lp_pool,lp_ref(0,1),e,32),"reserve provisional quota");
    CHECK(loom_pool_conflicts(&lp_bank,lp_extent(1,1)),"provisional extent excludes fixed I/O");
    CHECK(!loom_pool_conflicts(&lp_bank,lp_extent(2048,1)),"adjacent extent permitted");
    struct loom_pool other={0};
    CHECK(loom_pool_prepare(&lp_bank,&other,lp_ref(1,1),e+32,33)==-T_E_NOSPC,"combined member ceiling");
    CHECK(!loom_pool_prepare(&lp_bank,&other,lp_ref(1,1),e+32,32),"second pool shares bank");
    struct loom_pool third={0};
    CHECK(loom_pool_prepare(&lp_bank,&third,lp_ref(2,1),e+64,1)==-T_E_NOSPC,"provisional quota visible");
    struct loom_service_buffer_receipt r={0};
    CHECK(loom_pool_claim(&lp_bank,&lp_pool,1,&r)==-T_E_NOENT,"provisional pool inaccessible");
    CHECK(!loom_pool_rollback(&lp_bank,&lp_pool),"copyout fault rolls back");
    CHECK(!loom_pool_conflicts(&lp_bank,lp_extent(0,1)),"rollback releases exclusion");
    CHECK(loom_pool_conflicts(&lp_bank,lp_extent(2048,1)),"rollback retains sibling");
    CHECK(!loom_pool_prepare(&lp_bank,&lp_pool,lp_ref(0,2),e,32),"replacement with new incarnation");
    CHECK(!loom_pool_rollback(&lp_bank,&lp_pool) && !loom_pool_rollback(&lp_bank,&other),"rollback releases all quota");
    CHECK(!lp_compare(&lp_bank,&empty,sizeof lp_bank),"no provisional residue");
    return NULL;
}
static const char *lp_leases(void) {
    lp_clear(); LP_TRY(lp_create(2));
    struct loom_service_buffer_receipt a,b,c,bad;
    CHECK(!loom_pool_claim(&lp_bank,&lp_pool,32,&a),"first claim");
    CHECK(loom_pool_commit(&lp_bank,&lp_pool,&a,33,0,true)==-T_E_INVAL,"reply bounded by request");
    LP_TRY(lp_payload(a,0xa1,true));
    CHECK(!loom_pool_claim(&lp_bank,&lp_pool,32,&b),"second claim");
    CHECK(a.member!=b.member && a.lease!=b.lease,"held member not selected");
    LP_TRY(lp_payload(b,0xb2,false)); LP_TRY(lp_snapshot(0,0,0,2));
    CHECK(loom_pool_claim(&lp_bank,&lp_pool,32,&c)==-T_E_AGAIN,"empty pool waits without reusing payload");
    bad=b;bad.lease++;
    CHECK(loom_pool_return(&lp_bank,&lp_pool,&bad)==-T_E_NOENT,"wrong nonce cannot return");
    bad=b;bad.pool.incarnation++;
    CHECK(loom_pool_return(&lp_bank,&lp_pool,&bad)==-T_E_NOENT,"wrong incarnation cannot return");
    bad=b;bad.reserved=1;
    CHECK(loom_pool_return(&lp_bank,&lp_pool,&bad)==-T_E_INVAL,"malformed receipt rejected");
    CHECK(!loom_pool_return(&lp_bank,&lp_pool,&b),"return out of order");
    CHECK(!loom_pool_claim(&lp_bank,&lp_pool,32,&c) && c.member==b.member && c.lease>b.lease,"returned buffer gets fresh lease");
    LP_TRY(lp_payload(c,0xc3,true));
    CHECK(loom_pool_return(&lp_bank,&lp_pool,&b)==-T_E_NOENT,"duplicate cannot release replacement");
    for(u32 i=0;i<19;i++) CHECK(lp_backing[a.member*64+i]==0xa1,"retained bytes stable across CQ reuse");
    CHECK(!loom_pool_stream_put(&lp_pool),"scope retires with held payloads");
    CHECK(loom_pool_reap(&lp_bank,&lp_pool)==-T_E_BUSY,"retirement cannot recycle held payload");
    CHECK(!loom_pool_return(&lp_bank,&lp_pool,&a) && !loom_pool_return(&lp_bank,&lp_pool,&c),"returns survive source retirement");
    CHECK(!loom_pool_reap(&lp_bank,&lp_pool),"reap after final lease");
    struct loom_pool_extent e=lp_extent(0,64);
    CHECK(!loom_pool_prepare(&lp_bank,&lp_pool,lp_ref(3,8),&e,1),"reuse pool slot");
    CHECK(!loom_pool_publish(&lp_pool) && !loom_pool_stream_get(&lp_pool),"replacement stream");
    CHECK(!loom_pool_claim(&lp_bank,&lp_pool,32,&b),"replacement claim"); LP_TRY(lp_payload(b,0xd4,true));
    bad=b;bad.pool.incarnation=7;
    CHECK(loom_pool_return(&lp_bank,&lp_pool,&bad)==-T_E_NOENT,"old pool cannot release new lease");
    CHECK(!loom_pool_return(&lp_bank,&lp_pool,&b),"replacement return");
    return NULL;
}
static const char *lp_exhaustion(void) {
    lp_clear();LP_TRY(lp_create(1));struct loom_service_buffer_receipt a,b;
    CHECK(loom_pool_claim(&lp_bank,&lp_pool,65,&a)==-T_E_INVAL,"maximum must fit every member");
    lp_bank.next_nonce=UINT64_MAX-1;
    CHECK(!loom_pool_claim(&lp_bank,&lp_pool,64,&a) && a.lease==UINT64_MAX,"last nonce remains nonzero");
    CHECK(loom_pool_claim(&lp_bank,&lp_pool,64,&b)==-T_E_NOSPC,"exhaustion cannot wait or wrap");
    CHECK(!loom_pool_release_busy(&lp_bank,&lp_pool,&a),"abort releases local writer");
    CHECK(loom_pool_claim(&lp_bank,&lp_pool,64,&b)==-T_E_NOSPC,"failed shot nonce remains burned");
    LP_TRY(lp_snapshot(1,0,0,0));
    return NULL;
}
static const char *lp_pending_retirement(void) {
    lp_clear();LP_TRY(lp_create(1));struct loom_service_buffer_receipt a;
    CHECK(!loom_pool_claim(&lp_bank,&lp_pool,20,&a),"retirement claim");
    CHECK(loom_pool_reap(&lp_bank,&lp_pool)==-T_E_BUSY,"stream blocks reap");
    CHECK(!loom_pool_stream_put(&lp_pool),"drop source ref");
    CHECK(loom_pool_reap(&lp_bank,&lp_pool)==-T_E_BUSY,"busy blocks reap");
    CHECK(!loom_pool_commit(&lp_bank,&lp_pool,&a,1,42,true),"pre-abort committed reply");
    CHECK(loom_pool_reap(&lp_bank,&lp_pool)==-T_E_BUSY,"pending blocks reap");
    CHECK(!loom_pool_deliver(&lp_bank,&lp_pool,&a),"pending can publish after source retirement");
    CHECK(!loom_pool_return(&lp_bank,&lp_pool,&a) && !loom_pool_reap(&lp_bank,&lp_pool),"terminal and payload lifetimes independent");
    return NULL;
}
static const char *private_pool_fixture_run(void) {
    LP_TRY(lp_registration()); LP_TRY(lp_leases());
    LP_TRY(lp_exhaustion()); LP_TRY(lp_pending_retirement());
    return NULL;
}
#undef CHECK
#undef LP_TRY
#endif
