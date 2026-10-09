
#include <stdio.h>
#include <stdlib.h>
#include <thylacine/loom_service_pool_table.h>
#include <thylacine/errno.h>
#define LOOM_MAGIC 123
#define PROC_MAGIC 456
#define BURROW_TYPE_ANON 1
#define CHECK(c,s) do { if(!(c)) { puts(s);exit(11); } } while(0)
typedef struct { int held; } spin_lock_t;
static int held;
static void (*on_unlock)(void);
static void spin_lock(spin_lock_t *l) { CHECK(!l->held,"lock reentry");l->held=1;held++; }
static void spin_unlock(spin_lock_t *l) {
 CHECK(l->held,"lock release");l->held=0;held--;
 if(on_unlock) {void(*f)(void)=on_unlock;on_unlock=NULL;f();}
}
struct AddrSpace { spin_lock_t lock;u32 pages; };
struct Burrow { u32 type;u8 *pages;u64 size;struct AddrSpace *payer;int refs; };
struct Proc { u32 magic;struct AddrSpace *as;u64 stripes; };
struct loom_reg_buffer { struct Burrow *burrow;u8 *kva;u32 len,_pad; };
struct Loom {
 u32 magic;spin_lock_t lock;struct AddrSpace *service_as;u64 service_creator;
 bool service_closing;struct loom_service_pool_table *service_pools;
 struct loom_slot_table *service_slots;u32 receipt_size;
 struct Burrow *ring;struct loom_reg_buffer reg_buf[2];u32 n_reg_buf;
 void *inflight_ops,*chain;u32 async_inflight,admitting,chain_len,rearm_pending;
};
static struct AddrSpace a={.pages=10},other={.pages=10};
static u8 storage[4096];
static struct Burrow buffer={.type=BURROW_TYPE_ANON,.pages=storage,.size=sizeof(storage),.payer=&a,.refs=2};
static struct Burrow ring={.type=BURROW_TYPE_ANON,.pages=storage,.size=sizeof(storage),.payer=&a,.refs=2};
static bool loom_is_private(struct Loom *l) { return l && l->service_as; }
static u64 page_to_pa(u8 *p) {return (u64)p;}
static u8 *pa_to_kva(u64 p) {return (u8 *)p;}
static void burrow_ref(struct Burrow *b) {CHECK(held==1 && b->refs>0,"pin while table locked and alive");b->refs++;}
static bool burrow_unref_settled_in(struct Burrow *b,struct AddrSpace *as,u32 *refund) {
 CHECK(!held && b->refs>0,"drop outside locks with owned pin");b->refs--;
 *refund=(!b->refs && b->payer==as)?1:0;return !b->refs;
}
static void addrspace_uncharge_pages(struct AddrSpace *as,u32 n) {
 CHECK(as->lock.held && n<=as->pages,"exact-image refund under lock");as->pages-=n;
}
static void replace_table_after_unlock(void) {
 CHECK(!held && buffer.refs==4,"replacement sees independent pool pins");buffer.refs-=2;
}

