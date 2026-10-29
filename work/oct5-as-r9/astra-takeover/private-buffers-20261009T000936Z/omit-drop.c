
#include <stdio.h>
#include <stdlib.h>
#include <thylacine/loom_service_pool_table.h>
#include <thylacine/errno.h>
#define LOOM_MAGIC 123
#define PROC_MAGIC 456
#define LOOM_MAX_REG_BUFFERS 2
#define CHECK(c,s) do { if(!(c)) { puts(s);exit(11); } } while(0)
typedef struct { int held; } spin_lock_t;
static int held;
static void spin_lock(spin_lock_t *l) { CHECK(!l->held,"lock reentry");l->held=1;held++; }
static void spin_unlock(spin_lock_t *l) { CHECK(l->held,"lock release");l->held=0;held--; }
struct AddrSpace { spin_lock_t lock;u32 pages; };
struct Burrow { struct AddrSpace *payer;int refs;bool mapped; };
struct Proc { u32 magic;struct AddrSpace *as;u64 stripes; };
struct loom_buf_reg { u64 va,len; };
struct loom_reg_buffer { struct Burrow *burrow;u8 *kva;u32 len,_pad; };
struct Loom {
 u32 magic;spin_lock_t lock;struct AddrSpace *service_as;u64 service_creator;
 bool service_closing;struct loom_service_pool_table *service_pools;
 struct Burrow *ring;struct loom_reg_buffer reg_buf[2];u32 n_reg_buf;
};
static struct AddrSpace a={.pages=10},other={.pages=10};
static struct Burrow buffer={.payer=&a,.mapped=true},ring={.payer=&a,.mapped=true};
static bool loom_is_private(struct Loom *l) { return l && l->service_as; }
static bool burrow_unref_settled_in(struct Burrow *b,struct AddrSpace *as,u32 *refund) {
 CHECK(!held && b->refs>0,"drop outside locks with owned pin");b->refs--;
 *refund=(!b->refs && !b->mapped && b->payer==as)?1:0;return !b->refs && !b->mapped;
}
static void addrspace_uncharge_pages(struct AddrSpace *as,u32 n) {
 CHECK(as->lock.held && n<=as->pages,"exact-image refund under lock");as->pages-=n;
}
static int loom_resolve_buf(struct Proc *p,const struct loom_buf_reg *input,struct loom_reg_buffer *out) {
 CHECK(p->as->lock.held,"resolution holds creator image lock");
 if(input->len!=64 || (input->va!=1 && input->va!=2))return -1;
 struct Burrow *b=input->va==1?&buffer:&ring;
 b->refs++;*out=(struct loom_reg_buffer){.burrow=b,.len=64};return 0;
}

