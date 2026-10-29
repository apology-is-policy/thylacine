
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thylacine/loom_service_pool_table.h>
#include <thylacine/errno.h>
#define PAGE_SIZE 4096
#define SLUB_MAX_OBJECT_SIZE 2048
#define KP_ZERO 1
#define PROC_MAGIC 123
#define LOOM_MAX_REG_HANDLES 2
#define LOOM_MAX_REG_BUFFERS 2
#define CHECK(c,msg) do { if(!(c)) { puts(msg); exit(11); } } while(0)
typedef struct { int held; } spin_lock_t;
static int locks;
static void spin_lock(spin_lock_t *l) { CHECK(!l->held,"recursive lock");l->held=1;locks++; }
static void spin_unlock(spin_lock_t *l) { CHECK(l->held,"unlock without lock");l->held=0;locks--; }
static void extinction(const char *s) { puts(s);exit(12); }
struct AddrSpace { spin_lock_t lock;u32 pages,budget,private_rings,refs; };
struct Proc { u32 magic;struct AddrSpace *as;u64 stripes; };
struct Burrow { int unused; };
struct loom_reg_buffer { struct Burrow *burrow; void *kva; u32 len, _pad; };
struct Loom {
    struct AddrSpace *service_as;
    struct loom_slot_table *service_slots;
    struct loom_service_pool_table *service_pools;
    u64 service_creator;
    u32 service_metadata_pages;
    spin_lock_t lock;
    bool service_closing,sqpoll;
    u32 n_reg_buf,inflight_ops;
    struct loom_reg_buffer reg_buf[LOOM_MAX_REG_BUFFERS];
    struct { void *spoor; } reg[LOOM_MAX_REG_HANDLES];
    void *ring;u32 magic;
    char buddy_padding[3000];
};
struct loom_layout { u32 ring_size; };
static bool service_retire_ready=true,layout_fault,slots_fault,pools_fault,alloc_fault,inner_fault;
static u64 g_loom_destroyed,service_retired_count;
static void *allocations[8];
static int live,allocated,freed,recorded;
static u32 ring_charge;
static bool loom_private_layout_fault_take(void) { bool b=layout_fault;layout_fault=false;return b; }
static bool loom_private_pools_fault_take(void) { bool b=pools_fault;pools_fault=false;return b; }
static bool loom_private_slots_fault_take(void) { bool b=slots_fault;slots_fault=false;return b; }
static bool loom_measure(u32 sq,u32 cq,bool receipts,struct loom_layout *g) {
    CHECK(receipts,"fixture receipts");g->ring_size=PAGE_SIZE;return sq==2 && cq==2;
}
static bool addrspace_private_begin(struct AddrSpace *a) {
    CHECK(!locks,"image pin outside leaf lock");a->private_rings++;a->refs++;return true;
}
static void addrspace_private_end(struct AddrSpace *a) {
    CHECK(!locks && a->private_rings && a->refs>1,"image release ordering");
    a->private_rings--;a->refs--;
}
static u64 burrow_backing_pages(u64 bytes) {
    u64 pages=1;while(pages*PAGE_SIZE<bytes) pages*=2;return pages;
}
static bool proc_resource_exempt(struct Proc *p) { (void)p;return false; }
static bool addrspace_charge_pages(struct AddrSpace *a,u32 n,bool exempt) {
    CHECK(a->lock.held && !exempt,"charge under image lock");
    if(n>a->budget-a->pages) return false;a->pages+=n;return true;
}
static void addrspace_uncharge_pages(struct AddrSpace *a,u32 n) {
    CHECK(a->lock.held && n<=a->pages,"refund under image lock");a->pages-=n;
}
static void *kmalloc(size_t n,unsigned flags) {
    CHECK(!locks && flags==KP_ZERO && allocated<8,"allocation contract");
    if(alloc_fault) { alloc_fault=false;return NULL; }
    void *p=calloc(1,n);CHECK(p,"host allocation");allocations[allocated++]=p;live++;return p;
}
static void kfree(void *p) {
    if(!p)return;CHECK(!locks,"free outside lock");
    for(int i=0;i<allocated;i++) if(allocations[i]==p) {
        allocations[i]=NULL;live--;freed++;free(p);return;
    }
    extinction("free without ownership");
}
static struct Loom *loom_create_layout(u32 sq,u32 cq,bool exempt,bool receipts) {
    (void)sq;(void)cq;(void)exempt;(void)receipts;
    if(inner_fault)return NULL;
    struct Loom *l=kmalloc(sizeof(*l),KP_ZERO);if(l)l->ring=(void*)1;return l;
}
static void burrow_charge_record(void *ring,struct Proc *p,u32 pages) {
    CHECK(!locks && ring==(void*)1 && p->as->private_rings==1,"ring charge publication");
    ring_charge=pages;recorded++;
}
static bool burrow_unref_settled_in(void *ring,struct AddrSpace *a,u32 *refund) {
    CHECK(!locks && ring==(void*)1 && a->private_rings==1,"settled drop before image put");
    *refund=ring_charge;ring_charge=0;return true;
}
static bool loom_is_private(const struct Loom *l) { return l && l->service_as; }