static void loom_private_drop_buffer(struct AddrSpace *as, struct Burrow *b) {
    if (!b) return;
    u32 refund = 0;
    (void)burrow_unref_settled_in(b, as, &refund);
    if (refund) {
        spin_lock(&as->lock);
        addrspace_uncharge_pages(as, refund);
        spin_unlock(&as->lock);
    }
}
int loom_private_pool_prepare(struct Loom *l, struct Proc *p,
                              const struct loom_service_pool_create *in,
                              struct loom_service_ref *out) {
    if (!l || l->magic != LOOM_MAGIC || !loom_is_private(l) ||
        !p || p->magic != PROC_MAGIC || !in || !out) return -T_E_INVAL;
    if (p->as != l->service_as || p->stripes != l->service_creator)
        return -T_E_ACCES;
    if (!l->receipt_size) return -T_E_OPNOTSUPP;
    if (in->size != sizeof(*in) || in->version != 1 || in->flags ||
        in->reserved || in->result.slot || in->result.reserved ||
        in->result.incarnation || !in->count ||
        in->count > LOOM_SERVICE_POOL_MEMBERS) return -T_E_INVAL;
    for (u32 i = in->count; i < LOOM_SERVICE_POOL_MEMBERS; ++i) {
        const struct loom_service_pool_member *m = &in->members[i];
        if (m->buffer_index || m->reserved || m->offset || m->length)
            return -T_E_INVAL;
    }
    struct loom_pool_extent extents[LOOM_SERVICE_POOL_MEMBERS];
    int rc = 0;
    spin_lock(&l->lock);
    if (l->service_closing) { rc = -T_E_CANCELED; goto unlock; }
    // Private legacy dispatch is disabled. Refuse any unexpected legacy work,
    // including held/local admission, instead of pretending its extents were
    // checked. The future private engine must add exact accepted-fixed extent
    // reservations under this SAME lock before it can be enabled.
    if (l->inflight_ops || l->async_inflight || l->admitting || l->chain ||
        l->chain_len || l->rearm_pending) { rc = -T_E_BUSY; goto unlock; }
    for (u32 i = 0; i < in->count; ++i) {
        const struct loom_service_pool_member *m = &in->members[i];
        if (m->reserved || m->buffer_index >= l->n_reg_buf || !m->length) {
            rc = -T_E_INVAL; goto unlock;
        }
        const struct loom_reg_buffer *b = &l->reg_buf[m->buffer_index];
        if (!b->burrow || b->burrow == l->ring ||
            b->burrow->type != BURROW_TYPE_ANON || !b->burrow->pages ||
            m->offset > b->len || m->length > b->len - m->offset) {
            rc = -T_E_INVAL; goto unlock;
        }
        // Integer subtraction avoids pointer-difference UB and overflowing
        // user offset+length. Table pins guarantee this ANON direct-map base
        // remains stable even after every original user mapping is detached.
        u64 base = (u64)pa_to_kva(page_to_pa(b->burrow->pages));
        u64 kva = (u64)b->kva;
        if (kva < base || kva - base > b->burrow->size ||
            b->len > b->burrow->size - (kva - base)) {
            rc = -T_E_INVAL; goto unlock;
        }
        extents[i] = (struct loom_pool_extent){b->burrow,
                                              kva - base + m->offset,m->length};
    }
    rc = loom_service_pool_prepare(l->service_slots,l->service_pools,
                                   extents,in->count,out);
    if (!rc) {
        // One independent pin per member, including disjoint aliases of the
        // same Burrow. Ref cannot fail; the table's pin prevents destruction.
        // Provisional membership already excludes table replacement and other
        // overlapping pool registrations before this lock is released.
        for (u32 i = 0; i < in->count; ++i)
            burrow_ref((struct Burrow *)extents[i].backing);
    }
unlock:
    spin_unlock(&l->lock);
    return rc;
}
int loom_private_pool_publish(struct Loom *l, struct loom_service_ref ref) {
    if (!l || l->magic != LOOM_MAGIC || !loom_is_private(l)) return -T_E_INVAL;
    spin_lock(&l->lock);
    int rc = loom_service_pool_publish(l->service_slots,l->service_pools,ref);
    spin_unlock(&l->lock);
    return rc;
}
static int loom_private_pool_remove(struct Loom *l, struct loom_service_ref ref,
                                    bool rollback) {
    if (!l || l->magic != LOOM_MAGIC || !loom_is_private(l)) return -T_E_INVAL;
    struct Burrow *pins[LOOM_SERVICE_POOL_MEMBERS];
    u32 count = 0;
    spin_lock(&l->lock);
    for (u32 i = 0; i < LOOM_SERVICE_POOL_MEMBERS; ++i) {
        const struct loom_pool_cell *c = &l->service_pools->bank.cells[i];
        if (c->phase != LMEMBER_FREE && c->pool.slot == ref.slot &&
            c->pool.incarnation == ref.incarnation &&
            c->pool.reserved == ref.reserved)
            pins[count++] = (struct Burrow *)c->extent.backing;
    }
    int rc = rollback ? loom_service_pool_rollback(l->service_slots,l->service_pools,ref)
                      : loom_service_pool_reap(l->service_slots,l->service_pools,ref);
    spin_unlock(&l->lock);
    if (true)
        for (u32 i = 0; i < count; ++i)
            loom_private_drop_buffer(l->service_as,pins[i]);
    return rc;
}
int loom_private_pool_rollback(struct Loom *l, struct loom_service_ref ref) {
    return loom_private_pool_remove(l,ref,true);
}
int loom_private_pool_reap(struct Loom *l, struct loom_service_ref ref) {
    return loom_private_pool_remove(l,ref,false);
}