bool loom_service_pools_empty(const struct loom_service_pool_table *pools) {
    if (!pools) return false;
    for (u32 i = 0; i < LOOM_SERVICE_SLOT_COUNT; ++i)
        if (pools->pools[i].phase != LPOOL_EMPTY) return false;
    for (u32 i = 0; i < LOOM_SERVICE_POOL_MEMBERS; ++i)
        if (pools->bank.cells[i].phase != LMEMBER_FREE) return false;
    return true;
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
int loom_private_register_buffers(struct Loom *l, struct Proc *p,
                                  const struct loom_buf_reg *bufs, u32 n) {
    if (!l || l->magic != LOOM_MAGIC || !loom_is_private(l) ||
        !p || p->magic != PROC_MAGIC) return -T_E_INVAL;
    struct AddrSpace *as = l->service_as;
    if (p->as != as || p->stripes != l->service_creator) return -T_E_ACCES;
    if (n > LOOM_MAX_REG_BUFFERS || (n && !bufs)) return -T_E_INVAL;

    struct loom_reg_buffer fresh[LOOM_MAX_REG_BUFFERS];
    for (u32 i = 0; i < LOOM_MAX_REG_BUFFERS; ++i)
        fresh[i] = (struct loom_reg_buffer){0};
    u32 done = 0;
    int rc = 0;
    // Resolve the complete set against the retained creator image. Refuse the
    // ring itself, even when its mapping passes the ordinary ANON/RW gates.
    spin_lock(&as->lock);
    while (done < n) {
        if (loom_resolve_buf(p, &bufs[done], &fresh[done])) {
            rc = -T_E_INVAL;
            break;
        }
        if (fresh[done++].burrow == l->ring) { rc = -T_E_INVAL; break; }
    }
    spin_unlock(&as->lock);
    if (!rc) {
        spin_lock(&l->lock);
        // Close may win while resolution runs. Both provisional and live pools
        // forbid rebinding; the old table remains intact on either refusal.
        if (l->service_closing) rc = -T_E_CANCELED;
        else if (!loom_service_pools_empty(l->service_pools)) rc = -T_E_BUSY;
        else {
            for (u32 i = 0; i < LOOM_MAX_REG_BUFFERS; ++i) {
                struct loom_reg_buffer old = l->reg_buf[i];
                l->reg_buf[i] = fresh[i];
                fresh[i] = old;
            }
            l->n_reg_buf = n;
            done = LOOM_MAX_REG_BUFFERS; // displaced pins, not the new table
        }
        spin_unlock(&l->lock);
    }
    // Every refusal and replacement uses the same settled-drop primitive. An
    // unmap can have made this pin the final holder during the unlocked gap.
    for (u32 i = 0; i < done; ++i)
        if (false) loom_private_drop_buffer(as, fresh[i].burrow);
    return rc;
}

int main(void) {
 struct Proc p={.magic=PROC_MAGIC,.as=&a,.stripes=99};
 struct loom_service_pool_table pools={0};
 struct Loom l={.magic=LOOM_MAGIC,.service_as=&a,.service_creator=99,.service_pools=&pools,.ring=&ring};
 struct loom_buf_reg good={1,64},bad[2]={{1,64},{3,64}},self={2,64};
 p.as=&other;
 CHECK(loom_private_register_buffers(&l,&p,NULL,0)==-T_E_ACCES && !buffer.refs,"wrong image refused before pin");
 p.as=&a;p.stripes=100;
 CHECK(loom_private_register_buffers(&l,&p,&good,1)==-T_E_ACCES && !buffer.refs,"wrong creator refused before pin");p.stripes=99;
 CHECK(!loom_private_register_buffers(&l,&p,&good,1) && buffer.refs==1 && l.n_reg_buf==1,"table adopts exactly one pin");
 CHECK(loom_private_register_buffers(&l,&p,bad,2)==-T_E_INVAL && buffer.refs==1 && l.reg_buf[0].burrow==&buffer,"partial refusal drops speculative pin");
 CHECK(loom_private_register_buffers(&l,&p,&self,1)==-T_E_INVAL && !ring.refs && buffer.refs==1,"own ring refused without leak");
 pools.pools[0].phase=LPOOL_RESERVED;
 CHECK(loom_private_register_buffers(&l,&p,&good,1)==-T_E_BUSY && buffer.refs==1,"provisional pool blocks replacement");
 pools.pools[0].phase=LPOOL_LIVE;
 CHECK(loom_private_register_buffers(&l,&p,NULL,0)==-T_E_BUSY && buffer.refs==1,"live pool blocks clear");
 pools.pools[0].phase=LPOOL_EMPTY;
 CHECK(!loom_private_register_buffers(&l,&p,&good,1) && buffer.refs==1,"replacement releases displaced pin");
 buffer.mapped=false;
 CHECK(!loom_private_register_buffers(&l,&p,NULL,0) && !buffer.refs && a.pages==9,"last pin settles creator charge");
 buffer.payer=&other;buffer.mapped=true;
 CHECK(!loom_private_register_buffers(&l,&p,&good,1),"foreign backing allowed");buffer.mapped=false;
 CHECK(!loom_private_register_buffers(&l,&p,NULL,0) && !buffer.refs && a.pages==9 && other.pages==10,"foreign backing never refunds caller");
 l.service_closing=true;
 CHECK(loom_private_register_buffers(&l,&p,&good,1)==-T_E_CANCELED && !buffer.refs,"close rejects admission and drops pin");
 CHECK(!held,"no lock retained");puts("PASS private buffer admission and pin settlement");return 0;
}