struct Loom *loom_create_private(struct Proc *p, u32 sq, u32 cq, bool receipts) {
    // Taken before any refusal, so a shot armed for this call cannot outlive it.
    bool layout_fault = loom_private_layout_fault_take();
    bool slots_fault = loom_private_slots_fault_take();
    bool pools_fault = loom_private_pools_fault_take();
    struct loom_layout g;
    if (!service_retire_ready || !p || p->magic != PROC_MAGIC || !p->as ||
        !loom_measure(sq, cq, receipts, &g)) return NULL;
    struct AddrSpace *as = p->as;
    if (!addrspace_private_begin(as)) return NULL;
    // Loom uses the buddy path of kmalloc; a conservative whole page also
    // attributes its ring's slab descriptor for as long as ring mappings live.
    _Static_assert(sizeof(struct Loom) > 2048, "private Loom metadata uses buddy kmalloc");
    _Static_assert(sizeof(struct Burrow) <= PAGE_SIZE, "descriptor page attribution bounds object");
    // Reserve an entire buddy page for the 1552-byte identity ledger. Charging
    // that physical page avoids pretending shared slab occupancy is exact.
    // No allocator work under as->lock; unwind frees storage BEFORE its refund.
    _Static_assert(sizeof(struct loom_slot_table) <= PAGE_SIZE, "slot ledger page");
    _Static_assert(PAGE_SIZE > SLUB_MAX_OBJECT_SIZE, "slot page uses buddy kmalloc");
    _Static_assert(sizeof(struct loom_service_pool_table) > SLUB_MAX_OBJECT_SIZE,
                   "pool table uses buddy kmalloc");
    u32 metadata = (u32)burrow_backing_pages(sizeof(struct Loom)) + 1u
        ;
    u32 backing = (u32)burrow_backing_pages(g.ring_size) + 1u;
    bool exempt = proc_resource_exempt(p);
    spin_lock(&as->lock);
    bool charged = addrspace_charge_pages(as, metadata + backing, exempt);
    spin_unlock(&as->lock);
    if (!charged) { addrspace_private_end(as); return NULL; }
    struct loom_slot_table *slots = slots_fault ? NULL : kmalloc(PAGE_SIZE, KP_ZERO);
    struct loom_service_pool_table *pools = (!slots || pools_fault) ? NULL :
        kmalloc(sizeof(struct loom_service_pool_table), KP_ZERO);
    struct Loom *l = (!slots || !pools || layout_fault) ? NULL :
        loom_create_layout(sq, cq, exempt, receipts);
    if (!l) {
        kfree(pools);
        kfree(slots);
        spin_lock(&as->lock);
        addrspace_uncharge_pages(as, metadata + backing);
        spin_unlock(&as->lock);
        addrspace_private_end(as);
        return NULL;
    }
    l->service_slots = slots;
    l->service_pools = pools;
    l->service_as = as;
    l->service_creator = p->stripes;
    l->service_metadata_pages = metadata;
    // The charge follows the ring Burrow, including when a mapping survives fd
    // retirement. No raw Proc pointer is consulted by the eventual refund.
    burrow_charge_record(l->ring, p, backing);
    return l;
}
void loom_exec_latch(struct Loom *l) {
    if (!loom_is_private(l)) return;
    spin_lock(&l->lock);
    l->service_closing = true;
    loom_slots_close(l->service_slots);
    spin_unlock(&l->lock);
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
static void loom_private_destroy(struct Loom *l) {
    // Snapshotted before anything frees l: everything after the kfree below
    // reaches the address space through these two locals and never through l.
    struct AddrSpace *as = l->service_as;
    u32 metadata = l->service_metadata_pages;

    // Until the private engine owns its cleanup, refuse every legacy admission
    // path. A retirement must never enter blocking legacy clunk/join code.
    if (l->sqpoll || l->inflight_ops)
        extinction("private Loom reached legacy cleanup state");
    for (u32 i = 0; i < LOOM_MAX_REG_HANDLES; i++)
        if (l->reg[i].spoor) extinction("private Loom retained a legacy fid");

    // The engine must retire every record before handing its last owner ref to
    // this queue. Today no service handler admits records; fail closed if an
    // internal caller violates that boundary, rather than freeing live borrows.
    if (!l->service_slots) extinction("private Loom lost slot storage");
    for (u32 i = 0; i < LOOM_SERVICE_SLOT_COUNT; ++i)
        if (l->service_slots->slots[i].phase != LSLOT_FREE ||
            l->service_slots->slots[i].pins)
            extinction("private Loom retained a service slot");
    if (!loom_service_pools_empty(l->service_pools))
        extinction("private Loom retained pool storage");
    // No pool, writer or owner borrower survives here. Detach the bounded table
    // before settling each pin outside locks, while the exact image is retained.
    for (u32 i = 0; i < LOOM_MAX_REG_BUFFERS; ++i) {
        struct Burrow *b = l->reg_buf[i].burrow;
        l->reg_buf[i] = (struct loom_reg_buffer){0};
        loom_private_drop_buffer(as, b);
    }
    l->n_reg_buf = 0;
    kfree(l->service_pools);
    kfree(l->service_slots);

    // One settled drop rather than claim / drop / restore. The refund is decided
    // inside the same v->lock interval that decides whether this drop ends the
    // ring's occupancy, so a peer holder's final drop cannot land in a gap and
    // leave this one writing a charge record through a freed descriptor. A
    // nonfinal drop reports zero and leaves the record for whichever drop does
    // end the occupancy -- a ring mapping that outlives the fd settles it at VMA
    // teardown, which is why the refund is read from the drop and not from a
    // value claimed before it. The guards above bound LEGACY EXECUTION, BUFFER
    // AND FID state; they say nothing about mapping references, so a second
    // holder of this ring is not merely possible but routine, and no exclusivity
    // argument is available here or relied on.
    u32 refund = 0;
    (void)burrow_unref_settled_in(l->ring, as, &refund);

    l->magic = 0;
    kfree(l);

    // The pin addrspace_private_begin took is what keeps `as` addressable here,
    // so it is released LAST: addrspace_private_end may drop the final lifetime
    // reference and free the descriptor, and nothing may touch `as` after it.
    spin_lock(&as->lock);
    addrspace_uncharge_pages(as, metadata + refund);
    spin_unlock(&as->lock);
    addrspace_private_end(as);

    __atomic_fetch_add(&g_loom_destroyed, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&service_retired_count, 1, __ATOMIC_RELEASE);
}
int main(int argc,char **argv) {
    int mode=argc>1?atoi(argv[1]):0;
    struct AddrSpace a={.pages=10,.budget=100,.refs=1};
    struct Proc p={.magic=PROC_MAGIC,.as=&a,.stripes=987};
    // Separate processes isolate each failure edge and its exact allocation set.
    if(mode==1)a.budget=10;
    if(mode==2)slots_fault=true;
    if(mode==3)alloc_fault=true;
    if(mode==4)layout_fault=true;
    if(mode==5)inner_fault=true;
    if(mode==6)slots_fault=true;
    if(mode==8)pools_fault=true;
    struct Loom *l=loom_create_private(&p,mode==6?3:2,2,true);
    CHECK(!slots_fault && !pools_fault && !layout_fault,"one-shot faults consumed");
    if((mode>=1 && mode<=6) || mode==8) {
        CHECK(!l,"fault refuses publication");
        CHECK(live==0,"rollback frees slot and pool pages");
        CHECK(a.pages==10,"refusal restores accounting");
        CHECK(a.refs==1 && !a.private_rings,"refusal restores lifetime");
        CHECK(!recorded,"refusal publishes no ring charge");
        puts("PASS refusal");return 0;
    }
    CHECK(l && l->service_slots && l->service_pools && live==3,"owner retains all three allocations");
    CHECK(l->service_as==&a && l->service_creator==987,"exact image and identity");
    CHECK(l->service_metadata_pages==burrow_backing_pages(sizeof(*l))+1+burrow_backing_pages(sizeof(*l->service_pools)),
          "slot and pool pages charged in metadata");
    CHECK(a.pages==10+l->service_metadata_pages+2,"complete owner charge");
    struct loom_service_ref ref;
    spin_lock(&l->lock);
    CHECK(!loom_slot_reserve(l->service_slots,LOOM_SERVICE_SCOPE,&ref),"reserve");
    CHECK(!loom_slot_publish(l->service_slots,ref),"publish");
    CHECK(!loom_slot_pin(l->service_slots,ref,LOOM_SERVICE_SCOPE),"pin");
    spin_unlock(&l->lock);
    loom_exec_latch(l);
    CHECK(l->service_slots->closing,"close freezes ledger");
    if(mode==7) {
        loom_private_destroy(l);
        CHECK(false,"busy retirement returned");
    }
    spin_lock(&l->lock);
    CHECK(!loom_slot_unpin(l->service_slots,ref),"unpin after close");
    CHECK(!loom_slot_retire(l->service_slots,ref),"retire after close");
    CHECK(!loom_slot_reap(l->service_slots,ref),"reap after close");
    spin_unlock(&l->lock);
    loom_private_destroy(l);
    CHECK(live==0 && freed==3,"retirement frees owner and tables");
    CHECK(a.pages==10 && a.refs==1 && !a.private_rings,"retirement restores image");
    CHECK(service_retired_count==1 && g_loom_destroyed==1,"retirement published once");
    puts("PASS owner");return 0;
}