int main(void) {
 struct Proc p={.magic=PROC_MAGIC,.as=&a,.stripes=99};
 struct loom_service_pool_table pools={0};struct loom_slot_table slots={0};
 struct Loom l={.magic=LOOM_MAGIC,.service_as=&a,.service_creator=99,.service_pools=&pools,
   .service_slots=&slots,.receipt_size=64,.ring=&ring,.n_reg_buf=2,
   .reg_buf={{&buffer,storage+128,1024,0},{&buffer,storage+192,512,0}}};
 struct loom_service_pool_create in={.size=sizeof(in),.version=1,.count=2,
   .members={{0,0,64,64},{1,0,64,64}}};
 struct loom_service_ref ref={0},second={0},stale;
 p.as=&other;
 CHECK(loom_private_pool_prepare(&l,&p,&in,&ref)==-T_E_ACCES,"wrong image rejected");p.as=&a;
 p.stripes=100;
 CHECK(loom_private_pool_prepare(&l,&p,&in,&ref)==-T_E_ACCES,"wrong creator rejected");p.stripes=99;
 l.receipt_size=0;
 CHECK(loom_private_pool_prepare(&l,&p,&in,&ref)==-T_E_OPNOTSUPP,"receipt geometry required");l.receipt_size=64;
 in.result.incarnation=1;
 CHECK(loom_private_pool_prepare(&l,&p,&in,&ref)==-T_E_INVAL,"output field rejected");in.result.incarnation=0;
 in.members[63].length=1;
 CHECK(loom_private_pool_prepare(&l,&p,&in,&ref)==-T_E_INVAL,"unused tail rejected");in.members[63].length=0;
 in.members[0].reserved=1;
 CHECK(loom_private_pool_prepare(&l,&p,&in,&ref)==-T_E_INVAL,"member reserved rejected");in.members[0].reserved=0;
 in.members[0].offset=~(u64)0;
 CHECK(loom_private_pool_prepare(&l,&p,&in,&ref)==-T_E_INVAL,"wrapping range rejected");in.members[0].offset=64;
 in.members[0].length=1024;
 CHECK(loom_private_pool_prepare(&l,&p,&in,&ref)==-T_E_INVAL,"member end outside registration rejected");in.members[0].length=64;
 l.reg_buf[0].burrow=&ring;
 CHECK(loom_private_pool_prepare(&l,&p,&in,&ref)==-T_E_INVAL,"ring backing rejected");l.reg_buf[0].burrow=&buffer;
 l.reg_buf[0].kva=storage+4000;
 CHECK(loom_private_pool_prepare(&l,&p,&in,&ref)==-T_E_INVAL,"registration outside backing rejected");l.reg_buf[0].kva=storage+128;
 l.admitting=1;
 CHECK(loom_private_pool_prepare(&l,&p,&in,&ref)==-T_E_BUSY,"unresolved fixed admission rejected");l.admitting=0;
 l.chain=(void*)1;
 CHECK(loom_private_pool_prepare(&l,&p,&in,&ref)==-T_E_BUSY,"held fixed chain rejected");l.chain=NULL;
 in.members[1].offset=0;
 CHECK(loom_private_pool_prepare(&l,&p,&in,&ref)==-T_E_INVAL && !ref.incarnation && buffer.refs==2,
       "canonical alias overlap rejected");in.members[1].offset=64;
 CHECK(!loom_private_pool_prepare(&l,&p,&in,&ref) && buffer.refs==4,"member pins acquired exactly");
 CHECK(pools.bank.cells[0].extent.offset==192 && pools.bank.cells[1].extent.offset==256,
       "canonical offsets retain registered slice displacement");
 CHECK(loom_private_pool_prepare(&l,&p,&in,&second)==-T_E_BUSY && !second.incarnation && buffer.refs==4,
       "provisional overlap cannot leak pins");
 stale=ref;
 CHECK(!loom_private_pool_rollback(&l,ref) && buffer.refs==2,"rollback drops exactly member pins");
 CHECK(!loom_private_pool_prepare(&l,&p,&in,&ref) && buffer.refs==4 && ref.incarnation!=stale.incarnation,"identity burned");
 CHECK(loom_private_pool_rollback(&l,stale)==-T_E_NOENT && buffer.refs==4,"stale removal retains pins");
 CHECK(!loom_private_pool_publish(&l,ref),"publish succeeds");
 CHECK(loom_private_pool_rollback(&l,ref)==-T_E_NOENT && buffer.refs==4,"live rollback retains pins");
 CHECK(!loom_pool_stream_get(&pools.pools[ref.slot]),"stream get");
 CHECK(loom_private_pool_reap(&l,ref)==-T_E_BUSY && buffer.refs==4,"busy reap retains pins");
 CHECK(!loom_pool_stream_put(&pools.pools[ref.slot]),"stream put");
 CHECK(!loom_private_pool_reap(&l,ref) && buffer.refs==2,"reap drops exactly member pins");
 CHECK(!loom_private_pool_prepare(&l,&p,&in,&ref),"prepare before close");
 l.service_closing=true;loom_slots_close(&slots);
 CHECK(loom_private_pool_publish(&l,ref)==-T_E_CANCELED,"close refuses publication");
 CHECK(loom_private_pool_prepare(&l,&p,&in,&second)==-T_E_CANCELED,"close refuses prepare");
 on_unlock=replace_table_after_unlock;
 CHECK(!loom_private_pool_rollback(&l,ref) && buffer.refs==0 && a.pages==9,
       "after-close rollback settles final pins after concurrent table replacement");
 CHECK(loom_service_pools_empty(&pools) && !held,"empty ledgers and released locks");
 puts("PASS private pool canonical ownership and rollback");return 0;
}
