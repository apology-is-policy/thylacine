#include <thylacine/loom_service_protocol.h>
#include <thylacine/srvconn.h>
// Loom-2a tests -- the KObj_Loom ring substrate.
//
// Covers loom_create geometry + the kobj refcount lifecycle + the
// registered-handle table at the function level, and SYS_LOOM_SETUP /
// SYS_LOOM_REGISTER through the testable `_for_proc` inners on a fresh
// proc_alloc'd Proc (the sys_burrow_attach_for_proc test pattern). The full
// SYS_LOOM_SETUP user copy-in/out is trivial handler glue (E2E coverage lands
// with the native libthyla-rs API at Loom-6); the SQE dispatch + the CQE post
// arrive at Loom-3.
//
// Maps to specs/loom.tla: the ring geometry + the registered-object table (the
// spec's `reg`) + teardown (the spec's Teardown action) substrate.

#include "test.h"

#include <thylacine/loom.h>
#include <thylacine/loom_service_targets.h>
#include <thylacine/loom_service_nodes.h>
#include <thylacine/spoor.h>
#include <thylacine/burrow.h>
#include <thylacine/errno.h>
#include <thylacine/exec.h>
#include <thylacine/handle.h>
#include <thylacine/poll.h>
#include <thylacine/proc.h>
#include <thylacine/rendez.h>
#include <thylacine/sched.h>   // sched() -- cooperative yield to the SQPOLL kthread
#include <thylacine/thread.h>  // THREAD_SLEEPING -- the F2 park observation
#include <thylacine/types.h>
#include <thylacine/vma.h>

void test_loom_private_targets(void);
void test_loom_create_geometry(void);
void test_loom_create_rejects_bad_args(void);
void test_loom_refcount_lifecycle(void);
void test_loom_setup_via_proc(void);
void test_loom_setup_rejects(void);
void test_loom_register_handles(void);
void test_loom_register_rejects(void);
void test_loom_register_replaces(void);
void test_loom_post_cqe_back_pressure(void);
void test_loom_post_cqe_ignores_hostile_header(void);
void test_loom_dup_rejected(void);
void test_loom_enter_nop(void);
void test_loom_enter_submit_rejects(void);
void test_loom_enter_flags_and_bad_index(void);
void test_loom_enter_cq_admission_backpressure(void);
void test_loom_admission_counts_admitting(void);
void test_loom_wait_counts_admitting(void);
void test_loom_drain_waits_for_admitting(void);
void test_loom_cq_waiter_wake(void);
void test_loom_cq_waiter_no_spurious_wake_on_full(void);
void test_loom_poll(void);
void test_loom_enter_inline_min_complete(void);
void test_loom_enter_min_complete_no_inflight(void);
void test_loom_sqpoll_setup_and_teardown(void);
void test_loom_sqpoll_drains_sq(void);
void test_loom_sqpoll_parks_on_cq_full(void);

void test_loom_register_buffers(void);
void test_loom_register_buffers_rejects(void);
void test_loom_register_buffers_replace(void);
void test_loom_ring_charge_balance(void);
void test_loom_regbuf_charge_balance(void);
void test_loom_regbuf_foreign_charge_not_refunded(void);

// The testable syscall inners live in kernel/syscall.c (declared in loom.h);
// sys_pipe_for_proc is the KOBJ_SPOOR-pair maker (declared nowhere public --
// forward-declare it like test_sys_burrow.c does for its inner).
extern int sys_pipe_for_proc(struct Proc *p, hidx_t *out_rd, hidx_t *out_wr);
extern s64 sys_burrow_attach_for_proc(struct Proc *p, u64 length_raw);
extern s64 sys_burrow_detach_for_proc(struct Proc *p, u64 vaddr_raw, u64 length_raw);

static struct Proc *test_proc_make(void) {
    return proc_alloc();
}

static void test_proc_drop(struct Proc *p) {
    if (!p) return;
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// ---------------------------------------------------------------------------
// Geometry: the ring layout is consistent, page-rounded, and the header is
// stamped with the immutable masks/counts (visible via the kernel direct map).
// ---------------------------------------------------------------------------
#include "loom_receipt_fixture.h"
#include "loom_private_fixture.h"
#include "loom_slots_fixture.h"
#include "loom_nodes_fixture.h"

void test_loom_service_nodes(void);
void test_loom_service_nodes(void) {
    const char *error = loom_nodes_fixture();
    TEST_ASSERT(!error, error);
}

#include "loom_requests_fixture.h"
void test_loom_service_requests(void);
void test_loom_service_requests(void) {
    const char *error = loom_requests_fixture();
    TEST_ASSERT(!error,error);
}

static const char *loom_request_owner_fixture(void) {
    const char *error = NULL;
    struct Proc *p = test_proc_make();
    struct Loom *l = NULL;
    struct loom_service_ref scope = {0};
    struct loom_request_ref ref = {0};
    u64 goal = loom_private_retired();
    u32 base = p ? p->as->page_count : 0;
#define RO(c,m) do { if (!(c)) { error=m;goto done; } } while (0)
    RO(p && p->as,"request owner creator");
    loom_private_requests_fault_arm_for_test();
    l=loom_create_private(p,3,2,true);
    if (l) ++goal;
    RO(!l && !loom_private_requests_fault_armed_for_test(),"early refusal consumes request fault");
    loom_private_requests_fault_arm_for_test();
    l=loom_create_private(p,2,2,true);
    if (l) ++goal;
    RO(!l && !loom_private_requests_fault_armed_for_test() && p->as->page_count==base &&
       !p->as->private_rings,"request allocation failure unwinds tables and charge");
    l=loom_create_private(p,2,2,true);
    if (l) ++goal;
    RO(l && l->service_requests.records && l->service_requests.capacity==l->cq_entries &&
       loom_requests_empty(&l->service_requests),"CQ-sized zeroed request storage owned");
    struct loom_service_reserve in={.size=sizeof(in),.version=1,.kind=LOOM_SERVICE_SCOPE};
    RO(!loom_private_node_prepare(l,p,&in,&scope) && !loom_private_node_publish(l,scope),"request owner scope");
    spin_lock(&l->lock);
    int rc=loom_request_prepare(&l->service_requests,l->service_slots,l->service_nodes,scope,77,0,1,&ref);
    if (!rc) rc=loom_request_admit(&l->service_requests,l->service_slots,l->service_nodes,ref,1);
    if (!rc) rc=loom_request_finish(&l->service_requests,l->service_slots,l->service_nodes,ref,9);
    spin_unlock(&l->lock);
    RO(!rc,"request terminal committed");
    RO(!loom_post_cqe(l,1,1,0) && !loom_post_cqe(l,2,2,0),"fixture fills actual CQ");
    RO(loom_private_post_terminal(l,ref)==-T_E_AGAIN && l->cq_tail==2 &&
       l->service_requests.records[ref.index].phase==LREQ_TERMINAL,
       "full CQ retains terminal and private tail");
    loom_exec_latch(l); // prior committed success may still be delivered
    struct loom_ring_hdr *h=(void *)(l->ring_kva+l->hdr_off);
    h->cq_tail=0xFFFFFFFFu;h->cq_mask=0xFFFFFFFFu;
    __atomic_store_n(&h->cq_head,1,__ATOMIC_RELEASE);
    struct loom_service_buffer_receipt *receipts=(void *)(l->ring_kva+l->receipt_off);
    receipts[0].lease=999;
    RO(!loom_private_post_terminal(l,ref) && l->cq_tail==3 && h->cq_tail==3,
       "committed success survives close with private CQ geometry");
    struct loom_cqe *cq=(void *)(l->ring_kva+l->cqe_off);
    RO(cq[0].user_data==77 && cq[0].result==9 && !cq[0].flags && !receipts[0].lease,
       "terminal CQE exact and old receipt cleared");
    RO(loom_private_post_terminal(l,ref)==-T_E_BUSY && l->cq_tail==3,"no duplicate terminal post");
    spin_lock(&l->lock);
    rc=loom_request_reap(&l->service_requests,l->service_slots,ref);
    spin_unlock(&l->lock);
    RO(!rc && loom_requests_empty(&l->service_requests),"delivered request reaped");
done:
    loom_private_requests_fault_disarm_for_test();
    if (l) {
        // This fixture owns every request exclusively; there are no actual
        // protocol callbacks. Drain simulated records even on assertion failure.
        loom_exec_latch(l);
        spin_lock(&l->lock);
        for (u32 i=0;i<l->service_requests.capacity;++i) {
            struct loom_service_request *r=&l->service_requests.records[i];
            struct loom_request_ref rr={.index=i,.identity=r->identity};
            if (r->phase==LREQ_PREPARED) (void)loom_request_rollback(&l->service_requests,l->service_slots,rr);
            else if (r->phase!=LREQ_FREE) {
                while (r->borrows) (void)loom_request_put(&l->service_requests,rr);
                if (r->phase==LREQ_ACTIVE)
                    (void)loom_request_finish(&l->service_requests,l->service_slots,l->service_nodes,rr,-T_E_CANCELED);
                (void)loom_requests_detach(&l->service_requests,l->service_slots);
                if (r->phase==LREQ_TERMINAL) (void)loom_request_discard(&l->service_requests,rr);
                (void)loom_request_reap(&l->service_requests,l->service_slots,rr);
            }
        }
        spin_unlock(&l->lock);
        loom_unref(l);
    }
    if (!lp_wait(goal) && !error) error="request owner retirement timeout";
    if (p && !error && (p->as->page_count!=base || p->as->private_rings))
        error="request owner refunds exact charge";
    test_proc_drop(p);
#undef RO
    return error;
}
void test_loom_private_requests(void);
void test_loom_private_requests(void) {
    const char *error=loom_request_owner_fixture();
    TEST_ASSERT(!error,error);
}

#include "loom_node_owner_fixture.h"

void test_loom_private_node_owner(void);
void test_loom_private_node_owner(void) {
    struct Proc *p = test_proc_make();
    struct Loom *l = p ? loom_create_private(p,2,2,true) : NULL;
    u64 goal = loom_private_retired() + (l ? 1 : 0);
    const char *error = !l ? "node owner allocation" : loom_node_owner_fixture(l,p);
    if (l) loom_unref(l);
    if (!lp_wait(goal) && !error) error = "node owner retirement timeout";
    if (p) test_proc_drop(p);
    TEST_ASSERT(!error,error);
}

#include "loom_pool_table_fixture.h"

void test_loom_service_pool_table(void) {
    const char *error = loom_pool_table_fixture();
    TEST_ASSERT(error == NULL, error);
}

// Private owner integration: actual allocator/charge/close/retirer, while the
// sequential module fixture above remains independent of this owner wrapper.
static const char *loom_slot_owner_fixture(void) {
    const char *error = NULL;
    struct Proc *p = test_proc_make();
    struct Loom *l = NULL;
    struct loom_service_ref ref = {0};
    u64 goal = loom_private_retired();
    bool pinned = false;
#define SO_CHECK(c, msg) do { if (!(c)) { error = msg; goto done; } } while (0)
    SO_CHECK(p && p->as, "slot owner creator allocated");
    u32 base = p->as->page_count;
    int refs = addrspace_ref_count(p->as);
    loom_private_slots_fault_arm_for_test();
    l = loom_create_private(p, 3, 2, true);
    if (l) goal++;
    SO_CHECK(!l && !loom_private_slots_fault_armed_for_test(),
             "early refusal consumes slot allocation fault");
    loom_private_slots_fault_arm_for_test();
    l = loom_create_private(p, 2, 2, true);
    if (l) goal++;
    SO_CHECK(!l && !loom_private_slots_fault_armed_for_test(),
             "slot storage refusal consumes its fault");
    SO_CHECK(p->as->page_count == base && !p->as->private_rings &&
             addrspace_ref_count(p->as) == refs,
             "slot storage refusal rolls back charge and image pin");
    loom_private_pools_fault_arm_for_test();
    l = loom_create_private(p, 3, 2, true);
    if (l) goal++;
    SO_CHECK(!l && !loom_private_pools_fault_armed_for_test(),
             "early refusal consumes pool allocation fault");
    loom_private_pools_fault_arm_for_test();
    l = loom_create_private(p, 2, 2, true);
    if (l) goal++;
    SO_CHECK(!l && !loom_private_pools_fault_armed_for_test() &&
             p->as->page_count == base && !p->as->private_rings &&
             addrspace_ref_count(p->as) == refs,
             "pool storage refusal rolls back prior slot allocation and charge");
    loom_private_targets_fault_arm_for_test();
    l = loom_create_private(p, 3, 2, true);
    if (l) goal++;
    SO_CHECK(!l && !loom_private_targets_fault_armed_for_test(),
             "early refusal consumes target allocation fault");
    loom_private_targets_fault_arm_for_test();
    l = loom_create_private(p, 2, 2, true);
    if (l) goal++;
    SO_CHECK(!l && !loom_private_targets_fault_armed_for_test() &&
             p->as->page_count == base && !p->as->private_rings &&
             addrspace_ref_count(p->as) == refs,
             "target refusal rolls back prior tables and charge");
    loom_private_nodes_fault_arm_for_test();
    l = loom_create_private(p, 3, 2, true);
    if (l) goal++;
    SO_CHECK(!l && !loom_private_nodes_fault_armed_for_test(),
             "early refusal consumes node allocation fault");
    loom_private_nodes_fault_arm_for_test();
    l = loom_create_private(p, 2, 2, true);
    if (l) goal++;
    SO_CHECK(!l && !loom_private_nodes_fault_armed_for_test() &&
             p->as->page_count == base && !p->as->private_rings &&
             addrspace_ref_count(p->as) == refs,
             "node refusal rolls back prior tables and charge");
    // Allocation succeeds but downstream layout fails; the new page must also
    // be released. The host allocator control measures that free separately.
    loom_private_layout_fault_arm_for_test();
    l = loom_create_private(p, 2, 2, true);
    if (l) goal++;
    SO_CHECK(!l && p->as->page_count == base && !p->as->private_rings &&
             addrspace_ref_count(p->as) == refs,
             "downstream layout failure rolls back slot page charge and guard");
    l = loom_create_private(p, 2, 2, true);
    if (l) goal++;
    SO_CHECK(l && l->service_slots, "private owner owns its slot page");
    SO_CHECK(l->service_pools && loom_service_pools_empty(l->service_pools),
             "private owner owns zeroed pool storage");
    SO_CHECK(l->service_nodes && loom_service_nodes_empty(l->service_nodes),
             "private owner owns zeroed node storage");
    SO_CHECK(l->service_metadata_pages == (u32)burrow_backing_pages(sizeof(*l)) + 1u +
             (u32)burrow_backing_pages(sizeof(struct loom_service_pool_table)) +
             (u32)burrow_backing_pages(sizeof(struct loom_service_target_table)) + 2u,
             "slot pool target and node pages included in metadata charge");
    SO_CHECK(p->as->page_count == base + l->service_metadata_pages +
             (u32)burrow_backing_pages(l->ring_size) + 1u,
             "all base storage charged before owner publication");
    SO_CHECK(!l->service_slots->closing && !l->service_slots->next_incarnation,
             "slot page starts zero and open");
    spin_lock(&l->lock);
    int rc = loom_slot_reserve(l->service_slots, LOOM_SERVICE_SCOPE, &ref);
    if (!rc) rc = loom_slot_publish(l->service_slots, ref);
    if (!rc) { rc = loom_slot_pin(l->service_slots, ref, LOOM_SERVICE_SCOPE); pinned = !rc; }
    spin_unlock(&l->lock);
    SO_CHECK(!rc, "owner ledger admits exact internal borrow");
    struct loom_service_ref unused_live = {0}, unused_provisional = {0};
    spin_lock(&l->lock);
    rc = loom_service_node_prepare(l->service_slots, l->service_nodes,
                                  LOOM_SERVICE_SCOPE, (struct loom_service_ref){0}, &unused_live);
    if (!rc) rc = loom_service_node_publish(l->service_slots, l->service_nodes, unused_live);
    if (!rc) rc = loom_service_node_prepare(l->service_slots, l->service_nodes,
                                  LOOM_SERVICE_SCOPE, (struct loom_service_ref){0}, &unused_provisional);
    spin_unlock(&l->lock);
    SO_CHECK(!rc, "unused published and provisional scopes retained until owner cleanup");
    loom_exec_latch(l);
    spin_lock(&l->lock);
    rc = loom_slot_pin(l->service_slots, ref, LOOM_SERVICE_SCOPE);
    bool closed = l->service_slots->closing;
    u32 pins = l->service_slots->slots[ref.slot].pins;
    spin_unlock(&l->lock);
    SO_CHECK(closed && rc == -T_E_CANCELED && pins == 1,
             "owner close freezes new slots and retains existing borrow");
done:
    loom_private_slots_fault_disarm_for_test();
    loom_private_pools_fault_disarm_for_test();
    loom_private_targets_fault_disarm_for_test();
    loom_private_nodes_fault_disarm_for_test();
    loom_private_layout_fault_disarm_for_test();
    if (l && l->service_slots && ref.incarnation) {
        spin_lock(&l->lock);
        if (pinned) (void)loom_slot_unpin(l->service_slots, ref);
        if (l->service_slots->slots[ref.slot].phase == LSLOT_PROVISIONAL)
            (void)loom_slot_rollback(l->service_slots, ref);
        else {
            (void)loom_slot_retire(l->service_slots, ref);
            (void)loom_slot_reap(l->service_slots, ref);
        }
        spin_unlock(&l->lock);
    }
    if (l) loom_unref(l);
    if (!lp_wait(goal) && !error) error = "slot owner retirement timeout";
    if (p && !error && (p->as->page_count != base || p->as->private_rings ||
                        addrspace_ref_count(p->as) != refs))
        error = "slot page retirement refunds charge and image pin exactly once";
    if (p) test_proc_drop(p);
#undef SO_CHECK
    return error;
}
void test_loom_private_slot_owner(void) {
    const char *error = loom_slot_owner_fixture();
    TEST_ASSERT(error == NULL, error);
}

// Real private table pins and exact-image settlement; no private syscall or
// protocol admission is enabled by this internal fixture. Both Procs are idle.
static const char *loom_private_buffers_fixture(void) {
    const char *error = NULL;
    struct Proc *p = test_proc_make(), *q = test_proc_make();
    struct Loom *l = NULL;
    struct loom_service_ref pool = {0};
    u64 goal = loom_private_retired();
    u64 va = 0, ring_va = 0;
    u32 ring_size = 0, base = 0;
#define PB_CHECK(c,msg) do { if (!(c)) { error = msg; goto done; } } while (0)
    PB_CHECK(p && q, "private buffer creators allocated");
    base = p->as->page_count;
    l = loom_create_private(p,2,2,true);
    if (l) goal++;
    PB_CHECK(l, "private buffer owner allocated");
    u32 owner_pages = p->as->page_count;
    s64 attached = sys_burrow_attach_for_proc(p,PAGE_SIZE);
    PB_CHECK(attached > 0, "private buffer backing attached");
    va = (u64)attached;
    struct Burrow *b = vma_lookup(p,va)->burrow;
    int handles = burrow_handle_count(b);
    struct loom_buf_reg good = {va,PAGE_SIZE};
    PB_CHECK(loom_private_register_buffers(l,q,&good,1)==-T_E_ACCES,
             "another creator cannot register private buffers");
    u64 stripes = p->stripes;
    p->stripes = stripes + 1;
    int rc = loom_private_register_buffers(l,p,&good,1);
    p->stripes = stripes;
    PB_CHECK(rc==-T_E_ACCES, "creator incarnation checked independently of image");
    struct AddrSpace *as = p->as;
    p->as = q->as; // idle fixture only: simulate a different creator image
    rc = loom_private_register_buffers(l,p,&good,1);
    p->as = as;
    PB_CHECK(rc==-T_E_ACCES, "creator image checked independently of incarnation");
    PB_CHECK(!loom_private_register_buffers(l,p,&good,1) &&
             l->n_reg_buf==1 && l->reg_buf[0].burrow==b &&
             burrow_handle_count(b)==handles+1, "private table owns one real pin");
    struct loom_buf_reg bad_pair[2] = {good,{UINT64_MAX,2}};
    PB_CHECK(loom_private_register_buffers(l,p,bad_pair,2)==-T_E_INVAL &&
             l->n_reg_buf==1 && l->reg_buf[0].burrow==b &&
             burrow_handle_count(b)==handles+1,
             "partial resolution rollback preserves old table and exact pin count");
    ring_size = l->ring_size;
    PB_CHECK(!burrow_map(p,l->ring,LP_RING_VA,ring_size,VMA_PROT_RW),
             "ring mapped for self-buffer rejection");
    ring_va = LP_RING_VA;
    struct loom_buf_reg own_ring = {ring_va,ring_size};
    int ring_handles = burrow_handle_count(l->ring);
    PB_CHECK(loom_private_register_buffers(l,p,&own_ring,1)==-T_E_INVAL &&
             burrow_handle_count(l->ring)==ring_handles && l->reg_buf[0].burrow==b,
             "ring backing refused without retaining a pin");
    PB_CHECK(lp_detach_settling(p,ring_va,ring_size), "self-ring test mapping detached");
    ring_va = 0;
    struct loom_pool_extent extent = {b,0,64};
    spin_lock(&l->lock);
    rc = loom_service_pool_prepare(l->service_slots,l->service_pools,&extent,1,&pool);
    spin_unlock(&l->lock);
    PB_CHECK(!rc, "provisional pool consumes table backing");
    PB_CHECK(loom_private_register_buffers(l,p,&good,1)==-T_E_BUSY &&
             burrow_handle_count(b)==handles+1 && l->reg_buf[0].burrow==b,
             "provisional pool refuses rebinding and drops speculative pin");
    spin_lock(&l->lock);
    rc = loom_service_pool_publish(l->service_slots,l->service_pools,pool);
    spin_unlock(&l->lock);
    PB_CHECK(!rc && loom_private_register_buffers(l,p,NULL,0)==-T_E_BUSY &&
             l->n_reg_buf==1, "live pool refuses table clear");
    spin_lock(&l->lock);
    rc = loom_service_pool_reap(l->service_slots,l->service_pools,pool);
    spin_unlock(&l->lock);
    PB_CHECK(!rc, "pool reaped before backing table");
    pool.incarnation = 0;
    PB_CHECK(!sys_burrow_detach_for_proc(p,va,PAGE_SIZE), "private buffer unmapped");
    va = 0;
    PB_CHECK(p->as->page_count==owner_pages+burrow_backing_pages(PAGE_SIZE),
             "unmapped table pin retains backing charge");
    u64 destroyed = burrow_total_destroyed();
    PB_CHECK(!loom_private_register_buffers(l,p,NULL,0) && !l->n_reg_buf &&
             p->as->page_count==owner_pages && burrow_total_destroyed()==destroyed+1,
             "table clear frees and settles the final backing pin");
    // Foreign SHARED_IN backing must never refund this ring's image. q's
    // teardown owns its original mapping/charge; p holds only a borrowed pin.
    s64 foreign_va = sys_burrow_attach_for_proc(q,PAGE_SIZE);
    PB_CHECK(foreign_va>0, "foreign payer backing attached");
    struct Burrow *foreign = vma_lookup(q,(u64)foreign_va)->burrow;
    // SYS_BURROW_DETACH accepts the burrow window, not arbitrary low VAs.
    // Match the production share caller's destination-image lock contract.
    va = 0x140000000ull;
    spin_lock(&p->as->lock);
    rc = burrow_share_into(p,foreign,va,VMA_PROT_RW);
    spin_unlock(&p->as->lock);
    if (rc) va = 0;
    PB_CHECK(!rc, "foreign backing mapped into creator");
    good.va = va;
    PB_CHECK(!loom_private_register_buffers(l,p,&good,1), "foreign backing pinned");
    PB_CHECK(!sys_burrow_detach_for_proc(q,(u64)foreign_va,PAGE_SIZE) &&
             !sys_burrow_detach_for_proc(p,va,PAGE_SIZE), "foreign mappings detached");
    va = 0;
    destroyed = burrow_total_destroyed();
    PB_CHECK(!loom_private_register_buffers(l,p,NULL,0) &&
             burrow_total_destroyed()==destroyed+1 && p->as->page_count==owner_pages,
             "foreign final pin freed without refunding creator image");
    attached = sys_burrow_attach_for_proc(p,PAGE_SIZE);
    PB_CHECK(attached>0, "retirement buffer attached");
    va = (u64)attached;good.va = va;
    PB_CHECK(!loom_private_register_buffers(l,p,&good,1), "retirement buffer pinned");
    PB_CHECK(!sys_burrow_detach_for_proc(p,va,PAGE_SIZE), "retirement buffer unmapped");
    va = 0;
    loom_exec_latch(l);
    PB_CHECK(loom_private_register_buffers(l,p,NULL,0)==-T_E_CANCELED &&
             l->n_reg_buf==1, "close refuses rebinding and retains buffer for retirement");
done:
    if (l && pool.incarnation) {
        spin_lock(&l->lock);
        if (l->service_slots->slots[pool.slot].phase==LSLOT_PROVISIONAL)
            (void)loom_service_pool_rollback(l->service_slots,l->service_pools,pool);
        else (void)loom_service_pool_reap(l->service_slots,l->service_pools,pool);
        spin_unlock(&l->lock);
    }
    if (p && va) (void)sys_burrow_detach_for_proc(p,va,PAGE_SIZE);
    if (p && ring_va) (void)lp_detach_settling(p,ring_va,ring_size);
    if (l) loom_unref(l);
    if (!lp_wait(goal) && !error) error = "private buffer retirement timed out";
    if (p && !error && (p->as->page_count!=base || p->as->private_rings))
        error = "private buffer retirement refunds exact image";
    if (q) test_proc_drop(q);
    if (p) test_proc_drop(p);
#undef PB_CHECK
    return error;
}
// Canonical member aliases and real independent pins; no protocol/usercopy path.
static const char *loom_private_pool_pins_fixture(void) {
    const char *error = NULL;
    struct Proc *p = test_proc_make();
    struct Loom *l = NULL;
    struct loom_service_ref pool = {0}, again = {0};
    u64 goal = loom_private_retired(), va = 0;
    u32 baseline = 0;
    struct loom_service_pool_create in;
    for (u32 i = 0; i < sizeof(in); ++i) ((u8 *)&in)[i] = 0;
    in.size = sizeof(in); in.version = 1; in.count = 2;
#define PP_CHECK(c,msg) do { if (!(c)) { error = msg; goto done; } } while (0)
    PP_CHECK(p, "pool pin creator allocated");
    baseline = p->as->page_count;
    l = loom_create_private(p,2,2,true);
    if (l) goal++;
    PP_CHECK(l, "pool pin owner allocated");
    u32 owner_pages = p->as->page_count;
    s64 attached = sys_burrow_attach_for_proc(p,PAGE_SIZE);
    PP_CHECK(attached>0, "pool backing attached");
    va = (u64)attached;
    struct Burrow *b = vma_lookup(p,va)->burrow;
    int handles = burrow_handle_count(b);
    struct loom_buf_reg buffers[2] = {{va+128,1024},{va+192,512}};
    PP_CHECK(!loom_private_register_buffers(l,p,buffers,2) &&
             burrow_handle_count(b)==handles+2,"two table aliases pinned");
    in.members[0] = (struct loom_service_pool_member){0,0,64,64};
    in.members[1] = (struct loom_service_pool_member){1,0,0,64};
    PP_CHECK(loom_private_pool_prepare(l,p,&in,&pool)==-T_E_INVAL &&
             !pool.incarnation && burrow_handle_count(b)==handles+2 &&
             loom_service_pools_empty(l->service_pools),
             "different buffer indices cannot hide physical overlap");
    in.members[1].offset = 64;
    PP_CHECK(!loom_private_pool_prepare(l,p,&in,&pool) &&
             burrow_handle_count(b)==handles+4,"disjoint members acquire independent pins");
    struct loom_pool_cell *c = &l->service_pools->bank.cells[0];
    PP_CHECK(c->extent.backing==b && c->extent.offset==192 && c->extent.length==64,
             "canonical offset includes registered slice offset");
    struct loom_service_pool_create other;
    for (u32 i = 0; i < sizeof(other); ++i) ((u8 *)&other)[i] = 0;
    other.size=sizeof(other);other.version=1;other.count=1;
    other.members[0]=(struct loom_service_pool_member){1,0,0,64};
    PP_CHECK(loom_private_pool_prepare(l,p,&other,&again)==-T_E_BUSY &&
             !again.incarnation && burrow_handle_count(b)==handles+4,
             "provisional canonical extent excludes competing alias");
    PP_CHECK(loom_private_register_buffers(l,p,NULL,0)==-T_E_BUSY,
             "provisional pool holds table binding");
    struct loom_service_ref stale = pool;
    PP_CHECK(!loom_private_pool_rollback(l,pool) &&
             burrow_handle_count(b)==handles+2,"copyout rollback releases member pins");
    pool.incarnation=0;
    PP_CHECK(!loom_private_pool_prepare(l,p,&in,&pool) &&
             pool.incarnation!=stale.incarnation,"rollback burns pool identity");
    PP_CHECK(loom_private_pool_rollback(l,stale)==-T_E_NOENT &&
             burrow_handle_count(b)==handles+4,"stale rollback cannot release new pins");
    PP_CHECK(!loom_private_pool_publish(l,pool),"pool published after kernel continuation");
    spin_lock(&l->lock);
    int rc=loom_pool_stream_get(&l->service_pools->pools[pool.slot]);
    spin_unlock(&l->lock);
    if (!rc) {
        int refused=loom_private_pool_reap(l,pool);
        bool unchanged=burrow_handle_count(b)==handles+4;
        spin_lock(&l->lock);
        (void)loom_pool_stream_put(&l->service_pools->pools[pool.slot]);
        spin_unlock(&l->lock);
        PP_CHECK(refused==-T_E_BUSY && unchanged,"busy reap retains every pin");
    }
    PP_CHECK(!rc,"pool stream borrow acquired");
    PP_CHECK(!sys_burrow_detach_for_proc(p,va,PAGE_SIZE),"pooled backing unmapped");
    va=0;
    PP_CHECK(!loom_private_pool_reap(l,pool) &&
             burrow_handle_count(b)==handles+2,"reap drops member pins after unmap");
    pool.incarnation=0;
    u64 destroyed=burrow_total_destroyed();
    PP_CHECK(!loom_private_register_buffers(l,p,NULL,0) &&
             burrow_total_destroyed()==destroyed+1 && p->as->page_count==owner_pages,
             "final table pins free and refund backing exactly once");
    attached=sys_burrow_attach_for_proc(p,PAGE_SIZE);
    PP_CHECK(attached>0,"close-race backing attached");va=(u64)attached;
    buffers[0]=(struct loom_buf_reg){va,PAGE_SIZE};
    PP_CHECK(!loom_private_register_buffers(l,p,buffers,1),"close-race buffer pinned");
    in.count=1;in.members[1]=(struct loom_service_pool_member){0};
    PP_CHECK(!loom_private_pool_prepare(l,p,&in,&pool),"close-race pool prepared");
    loom_exec_latch(l);
    PP_CHECK(loom_private_pool_publish(l,pool)==-T_E_CANCELED,
             "close before publish refuses commit");
    PP_CHECK(!loom_private_pool_rollback(l,pool),"rollback remains available after close");
    pool.incarnation=0;
done:
    if (l && pool.incarnation) {
        if (l->service_slots->slots[pool.slot].phase==LSLOT_PROVISIONAL)
            (void)loom_private_pool_rollback(l,pool);
        else (void)loom_private_pool_reap(l,pool);
    }
    if (l && again.incarnation) (void)loom_private_pool_rollback(l,again);
    if (p && va) (void)sys_burrow_detach_for_proc(p,va,PAGE_SIZE);
    if (l) loom_unref(l);
    if (!lp_wait(goal) && !error) error="private pool pin retirement timed out";
    if (p && !error && (p->as->page_count!=baseline || p->as->private_rings))
        error="private pool pin retirement refunds creator";
    if (p) test_proc_drop(p);
#undef PP_CHECK
    return error;
}
static const char *loom_private_fixed_fixture(void) {
    const char *error = NULL;
    struct Proc *p = test_proc_make();
    struct Loom *l = NULL;
    struct loom_private_fixed first = {0}, second = {0}, extra = {0};
    struct loom_service_ref pool = {0};
    struct loom_service_pool_create in;
    for (u32 i=0; i<sizeof(in); ++i) ((u8 *)&in)[i]=0;
    in.size=sizeof(in);in.version=1;in.count=1;
    u64 goal=loom_private_retired(), va=0;
    u32 baseline=0;
#define FX_CHECK(c,msg) do { if (!(c)) { error=msg; goto done; } } while (0)
    FX_CHECK(p,"fixed creator allocated");baseline=p->as->page_count;
    l=loom_create_private(p,2,2,true);if(l)goal++;
    FX_CHECK(l,"fixed owner allocated");
    u32 owner_pages=p->as->page_count;
    s64 attached=sys_burrow_attach_for_proc(p,PAGE_SIZE);
    FX_CHECK(attached>0,"fixed backing attached");va=(u64)attached;
    struct Burrow *b=vma_lookup(p,va)->burrow;
    int handles=burrow_handle_count(b);
    struct loom_buf_reg bufs[2]={{va+128,1024},{va+192,512}};
    FX_CHECK(!loom_private_register_buffers(l,p,bufs,2),"fixed aliases registered");
    struct loom_service_pool_member fixed={0,0,64,64};
    FX_CHECK(!loom_private_fixed_acquire(l,p,&fixed,&first) &&
             burrow_handle_count(b)==handles+3 && l->service_fixed_count==1 &&
             l->refcount==2,"fixed reservation owns backing and ring");
    FX_CHECK(loom_private_fixed_acquire(l,p,&fixed,&first)==-T_E_BUSY &&
             burrow_handle_count(b)==handles+3,"duplicate token cannot acquire twice");
    in.members[0]=(struct loom_service_pool_member){1,0,0,64};
    FX_CHECK(loom_private_pool_prepare(l,p,&in,&pool)==-T_E_BUSY && !pool.incarnation,
             "fixed alias excludes new pool");
    in.members[0].offset=64;
    FX_CHECK(!loom_private_pool_prepare(l,p,&in,&pool),"touching nonoverlap pool accepted");
    fixed=(struct loom_service_pool_member){0,0,128,64};
    FX_CHECK(loom_private_fixed_acquire(l,p,&fixed,&second)==-T_E_BUSY && !second.owner,
             "provisional pool excludes new fixed alias");
    fixed.offset=192;
    FX_CHECK(!loom_private_fixed_acquire(l,p,&fixed,&second),"nonoverlap fixed accepted");
    FX_CHECK(loom_private_fixed_acquire(l,p,&fixed,&extra)==-T_E_AGAIN && !extra.owner,
             "fixed reservations bounded by existing CQ capacity");
    FX_CHECK(!loom_private_pool_rollback(l,pool),"provisional pool removed");pool.incarnation=0;
    FX_CHECK(!loom_private_register_buffers(l,p,NULL,0) &&
             burrow_handle_count(b)==handles+2,"rebind preserves accepted fixed pins");
    FX_CHECK(!sys_burrow_detach_for_proc(p,va,PAGE_SIZE),"fixed backing unmapped");va=0;
    FX_CHECK(p->as->page_count==owner_pages+burrow_backing_pages(PAGE_SIZE),
             "fixed pins retain unmapped backing charge");
    FX_CHECK(!loom_private_fixed_release(l,&first) && l->refcount==2 &&
             l->service_fixed_count==1,"first release drops one ring and backing pin");
    FX_CHECK(loom_private_fixed_release(l,&first)==-T_E_NOENT && l->refcount==2,
             "double release changes no ownership");
    loom_exec_latch(l);
    FX_CHECK(loom_private_fixed_acquire(l,p,&fixed,&extra)==-T_E_CANCELED,
             "close refuses fixed admission");
    // Last handle reference can leave while a local fixed borrower survives.
    // Its final release settles the backing before queuing owner retirement.
    struct Loom *borrowed=l;
    loom_unref(l);l=NULL;
    FX_CHECK(second.owner==borrowed && borrowed->refcount==1,
             "fixed borrower retains owner after handle drop");
    FX_CHECK(!loom_private_fixed_release(borrowed,&second),"final fixed release after close");
done:
    if (first.owner) (void)loom_private_fixed_release(first.owner,&first);
    if (second.owner) (void)loom_private_fixed_release(second.owner,&second);
    if (extra.owner) (void)loom_private_fixed_release(extra.owner,&extra);
    if (l && pool.incarnation) (void)loom_private_pool_rollback(l,pool);
    if (p && va) (void)sys_burrow_detach_for_proc(p,va,PAGE_SIZE);
    if (l) loom_unref(l);
    if (!lp_wait(goal) && !error) error="fixed owner retirement timed out";
    if (p && !error && (p->as->page_count!=baseline || p->as->private_rings))
        error="fixed retirement refunds exact image";
    if (p) test_proc_drop(p);
#undef FX_CHECK
    return error;
}
void test_loom_private_fixed(void) {
    const char *error=loom_private_fixed_fixture();
    TEST_ASSERT(error==NULL,error);
}

void test_loom_private_pool_pins(void) {
    const char *error = loom_private_pool_pins_fixture();
    TEST_ASSERT(error == NULL, error);
}

void test_loom_private_buffers(void) {
    const char *error = loom_private_buffers_fixture();
    TEST_ASSERT(error == NULL, error);
}

void test_loom_service_slots(void) {
    const char *slot_error = loom_slots_fixture();
    TEST_ASSERT(slot_error == NULL, slot_error);
}

// Its own test rather than a passenger on the geometry fixture: this one
// asserts the whole private-owner admission/retirement lifecycle, and a failure
// should name that instead of a ring-layout check.
void test_loom_private_owner_lifecycle(void) {
    const char *owner_error = loom_private_fixture();
    TEST_ASSERT(owner_error == NULL, owner_error);
}

void test_loom_create_geometry(void) {
    const char *receipt_error = loom_receipt_fixture();
    TEST_ASSERT(receipt_error == NULL, receipt_error);
    u64 created0 = loom_total_created();

    struct Loom *l = loom_create(8, 16, false);
    TEST_ASSERT(l != NULL, "loom_create(8,16) returned NULL");
    TEST_EXPECT_EQ(loom_total_created() - created0, (u64)1, "created counter +1");

    TEST_EXPECT_EQ(l->sq_entries, 8u, "sq_entries");
    TEST_EXPECT_EQ(l->cq_entries, 16u, "cq_entries");
    TEST_EXPECT_EQ(l->hdr_off, 0u, "hdr at offset 0");
    TEST_EXPECT_EQ(l->sq_array_size, 8u * 4u, "sq_array_size = entries*4");
    TEST_EXPECT_EQ(l->sqe_size, 8u * 64u, "sqe_size = entries*sizeof(sqe)");
    TEST_EXPECT_EQ(l->cqe_size, 16u * 16u, "cqe_size = cq_entries*sizeof(cqe)");

    // Regions 64-aligned + non-overlapping + within the page-rounded ring.
    TEST_ASSERT((l->sq_array_off & 63u) == 0, "sq_array_off 64-aligned");
    TEST_ASSERT((l->sqe_off & 63u) == 0, "sqe_off 64-aligned");
    TEST_ASSERT((l->cqe_off & 63u) == 0, "cqe_off 64-aligned");
    TEST_ASSERT(l->sq_array_off >= l->hdr_off + 64u, "sq_array after hdr");
    TEST_ASSERT(l->sqe_off >= l->sq_array_off + l->sq_array_size, "sqe after sq_array");
    TEST_ASSERT(l->cqe_off >= l->sqe_off + l->sqe_size, "cqe after sqe");
    TEST_ASSERT(l->ring_size >= l->cqe_off + l->cqe_size, "ring holds all regions");
    TEST_ASSERT((l->ring_size & (PAGE_SIZE - 1u)) == 0, "ring_size page-aligned");
    TEST_ASSERT(l->ring_kva != NULL, "ring_kva populated");

    // The header is stamped (read via the kernel direct map); heads/tails/flags
    // start zero (KP_ZERO).
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    TEST_EXPECT_EQ(h->sq_mask, 7u, "sq_mask = entries-1");
    TEST_EXPECT_EQ(h->sq_entries, 8u, "hdr sq_entries");
    TEST_EXPECT_EQ(h->cq_mask, 15u, "cq_mask = cq_entries-1");
    TEST_EXPECT_EQ(h->cq_entries, 16u, "hdr cq_entries");
    TEST_EXPECT_EQ(h->sq_head, 0u, "sq_head 0");
    TEST_EXPECT_EQ(h->sq_tail, 0u, "sq_tail 0");
    TEST_EXPECT_EQ(h->cq_head, 0u, "cq_head 0");
    TEST_EXPECT_EQ(h->cq_tail, 0u, "cq_tail 0");
    TEST_EXPECT_EQ(h->flags, 0u, "flags 0");
    TEST_EXPECT_EQ(h->dropped, 0u, "dropped 0");
    TEST_EXPECT_EQ(h->overflow, 0u, "overflow 0");

    loom_unref(l);
}

void test_loom_create_rejects_bad_args(void) {
    TEST_ASSERT(loom_create(0, 0, false) == NULL, "sq_entries 0 rejected");
    TEST_ASSERT(loom_create(3, 6, false) == NULL, "non-power-of-2 sq rejected");
    TEST_ASSERT(loom_create(LOOM_MAX_ENTRIES * 2u, LOOM_MAX_ENTRIES * 4u, false) == NULL,
                "sq over max rejected");
    TEST_ASSERT(loom_create(8, 4, false) == NULL, "cq < sq rejected");
    TEST_ASSERT(loom_create(8, 6, false) == NULL, "non-power-of-2 cq rejected");
}

void test_loom_refcount_lifecycle(void) {
    u64 destroyed0 = loom_total_destroyed();

    struct Loom *l = loom_create(4, 8, false);
    TEST_ASSERT(l != NULL, "loom_create(4,8) returned NULL");

    loom_ref(l);   // refcount 1 -> 2
    loom_unref(l); // 2 -> 1, NOT freed
    TEST_EXPECT_EQ(loom_total_destroyed() - destroyed0, (u64)0,
                   "not freed while a ref remains");

    loom_unref(l); // 1 -> 0, freed
    TEST_EXPECT_EQ(loom_total_destroyed() - destroyed0, (u64)1,
                   "freed when last ref drops");
}

// ---------------------------------------------------------------------------
// SYS_LOOM_SETUP via the testable inner: allocate + map the ring + install the
// handle on a fresh Proc.
// ---------------------------------------------------------------------------
void test_loom_setup_via_proc(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "proc_alloc returned NULL");

    u64 destroyed0 = loom_total_destroyed();

    struct loom_params kp;
    hidx_t fd = -1;
    int rc = sys_loom_setup_for_proc(p, 8, 0, &kp, &fd);
    TEST_EXPECT_EQ(rc, 0, "sys_loom_setup_for_proc succeeds");
    TEST_ASSERT(fd >= 0, "setup returned a valid fd");
    TEST_EXPECT_EQ(kp.sq_entries, 8u, "params sq_entries");
    TEST_EXPECT_EQ(kp.cq_entries, 16u, "params cq_entries (2x default)");
    TEST_ASSERT(kp.ring_va >= EXEC_USER_BURROW_BASE, "ring mapped in burrow window");
    TEST_ASSERT(kp.ring_va < EXEC_USER_BURROW_TOP, "ring below window top");
    TEST_ASSERT((kp.ring_size & (PAGE_SIZE - 1u)) == 0, "ring_size page-aligned");

    // The ring VMA is installed at ring_va.
    struct Vma *vma = vma_lookup(p, kp.ring_va);
    TEST_ASSERT(vma != NULL, "ring VMA installed at ring_va");

    // The handle is a KObj_Loom; its ring header is kernel-readable + stamped.
    struct Handle h;
    TEST_ASSERT(handle_get(p, fd, &h) == 0, "handle_get(loom fd) succeeds");
    TEST_EXPECT_EQ((int)h.kind, (int)KOBJ_LOOM, "handle is KOBJ_LOOM");
    struct Loom *l = (struct Loom *)h.obj;
    struct loom_ring_hdr *hdr = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    TEST_EXPECT_EQ(hdr->sq_entries, 8u, "ring header readable via direct map");
    handle_put(&h);

    // proc_free closes the handle (-> loom_unref frees the ring) + tears down
    // the VMA. The ring is destroyed exactly once.
    test_proc_drop(p);
    TEST_EXPECT_EQ(loom_total_destroyed() - destroyed0, (u64)1,
                   "ring freed on proc teardown");
}

void test_loom_setup_rejects(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "proc_alloc returned NULL");

    struct loom_params kp;
    hidx_t fd = -1;
    TEST_EXPECT_EQ(sys_loom_setup_for_proc(p, 0, 0, &kp, &fd), -1, "entries 0 rejected");
    TEST_EXPECT_EQ(sys_loom_setup_for_proc(p, 3, 0, &kp, &fd), -1, "non-pow2 rejected");
    TEST_EXPECT_EQ(sys_loom_setup_for_proc(p, LOOM_MAX_ENTRIES * 2u, 0, &kp, &fd), -1,
                   "over-max rejected");
    TEST_EXPECT_EQ(sys_loom_setup_for_proc(p, 8, LOOM_SETUP_CQSIZE, &kp, &fd), -1,
                   "still-reserved LOOM_SETUP_CQSIZE flag rejected");
    TEST_EXPECT_EQ(sys_loom_setup_for_proc(p, 8, 0x80000000u, &kp, &fd), -1,
                   "unknown high setup-flag bit rejected");
    TEST_EXPECT_EQ(sys_loom_setup_for_proc(NULL, 8, 0, &kp, &fd), -1, "NULL proc rejected");

    test_proc_drop(p);
}

// ---------------------------------------------------------------------------
// SYS_LOOM_REGISTER: install pipe Spoors into the fixed-handle table; the ring
// holds its own ref (the I-30 pin substrate); the rights snapshot is captured.
// ---------------------------------------------------------------------------
void test_loom_register_handles(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "proc_alloc returned NULL");

    struct loom_params kp;
    hidx_t loom_fd = -1;
    TEST_ASSERT(sys_loom_setup_for_proc(p, 8, 0, &kp, &loom_fd) == 0, "setup");

    hidx_t rd = -1, wr = -1;
    TEST_ASSERT(sys_pipe_for_proc(p, &rd, &wr) == 0, "pipe pair");

    hidx_t fds[2] = { rd, wr };
    int rc = sys_loom_register_for_proc(p, loom_fd, LOOM_REGISTER_HANDLES, fds, 2);
    TEST_EXPECT_EQ(rc, 0, "register 2 handles succeeds");

    // The ring holds the two Spoors with rights snapshots; slot 2 is empty.
    struct Handle h;
    TEST_ASSERT(handle_get(p, loom_fd, &h) == 0, "handle_get(loom)");
    struct Loom *l = (struct Loom *)h.obj;
    TEST_ASSERT(l->reg[0].spoor != NULL, "reg[0] populated");
    TEST_ASSERT(l->reg[1].spoor != NULL, "reg[1] populated");
    TEST_ASSERT(l->reg[2].spoor == NULL, "reg[2] empty");
    TEST_ASSERT(l->reg[0].rights != 0, "reg[0] rights snapshot non-empty");
    handle_put(&h);

    test_proc_drop(p);
}

void test_loom_register_rejects(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "proc_alloc returned NULL");

    struct loom_params kp;
    hidx_t loom_fd = -1;
    TEST_ASSERT(sys_loom_setup_for_proc(p, 8, 0, &kp, &loom_fd) == 0, "setup");

    hidx_t rd = -1, wr = -1;
    TEST_ASSERT(sys_pipe_for_proc(p, &rd, &wr) == 0, "pipe pair");

    hidx_t one[1] = { rd };
    // Unsupported op (BUFFERS reserved for Loom-6).
    TEST_EXPECT_EQ(sys_loom_register_for_proc(p, loom_fd, LOOM_REGISTER_BUFFERS, one, 1),
                   -1, "LOOM_REGISTER_BUFFERS rejected");
    // nargs over the table cap.
    TEST_EXPECT_EQ(sys_loom_register_for_proc(p, loom_fd, LOOM_REGISTER_HANDLES, one,
                                              LOOM_MAX_REG_HANDLES + 1u),
                   -1, "nargs over cap rejected");
    // A non-KOBJ_SPOOR fd (the loom fd itself) is rejected.
    hidx_t bad[1] = { loom_fd };
    TEST_EXPECT_EQ(sys_loom_register_for_proc(p, loom_fd, LOOM_REGISTER_HANDLES, bad, 1),
                   -1, "non-Spoor fd rejected");
    // A bogus loom_fd is rejected.
    TEST_EXPECT_EQ(sys_loom_register_for_proc(p, (hidx_t)999, LOOM_REGISTER_HANDLES, one, 1),
                   -1, "bad loom_fd rejected");

    test_proc_drop(p);
}

void test_loom_register_replaces(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "proc_alloc returned NULL");

    struct loom_params kp;
    hidx_t loom_fd = -1;
    TEST_ASSERT(sys_loom_setup_for_proc(p, 8, 0, &kp, &loom_fd) == 0, "setup");

    hidx_t rd = -1, wr = -1;
    TEST_ASSERT(sys_pipe_for_proc(p, &rd, &wr) == 0, "pipe pair");

    hidx_t two[2] = { rd, wr };
    TEST_ASSERT(sys_loom_register_for_proc(p, loom_fd, LOOM_REGISTER_HANDLES, two, 2) == 0,
                "register 2");

    // Re-register a single handle -- the whole table is replaced
    // (IORING_REGISTER_FILES semantics); slot 1 is cleared (its old Spoor
    // clunked).
    hidx_t one[1] = { rd };
    TEST_ASSERT(sys_loom_register_for_proc(p, loom_fd, LOOM_REGISTER_HANDLES, one, 1) == 0,
                "re-register 1");

    struct Handle h;
    TEST_ASSERT(handle_get(p, loom_fd, &h) == 0, "handle_get(loom)");
    struct Loom *l = (struct Loom *)h.obj;
    TEST_ASSERT(l->reg[0].spoor != NULL, "reg[0] still populated");
    TEST_ASSERT(l->reg[1].spoor == NULL, "reg[1] cleared after replace");
    handle_put(&h);

    // Clearing the table (n = 0) releases all.
    TEST_ASSERT(sys_loom_register_for_proc(p, loom_fd, LOOM_REGISTER_HANDLES, NULL, 0) == 0,
                "clear table");
    TEST_ASSERT(handle_get(p, loom_fd, &h) == 0, "handle_get(loom) again");
    l = (struct Loom *)h.obj;
    TEST_ASSERT(l->reg[0].spoor == NULL, "reg[0] cleared after empty register");
    handle_put(&h);

    test_proc_drop(p);
}

// ---------------------------------------------------------------------------
// SYS_LOOM_REGISTER(LOOM_REGISTER_BUFFERS) (Loom-6): a VA range resolves to its
// backing anon Burrow, which the table pins (an independent burrow_ref -- the
// I-30 buffer-pin substrate), exposing its contiguous direct-map slice for
// zero-copy payload. Mirrors specs/loom.tla's `reg` slot + ObjPinnedToSnapshot.
// ---------------------------------------------------------------------------
#include "private_pool_fixture.h"

void test_loom_register_buffers(void) {
    const char *pool_error = private_pool_fixture_run();
    TEST_ASSERT(pool_error == NULL, pool_error);
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "proc_alloc");

    struct loom_params kp;
    hidx_t loom_fd = -1;
    TEST_ASSERT(sys_loom_setup_for_proc(p, 8, 0, &kp, &loom_fd) == 0, "setup");

    s64 bva = sys_burrow_attach_for_proc(p, PAGE_SIZE);
    TEST_ASSERT(bva > 0, "burrow-attach a buffer page");
    struct Vma *bvma = vma_lookup(p, (u64)bva);
    TEST_ASSERT(bvma != NULL && bvma->burrow != NULL, "buffer VMA + Burrow");
    int hc0 = burrow_handle_count(bvma->burrow);

    struct loom_buf_reg br = { .va = (u64)bva, .len = PAGE_SIZE };
    TEST_EXPECT_EQ(sys_loom_register_buffers_for_proc(p, loom_fd, &br, 1), 0,
                   "register one buffer");

    struct Handle h;
    TEST_ASSERT(handle_get(p, loom_fd, &h) == 0, "handle_get(loom)");
    struct Loom *l = (struct Loom *)h.obj;
    TEST_EXPECT_EQ((u64)l->n_reg_buf, (u64)1, "n_reg_buf == 1");
    TEST_ASSERT(l->reg_buf[0].burrow == bvma->burrow, "slot 0 -> the buffer Burrow");
    TEST_EXPECT_EQ((u64)l->reg_buf[0].len, (u64)PAGE_SIZE, "slot 0 len");
    TEST_ASSERT(l->reg_buf[0].kva != NULL, "slot 0 kva set");
    TEST_ASSERT(l->reg_buf[1].burrow == NULL, "slot 1 empty");
    handle_put(&h);

    // The table holds its OWN handle_count ref (the pin) on top of the mapping.
    TEST_EXPECT_EQ(burrow_handle_count(bvma->burrow), hc0 + 1, "table holds its own ref");

    test_proc_drop(p);   // clears the table -> drops the pin; frees everything
}

void test_loom_register_buffers_rejects(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "proc_alloc");

    struct loom_params kp;
    hidx_t loom_fd = -1;
    TEST_ASSERT(sys_loom_setup_for_proc(p, 8, 0, &kp, &loom_fd) == 0, "setup");

    s64 bva = sys_burrow_attach_for_proc(p, PAGE_SIZE);
    TEST_ASSERT(bva > 0, "buffer page");
    int hc0 = burrow_handle_count(vma_lookup(p, (u64)bva)->burrow);   // baseline (just the mapping)

    struct loom_buf_reg one  = { .va = (u64)bva, .len = PAGE_SIZE };
    struct loom_buf_reg zlen = { .va = (u64)bva, .len = 0 };
    struct loom_buf_reg oob  = { .va = (u64)bva, .len = 2u * PAGE_SIZE };  // region is 1 page
    struct loom_buf_reg um   = { .va = 0x1000,   .len = PAGE_SIZE };       // unmapped low VA

    TEST_EXPECT_EQ(sys_loom_register_buffers_for_proc(p, loom_fd, &one,
                   LOOM_MAX_REG_BUFFERS + 1u), -1, "n over cap rejected");
    TEST_EXPECT_EQ(sys_loom_register_buffers_for_proc(p, loom_fd, &zlen, 1), -1, "len 0 rejected");
    TEST_EXPECT_EQ(sys_loom_register_buffers_for_proc(p, loom_fd, &oob, 1), -1, "OOB len rejected");
    TEST_EXPECT_EQ(sys_loom_register_buffers_for_proc(p, loom_fd, &um, 1), -1, "unmapped VA rejected");
    TEST_EXPECT_EQ(sys_loom_register_buffers_for_proc(p, (hidx_t)999, &one, 1), -1, "bad loom_fd rejected");

    // Every reject is atomic: the table stays empty + no ref leaked.
    struct Handle h;
    TEST_ASSERT(handle_get(p, loom_fd, &h) == 0, "handle_get(loom)");
    struct Loom *l = (struct Loom *)h.obj;
    TEST_EXPECT_EQ((u64)l->n_reg_buf, (u64)0, "table untouched by rejects");
    TEST_ASSERT(l->reg_buf[0].burrow == NULL, "slot 0 still empty");
    handle_put(&h);
    TEST_EXPECT_EQ(burrow_handle_count(vma_lookup(p, (u64)bva)->burrow), hc0,
                   "no ref leaked into the buffer Burrow by the rejected attempts");

    test_proc_drop(p);
}

void test_loom_register_buffers_replace(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "proc_alloc");

    struct loom_params kp;
    hidx_t loom_fd = -1;
    TEST_ASSERT(sys_loom_setup_for_proc(p, 8, 0, &kp, &loom_fd) == 0, "setup");

    s64 bva = sys_burrow_attach_for_proc(p, PAGE_SIZE);
    TEST_ASSERT(bva > 0, "buffer page");
    struct Burrow *b = vma_lookup(p, (u64)bva)->burrow;
    int hc0 = burrow_handle_count(b);

    struct loom_buf_reg br = { .va = (u64)bva, .len = PAGE_SIZE };
    TEST_ASSERT(sys_loom_register_buffers_for_proc(p, loom_fd, &br, 1) == 0, "register");
    TEST_EXPECT_EQ(burrow_handle_count(b), hc0 + 1, "one pin after register");

    // Re-register the SAME region: the prior pin is released, a fresh one taken
    // (net unchanged -- IORING_REGISTER_BUFFERS replace semantics).
    TEST_ASSERT(sys_loom_register_buffers_for_proc(p, loom_fd, &br, 1) == 0, "re-register");
    TEST_EXPECT_EQ(burrow_handle_count(b), hc0 + 1, "still one pin (replace freed the old)");

    // Clear (n == 0) releases the pin.
    TEST_ASSERT(sys_loom_register_buffers_for_proc(p, loom_fd, NULL, 0) == 0, "clear");
    TEST_EXPECT_EQ(burrow_handle_count(b), hc0, "pin released on clear");

    struct Handle h;
    TEST_ASSERT(handle_get(p, loom_fd, &h) == 0, "handle_get(loom)");
    struct Loom *l = (struct Loom *)h.obj;
    TEST_EXPECT_EQ((u64)l->n_reg_buf, (u64)0, "n_reg_buf 0 after clear");
    TEST_ASSERT(l->reg_buf[0].burrow == NULL, "slot 0 cleared");
    handle_put(&h);

    test_proc_drop(p);
}

// ---------------------------------------------------------------------------
// loom_post_cqe back-pressure (Loom-2b): the POST_CQE front-end's CQ writer
// never overwrites an unreaped completion (CqNeverOverfull, I-29). A full CQ
// refuses the post (-1) + bumps the overflow counter; a user-side reap frees
// slots so later posts succeed (wrapping). Maps to specs/loom.tla PostCqe's
// `Cardinality(cq) < CQ_CAP` guard + CqNeverOverfull.
// ---------------------------------------------------------------------------
void test_loom_post_cqe_back_pressure(void) {
    struct Loom *l = loom_create(4, 4, false);   // cq_entries = 4 (smallest exercising wrap)
    TEST_ASSERT(l != NULL, "loom_create(4,4)");
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    // Fill the CQ to capacity: every post within capacity succeeds.
    for (u32 i = 0; i < 4; i++) {
        TEST_EXPECT_EQ(loom_post_cqe(l, (u64)(0x1000u + i), (s32)i, 0), 0,
                       "post within capacity succeeds");
    }
    TEST_EXPECT_EQ((u64)h->cq_tail, (u64)4, "cq_tail = 4 (full)");
    TEST_EXPECT_EQ((u64)h->overflow, (u64)0, "no overflow while posts fit");

    // Full CQ: the post is REFUSED, overflow bumped, and -- critically -- no
    // staged CQE is overwritten (CqNeverOverfull).
    TEST_EXPECT_EQ(loom_post_cqe(l, 0x9999u, 42, 0), -1, "post into full CQ refused");
    TEST_EXPECT_EQ((u64)h->cq_tail, (u64)4, "cq_tail unchanged (no overwrite)");
    TEST_EXPECT_EQ((u64)h->overflow, (u64)1, "overflow counter bumped");
    for (u32 i = 0; i < 4; i++) {
        TEST_EXPECT_EQ(cqes[i].user_data, (u64)(0x1000u + i), "staged CQE intact");
        TEST_EXPECT_EQ((u64)(u32)cqes[i].result, (u64)i, "staged CQE result intact");
    }

    // Userspace reaps 2 (advances cq_head) -> 2 slots free -> 2 more posts land
    // in the wrapped slots; the 3rd is refused again.
    __atomic_store_n(&h->cq_head, 2u, __ATOMIC_RELEASE);
    TEST_EXPECT_EQ(loom_post_cqe(l, 0x2000u, 7, 0), 0, "post after reap succeeds");
    TEST_EXPECT_EQ(loom_post_cqe(l, 0x2001u, 8, 0), 0, "second post after reap succeeds");
    TEST_EXPECT_EQ(loom_post_cqe(l, 0x2002u, 9, 0), -1, "CQ full again");
    TEST_EXPECT_EQ((u64)h->cq_tail, (u64)6, "two more posted (tail 4 -> 6)");
    TEST_EXPECT_EQ((u64)h->overflow, (u64)2, "overflow bumped once more");
    // tail 4 -> slot (4 & 3) = 0; tail 5 -> slot (5 & 3) = 1.
    TEST_EXPECT_EQ(cqes[4u & h->cq_mask].user_data, (u64)0x2000u, "wrapped slot holds new CQE");
    TEST_EXPECT_EQ(cqes[5u & h->cq_mask].user_data, (u64)0x2001u, "wrapped slot holds new CQE");

    loom_unref(l);
}

// SA-1 regression: loom_post_cqe must compute its write index from KERNEL-PRIVATE
// geometry, never from the shared (userspace-writable) ring header. A hostile
// cq_mask + cq_tail would otherwise drive an out-of-bounds kernel write (a
// Loom-3 trap, since the ring is userspace-controlled there). Here we corrupt
// the shared header and assert the CQE still lands at the kernel-private index.
void test_loom_post_cqe_ignores_hostile_header(void) {
    struct Loom *l = loom_create(4, 4, false);
    TEST_ASSERT(l != NULL, "loom_create(4,4)");
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    // Simulate a hostile / corrupt userspace mutation of the shared control words.
    __atomic_store_n(&h->cq_mask, 0xFFFFFFFFu, __ATOMIC_RELEASE);
    __atomic_store_n(&h->cq_tail, 0x10000u, __ATOMIC_RELEASE);   // would be idx 0x10000 if trusted

    int rc = loom_post_cqe(l, 0xABCDu, 7, 0);
    TEST_EXPECT_EQ(rc, 0, "post succeeds on kernel-private geometry");
    // The CQE landed at the kernel-private index (priv cq_tail 0 & (cq_entries-1) = 0),
    // NOT at the hostile (0x10000 & 0xFFFFFFFF) which would be far out of bounds.
    TEST_EXPECT_EQ(cqes[0].user_data, (u64)0xABCDu, "CQE at kernel-private idx 0 (not the hostile idx)");
    // The kernel mirrored its own private tail (1), overwriting the hostile value.
    TEST_EXPECT_EQ((u64)h->cq_tail, (u64)1, "kernel mirrored its private tail (1), clobbering the hostile cq_tail");

    loom_unref(l);
}

// ---------------------------------------------------------------------------
// Loom-3 SYS_LOOM_ENTER: the SQ consume + SQE dispatch + the inline-completion
// paths (no 9P engine needed). The engine-driven FSYNC dispatch + the
// submit-time rights pin + the #898 quiesce live in test_9p_client.c (they need
// the loopback client harness). Maps to specs/loom.tla Consume + Dispatch +
// PostCqe (the inline branch) + the SQ-index ring TOCTOU bounding.
// ---------------------------------------------------------------------------

// Stage one SQE into the ring at `slot` (identity SQ-index indirection: the SQ
// ring slot `slot` points at SQE `slot`). Writes via the kernel direct map.
static void loom_stage_sqe(struct Loom *l, u32 slot, u8 opcode, u8 flags,
                           u32 handle_idx, u32 len, u64 user_data) {
    struct loom_sqe *sqes = (struct loom_sqe *)(l->ring_kva + l->sqe_off);
    u32 *sqa = (u32 *)(l->ring_kva + l->sq_array_off);
    struct loom_sqe *s = &sqes[slot];
    for (u32 i = 0; i < sizeof(*s); i++) ((u8 *)s)[i] = 0;
    s->opcode     = opcode;
    s->flags      = flags;
    s->handle_idx = handle_idx;
    s->len        = len;
    s->user_data  = user_data;
    sqa[slot] = slot;
}

// NOP: the io_uring smoke op completes inline with result 0 (no engine, no
// handle). Also pins the SQ consume + the kernel-private sq_head mirror.
void test_loom_enter_nop(void) {
    struct Loom *l = loom_create(8, 16, false);
    TEST_ASSERT(l != NULL, "loom_create(8,16)");
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    loom_stage_sqe(l, 0, LOOM_OP_NOP, 0, 0, 0, 0x4E4F50ULL /* "NOP" */);
    __atomic_store_n(&h->sq_tail, 1u, __ATOMIC_RELEASE);

    int n = loom_enter(l, 1, 1, 0);
    TEST_EXPECT_EQ(n, 1, "one SQE consumed");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)1, "NOP completes inline -> 1 CQE");
    TEST_EXPECT_EQ(cqes[0].user_data, (u64)0x4E4F50ULL, "CQE user_data echoed");
    TEST_EXPECT_EQ((u64)(s64)cqes[0].result, (u64)0, "NOP result 0");
    TEST_EXPECT_EQ((u64)h->sq_head, (u64)1, "sq_head advanced + mirrored to the shared header");
    TEST_EXPECT_EQ((u64)l->sq_head, (u64)1, "kernel-private sq_head advanced");
    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)0, "NOP never goes in flight");

    loom_unref(l);
}

// The submit-time reject paths each post an error CQE (the SQE is still consumed
// -- io_uring semantics), never a dropped/lost SQE: a bad handle_idx (-EINVAL),
// an empty registered slot (-EBADF), a still-unimplemented-but-in-range opcode
// (-ENOSYS -- the direct-descriptor seam #916), and an out-of-range opcode (-EINVAL).
void test_loom_enter_submit_rejects(void) {
    struct Loom *l = loom_create(8, 16, false);
    TEST_ASSERT(l != NULL, "loom_create(8,16)");
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    loom_stage_sqe(l, 0, LOOM_OP_FSYNC,   0, 99u, 0, 0xAAu);  // handle_idx out of range
    loom_stage_sqe(l, 1, LOOM_OP_FSYNC,   0, 0u,  0, 0xBBu);  // reg[0] empty (no register call)
    loom_stage_sqe(l, 2, LOOM_OP_WALK,    0, 0u,  0, 0xCCu);  // in-range, unimplemented (direct-descriptor #916)
    loom_stage_sqe(l, 3, LOOM_OP_COUNT,   0, 0u,  0, 0xDDu);  // out-of-range opcode
    __atomic_store_n(&h->sq_tail, 4u, __ATOMIC_RELEASE);

    int n = loom_enter(l, 4, 0, LOOM_ENTER_NONBLOCK);
    TEST_EXPECT_EQ(n, 4, "four SQEs consumed (none dropped)");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)4, "four error CQEs posted");
    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)0, "no op dispatched (all rejected at submit)");
    // CQEs land in submit order (cq slot i = SQE i for the first pass).
    TEST_EXPECT_EQ(cqes[0].user_data, (u64)0xAAu, "cqe0 echoed");
    TEST_EXPECT_EQ((u64)(s64)cqes[0].result, (u64)(s64)(-(s32)T_E_INVAL), "bad handle_idx -> -EINVAL");
    TEST_EXPECT_EQ((u64)(s64)cqes[1].result, (u64)(s64)(-(s32)T_E_BADF),  "empty reg slot -> -EBADF");
    TEST_EXPECT_EQ((u64)(s64)cqes[2].result, (u64)(s64)(-(s32)T_E_NOSYS), "unimplemented opcode -> -ENOSYS");
    TEST_EXPECT_EQ((u64)(s64)cqes[3].result, (u64)(s64)(-(s32)T_E_INVAL), "out-of-range opcode -> -EINVAL");

    loom_unref(l);
}

// A still-reserved SQE flag is rejected (-EINVAL) so a future flag is never
// silently ignored; an out-of-range SQ-index indirection is DROPPED (the
// diagnostic counter bumps, the SQE array is never indexed out of bounds -- the
// SA-1 discipline on the SQ side). LINK/DRAIN/MULTISHOT are now IMPLEMENTED
// (Loom-5), so CQE_SKIP is the remaining reserved flag (deferred out of Loom-5b
// -- it suppresses a success CQE, which needs a loom_order.tla carve-out first).
void test_loom_enter_flags_and_bad_index(void) {
    struct Loom *l = loom_create(8, 16, false);
    TEST_ASSERT(l != NULL, "loom_create(8,16)");
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);
    u32 *sqa = (u32 *)(l->ring_kva + l->sq_array_off);

    loom_stage_sqe(l, 0, LOOM_OP_NOP, LOOM_SQE_CQE_SKIP, 0, 0, 0x11u);  // reserved flag set
    loom_stage_sqe(l, 1, LOOM_OP_NOP, 0, 0, 0, 0x22u);
    sqa[1] = 999u;   // out-of-range SQ-index indirection -> dropped, never indexes sqes[]
    __atomic_store_n(&h->sq_tail, 2u, __ATOMIC_RELEASE);

    int n = loom_enter(l, 2, 0, LOOM_ENTER_NONBLOCK);
    TEST_EXPECT_EQ(n, 1, "one SQE consumed (the flag-reject); the bad-index one was dropped");
    TEST_EXPECT_EQ((u64)h->dropped, (u64)1, "bad SQ indirection counted as dropped");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)1, "one CQE (the reserved-flag reject)");
    TEST_EXPECT_EQ((u64)(s64)cqes[0].result, (u64)(s64)(-(s32)T_E_INVAL), "reserved flag -> -EINVAL");
    TEST_EXPECT_EQ((u64)h->sq_head, (u64)2, "sq_head consumed both ring slots");

    loom_unref(l);
}

// F2 regression: submit-time CQ admission back-pressures (never silently drops a
// completion -- I-29). Fill the CQ via inline NOPs without reaping; a further
// enter with work available must consume 0 (admission blocks) and NOT bump
// overflow; after reaping, a re-enter consumes again. Maps to specs/loom.tla's
// "an admitted op always reaches a CQE" -- the impl back-pressures at submit
// instead of dropping at completion.
void test_loom_enter_cq_admission_backpressure(void) {
    struct Loom *l = loom_create(4, 4, false);   // sq 4, cq 4 (cq == sq exercises the cap fast)
    TEST_ASSERT(l != NULL, "loom_create(4,4)");
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);

    // Stage 4 NOPs (slots 0..3); enter consumes all 4 (cq_ready 0..3 < cq_entries
    // 4 at each admission check) -> CQ now full (cq_ready == 4).
    for (u32 i = 0; i < 4; i++) loom_stage_sqe(l, i, LOOM_OP_NOP, 0, 0, 0, 0x100u + i);
    __atomic_store_n(&h->sq_tail, 4u, __ATOMIC_RELEASE);
    int n1 = loom_enter(l, 4, 0, LOOM_ENTER_NONBLOCK);
    TEST_EXPECT_EQ(n1, 4, "first enter consumes all 4 NOPs");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)4, "CQ full (4 CQEs)");
    TEST_EXPECT_EQ((u64)h->overflow, (u64)0, "no overflow");

    // More work available (re-bump sq_tail to 8, reusing the ring slots), but the
    // CQ is full -> admission blocks BEFORE consuming -> 0 submitted, 0 dropped,
    // 0 overflow (the completion is NOT lost -- the SQE waits for the next enter).
    for (u32 i = 0; i < 4; i++) loom_stage_sqe(l, i, LOOM_OP_NOP, 0, 0, 0, 0x200u + i);
    __atomic_store_n(&h->sq_tail, 8u, __ATOMIC_RELEASE);
    int n2 = loom_enter(l, 4, 0, LOOM_ENTER_NONBLOCK);
    TEST_EXPECT_EQ(n2, 0, "second enter consumes 0 (CQ-full admission back-pressure)");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)4, "cq_tail unchanged (no completion posted/dropped)");
    TEST_EXPECT_EQ((u64)h->overflow, (u64)0, "no overflow -- back-pressured at submit, not dropped");
    TEST_EXPECT_EQ((u64)h->sq_head, (u64)4, "sq_head unchanged (no SQE consumed)");

    // Reap all 4 (advance cq_head) -> CQ drains -> a re-enter consumes the waiting 4.
    __atomic_store_n(&h->cq_head, 4u, __ATOMIC_RELEASE);
    int n3 = loom_enter(l, 4, 0, LOOM_ENTER_NONBLOCK);
    TEST_EXPECT_EQ(n3, 4, "after reap, the waiting SQEs are consumed");
    TEST_EXPECT_EQ((u64)h->overflow, (u64)0, "still no overflow across the whole sequence");

    loom_unref(l);
}

// Exact concurrent admission (the Loom-5 audit F2 residual, closed with LOOM.md
// 8.5.1). A sibling driver between its consume and its disposition holds its CQ
// slot in `admitting`, so this driver's admission counts it -- simulated white-box
// here: siblings holding three of a cq-4 ring's slots leave room for exactly ONE
// consume, not four. And every consume path gives its reservation back: after a
// batch that runs the fast path, a HELD drain entry admitted later, and a LINK
// whose failed head cancels its successor, `admitting` is 0 again.
void test_loom_admission_counts_admitting(void) {
    struct Loom *l = loom_create(4, 4, false);
    TEST_ASSERT(l != NULL, "loom_create(4,4)");
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);

    spin_lock(&l->lock);
    l->admitting = 3;                                 // three siblings mid-dispatch
    spin_unlock(&l->lock);
    for (u32 i = 0; i < 4; i++) loom_stage_sqe(l, i, LOOM_OP_NOP, 0, 0, 0, 0x300u + i);
    __atomic_store_n(&h->sq_tail, 4u, __ATOMIC_RELEASE);
    int n1 = loom_enter(l, 4, 0, LOOM_ENTER_NONBLOCK);
    TEST_EXPECT_EQ(n1, 1, "one consume fits beside three admitting siblings");
    TEST_EXPECT_EQ((u64)h->overflow, (u64)0, "no completion dropped");
    spin_lock(&l->lock);
    TEST_EXPECT_EQ((u64)l->admitting, (u64)3, "this driver returned its own reservation");
    l->admitting = 0;                                 // the siblings' dispositions land
    spin_unlock(&l->lock);

    __atomic_store_n(&h->cq_head, 1u, __ATOMIC_RELEASE);
    int n2 = loom_enter(l, 4, 0, LOOM_ENTER_NONBLOCK);
    TEST_EXPECT_EQ(n2, 3, "the siblings gone, the waiting three are consumed");
    TEST_EXPECT_EQ((u64)l->admitting, (u64)0, "fast path: every reservation returned");
    __atomic_store_n(&h->cq_head, 4u, __ATOMIC_RELEASE);

    // A DRAIN entry goes HELD at consume (its reservation returned there) and is
    // re-reserved when the admit pass dispatches it.
    loom_stage_sqe(l, 0, LOOM_OP_NOP, LOOM_SQE_DRAIN, 0, 0, 0x400u);
    __atomic_store_n(&h->sq_tail, 5u, __ATOMIC_RELEASE);
    TEST_EXPECT_EQ(loom_enter(l, 1, 0, LOOM_ENTER_NONBLOCK), 1, "drain entry consumed");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)5, "the drain entry was admitted and completed");
    TEST_EXPECT_EQ((u64)l->admitting, (u64)0, "chain dispatch: reservation returned");

    // A LINK head that fails inline (a READ on an empty handle slot) cancels its
    // successor: the cancel path reserves and returns too.
    loom_stage_sqe(l, 1, LOOM_OP_READ, LOOM_SQE_LINK, 0, 8, 0x500u);
    loom_stage_sqe(l, 2, LOOM_OP_NOP, 0, 0, 0, 0x501u);
    __atomic_store_n(&h->sq_tail, 7u, __ATOMIC_RELEASE);
    TEST_EXPECT_EQ(loom_enter(l, 2, 0, LOOM_ENTER_NONBLOCK), 2, "link pair consumed");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)7, "failed head + cancelled successor both posted");
    TEST_EXPECT_EQ((u64)l->admitting, (u64)0, "cancel path: reservation returned");
    TEST_EXPECT_EQ((u64)h->overflow, (u64)0, "no completion dropped anywhere");

    loom_unref(l);
}

// A DRAIN waits for a sibling mid-dispatch (LOOM.md 8.5.1, round-2 audit F2;
// loom_order.tla DrainOrdered). An op another driver consumed and has not disposed
// of is neither posted nor in flight -- only `admitting` sees it, and 8.5.1 made
// that window a parent-stat RPC wide. The bare admit pass after the release is the
// control: the drain held, not stuck.
void test_loom_drain_waits_for_admitting(void) {
    struct Loom *l = loom_create(4, 4, false);
    TEST_ASSERT(l != NULL, "loom_create(4,4)");
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);

    spin_lock(&l->lock);
    l->admitting = 1;                                 // a sibling, mid-submit
    spin_unlock(&l->lock);
    loom_stage_sqe(l, 0, LOOM_OP_NOP, LOOM_SQE_DRAIN, 0, 0, 0x600u);
    __atomic_store_n(&h->sq_tail, 1u, __ATOMIC_RELEASE);
    TEST_EXPECT_EQ(loom_enter(l, 1, 0, LOOM_ENTER_NONBLOCK), 1, "drain entry consumed");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)0, "the drain held behind the mid-submit sibling");
    TEST_ASSERT(l->chain != NULL, "the drain is still in the chain");

    spin_lock(&l->lock);
    l->admitting = 0;                                 // the sibling's disposition lands
    spin_unlock(&l->lock);
    TEST_EXPECT_EQ(loom_enter(l, 0, 0, LOOM_ENTER_NONBLOCK), 0, "a bare admit pass");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)1, "the sibling disposed of, the drain runs");
    TEST_EXPECT_EQ(cqes[0].user_data, 0x600u, "the drain's own CQE");
    TEST_EXPECT_EQ((u64)h->overflow, (u64)0, "no completion dropped");
    loom_unref(l);
}

// SA-2: KOBJ_LOOM is non-transferable, so handle_dup must reject a loom fd (the
// same gate as the hardware / srv kinds). Pins the non-dup-ability explicitly.
void test_loom_dup_rejected(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "proc_alloc returned NULL");

    struct loom_params kp;
    hidx_t fd = -1;
    TEST_ASSERT(sys_loom_setup_for_proc(p, 8, 0, &kp, &fd) == 0, "setup");

    hidx_t dup = handle_dup(p, fd, RIGHT_READ | RIGHT_WRITE);
    TEST_EXPECT_EQ((int)dup, -1, "handle_dup(loom_fd) rejected (KOBJ_LOOM non-transferable)");

    test_proc_drop(p);
}

// ---------------------------------------------------------------------------
// Loom-4b: the CQ wait-list. A SYS_LOOM_ENTER thread that finds a sibling
// already holding the reader role installs a poll_waiter here and sleeps;
// loom_post_cqe wakes it after publishing a CQE. These tests pin the wake
// MECHANISM + the new wait-phase early-outs deterministically (white-box).
// The full sleep -> woken-by-a-real-sibling-reader interleaving needs a second
// kernel thread + the loopback client and lands with the deferred multi-in-
// flight / cross-Proc-death SMP harness (shared with #841/#907). Maps to
// specs/loom.tla: PostCqe-wake (NoMissedCqWake, I-9) + CqWaitFlagSound +
// CqWaitCommitOrSleep's give-up arms.
// ---------------------------------------------------------------------------

// PostCqe-wake (NoMissedCqWake): a successful CQE post sets the registered
// CQ-waiter's wake flag, so a thread sleeping in loom_wait_for_completions
// re-evaluates. poll_waiter_list_wake writes pw->ready before signalling, so
// the flag is the cross-lock hand-off loom_cqw_cond reads under the rendez lock.
void test_loom_cq_waiter_wake(void) {
    struct Loom *l = loom_create(4, 8, false);
    TEST_ASSERT(l != NULL, "loom_create(4,8)");

    struct Rendez r;
    rendez_init(&r);
    struct poll_waiter pw;
    poll_waiter_init(&pw, &r);

    // Register-then-observe install on the ring's CQ wait-list.
    poll_waiter_list_register(&l->cq_waiters, &pw);
    TEST_ASSERT(pw.ready == false, "fresh CQ-waiter not ready before any post");

    // A successful post wakes the registered waiter.
    TEST_EXPECT_EQ(loom_post_cqe(l, 0xCAFEu, 0, 0), 0, "CQE post succeeds");
    TEST_ASSERT(pw.ready == true, "post sets the registered CQ-waiter's wake flag");

    poll_waiter_list_unregister(&pw);
    pw.magic = 0;
    loom_unref(l);
}

// CqWaitFlagSound: a REFUSED post (CQ full -> no completion became available)
// must NOT set the wake flag -- a sleeping waiter is not spuriously woken to
// re-sample an unchanged (already-full) CQ. (The wake lives only on the
// success path, after the cq_tail bump; the CQ-full path returns -1 first.)
void test_loom_cq_waiter_no_spurious_wake_on_full(void) {
    struct Loom *l = loom_create(4, 4, false);   // cq_entries = 4
    TEST_ASSERT(l != NULL, "loom_create(4,4)");

    for (u32 i = 0; i < 4; i++) {
        TEST_EXPECT_EQ(loom_post_cqe(l, 0x10u + i, 0, 0), 0, "fill CQ");
    }

    struct Rendez r;
    rendez_init(&r);
    struct poll_waiter pw;
    poll_waiter_init(&pw, &r);
    poll_waiter_list_register(&l->cq_waiters, &pw);

    TEST_EXPECT_EQ(loom_post_cqe(l, 0x99u, 0, 0), -1, "post refused (CQ full)");
    TEST_ASSERT(pw.ready == false, "a refused post does not wake (no spurious flag)");

    poll_waiter_list_unregister(&pw);
    pw.magic = 0;
    loom_unref(l);
}

// KT-1.5: the KObj_Loom .poll hook (loom_poll), driven directly (white-box).
// An empty CQ reports not-ready AND registers the waiter (register-then-observe);
// a CQE post then sets that waiter's flag (the same NoMissedCqWake / cq_waiters
// path test_loom_cq_waiter_wake pins) and a re-poll reports POLLIN. A POLLOUT-only
// request registers nothing and reports nothing (SQ-space is unmodelled at v1.0).
void test_loom_poll(void) {
    struct Loom *l = loom_create(4, 8, false);
    TEST_ASSERT(l != NULL, "loom_create(4,8)");

    struct Rendez r;
    rendez_init(&r);
    struct poll_waiter pw;
    poll_waiter_init(&pw, &r);

    short re = loom_poll(l, POLLIN, &pw);
    TEST_EXPECT_EQ((int)re, 0, "empty ring: loom_poll reports not-ready");
    TEST_ASSERT(pw.list != NULL, "loom_poll REGISTERED the waiter on cq_waiters");
    TEST_ASSERT(pw.ready == false, "fresh waiter not ready before any post");

    TEST_EXPECT_EQ(loom_post_cqe(l, 0xF00Du, 0, 0), 0, "CQE post succeeds");
    TEST_ASSERT(pw.ready == true, "post sets the loom_poll-registered waiter flag (I-9)");
    poll_waiter_list_unregister(&pw);

    struct poll_waiter pw2;
    poll_waiter_init(&pw2, &r);
    short re2 = loom_poll(l, POLLIN, &pw2);
    TEST_ASSERT((re2 & POLLIN) != 0, "ready ring: loom_poll reports POLLIN");
    poll_waiter_list_unregister(&pw2);

    struct poll_waiter pw3;
    poll_waiter_init(&pw3, &r);
    short re3 = loom_poll(l, POLLOUT, &pw3);
    TEST_EXPECT_EQ((int)re3, 0, "POLLOUT-only: loom_poll reports nothing (v1.0)");
    TEST_ASSERT(pw3.list == NULL, "POLLOUT-only: no registration");

    pw.magic = 0; pw2.magic = 0; pw3.magic = 0;
    loom_unref(l);
}

// The reworked wait phase must NOT hang on the common no-sibling path. With
// inline NOPs (which complete during submit), a BLOCKING enter with
// min_complete == the NOP count sees the CQ already satisfied at the first
// sample and returns without ever sleeping (async_inflight stays 0 -- there is
// no reader to drive). CqWaitCommitOrSleep: flag/level satisfied -> return.
void test_loom_enter_inline_min_complete(void) {
    struct Loom *l = loom_create(8, 16, false);
    TEST_ASSERT(l != NULL, "loom_create(8,16)");
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);

    for (u32 i = 0; i < 3; i++) loom_stage_sqe(l, i, LOOM_OP_NOP, 0, 0, 0, 0x300u + i);
    __atomic_store_n(&h->sq_tail, 3u, __ATOMIC_RELEASE);

    int n = loom_enter(l, 3, 3, 0);   // min_complete = 3, BLOCKING (no NONBLOCK)
    TEST_EXPECT_EQ(n, 3, "3 NOPs consumed");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)3, "3 inline CQEs; wait returned without sleeping");
    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)0, "nothing went in flight (no reader to drive)");

    loom_unref(l);
}

// CqWaitCommitOrSleep give-up arm: min_complete > 0 but nothing is (or can be)
// in flight -> the wait returns immediately rather than blocking for CQEs that
// can never arrive. A blocking enter with no SQEs staged and async_inflight == 0
// must NOT hang.
void test_loom_enter_min_complete_no_inflight(void) {
    struct Loom *l = loom_create(8, 16, false);
    TEST_ASSERT(l != NULL, "loom_create(8,16)");

    int n = loom_enter(l, 0, 5, 0);   // min_complete = 5, BLOCKING, but no work
    TEST_EXPECT_EQ(n, 0, "no SQEs consumed");
    TEST_EXPECT_EQ((u64)l->cq_tail, (u64)0, "no CQEs; wait returned without hanging");
    TEST_EXPECT_EQ((u64)l->async_inflight, (u64)0, "nothing in flight");

    loom_unref(l);
}

// ---------------------------------------------------------------------------
// Loom-4c: the SQPOLL poll-thread. LOOM_SETUP_SQPOLL spawns a per-ring kproc()
// kthread that drains the SQ zero-syscall + drives the elected reader; loom_free
// joins it. These pin the spawn -> park -> stop -> join lifecycle + the zero-
// syscall drain DETERMINISTICALLY (white-box). The full kthread-vs-ENTER-vs-
// Proc-death interleaving over a live dev9p client lands with the deferred SMP
// harness (#907). Maps to LOOM.md 8.6; the kthread reuses the audited
// specs/loom.tla Consume / Dispatch / PostCqe path -- only the park wait/wake is
// the new modeled surface (CqWaitRegister already covered by 4b).
// ---------------------------------------------------------------------------

// Spawn + clean join. Setting up an SQPOLL ring spawns the kthread (l->sqpoll
// set; out.flags echoes the grant); tearing the ring down (proc_free -> the last
// handle close -> loom_unref -> loom_free) stops + JOINS the kthread and frees
// the ring EXACTLY once -- no hang, no leak. With no SQEs the kthread parks
// immediately, so the join exercises the wake-park -> observe-stopping ->
// EXITING-terminal -> thread_free path end to end.
void test_loom_sqpoll_setup_and_teardown(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "proc_alloc");
    u64 destroyed0 = loom_total_destroyed();

    struct loom_params kp;
    hidx_t fd = -1;
    int rc = sys_loom_setup_for_proc(p, 8, LOOM_SETUP_SQPOLL, &kp, &fd);
    TEST_EXPECT_EQ(rc, 0, "SQPOLL setup succeeds");
    TEST_ASSERT(fd >= 0, "setup returned a valid fd");
    TEST_EXPECT_EQ(kp.flags, (u32)LOOM_SETUP_SQPOLL, "out.flags echoes the SQPOLL grant");

    struct Handle h;
    TEST_ASSERT(handle_get(p, fd, &h) == 0, "handle_get(loom fd)");
    struct Loom *l = (struct Loom *)h.obj;
    TEST_ASSERT(l->sqpoll != NULL, "SQPOLL kthread spawned");
    handle_put(&h);

    // proc_free closes the handle -> loom_unref -> loom_free joins the kthread +
    // frees the ring exactly once. A hang here would mean the join never
    // converged (the whole point of the test).
    test_proc_drop(p);
    TEST_EXPECT_EQ(loom_total_destroyed() - destroyed0, (u64)1,
                   "ring freed (kthread joined) exactly once on teardown");
}

// Zero-syscall drain. On an SQPOLL ring the user stages SQEs + bumps sq_tail; an
// ENTER(to_submit=0) WAKES the kthread (it never submits on SQPOLL), which drains
// the SQ + posts the inline NOP CQEs. cq_tail reaching the staged count proves
// the kthread -- not this call -- consumed. The test cooperatively sched()s so
// the kthread runs even at -smp 1 (the test_torpor.c yield idiom); the spin is
// bounded so a real failure terminates rather than hangs.
void test_loom_sqpoll_drains_sq(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "proc_alloc");

    struct loom_params kp;
    hidx_t fd = -1;
    TEST_EXPECT_EQ(sys_loom_setup_for_proc(p, 8, LOOM_SETUP_SQPOLL, &kp, &fd), 0,
                   "SQPOLL setup");
    struct Handle h;
    TEST_ASSERT(handle_get(p, fd, &h) == 0, "handle_get");
    struct Loom *l = (struct Loom *)h.obj;
    struct loom_ring_hdr *hdr = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);

    for (u32 i = 0; i < 3; i++) loom_stage_sqe(l, i, LOOM_OP_NOP, 0, 0, 0, 0x700u + i);
    __atomic_store_n(&hdr->sq_tail, 3u, __ATOMIC_RELEASE);

    // ENTER(to_submit=0, min_complete=0) on an SQPOLL ring submits nothing -- it
    // just wakes the (parked) kthread to drain.
    TEST_EXPECT_EQ(sys_loom_enter_for_proc(p, fd, 0, 0, 0), 0,
                   "ENTER on SQPOLL submits nothing (kthread owns submission)");

    // Yield to the kthread until it has drained the 3 NOPs. Bounded so a hung
    // kthread fails the test rather than wedging the whole suite.
    bool drained = false;
    for (u32 round = 0; round < 100000u; round++) {
        // Read the shared-header mirror (loom_post_cqe publishes it with a release
        // store), NOT the kernel-private l->cq_tail (a plain store under l->lock) --
        // the mirror read is a clean acquire with no plain-write/atomic-read race (F5).
        if (__atomic_load_n(&hdr->cq_tail, __ATOMIC_ACQUIRE) >= 3u) { drained = true; break; }
        sched();   // cooperative yield -> the kthread gets the CPU at -smp 1
    }
    TEST_ASSERT(drained, "SQPOLL kthread drained the SQ zero-syscall (3 CQEs posted)");

    handle_put(&h);
    test_proc_drop(p);
}

// F2 regression: on CQ-full backpressure the SQPOLL kthread PARKS (state ==
// THREAD_SLEEPING) -- it does NOT busy-loop. cq_entries == 2*sq_entries (the
// io_uring default), so the CQ fills only after MORE NOPs than the SQ ring holds --
// submit cq_entries NOPs in sq_entries-sized batches (refilling the ring), draining
// each batch zero-syscall, WITHOUT reaping. Then one more NOP, pending, with the CQ
// full: the kthread cannot admit it. Pre-F2 the park cond fired on sq_tail != sq_head
// alone, sleep() returned WITHOUT sched()ing, and the kthread spun at 100% CPU (on
// -smp 1 it would never yield -> this test would HANG); post-F2 the cond also gates on
// CQ room, so the kthread parks. Then reap (advance cq_head) + ENTER-wake and the
// extra NOP drains -- proving the park RELEASES on the reap+ENTER path (the
// NEED_WAKEUP contract).
void test_loom_sqpoll_parks_on_cq_full(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "proc_alloc");

    struct loom_params kp;
    hidx_t fd = -1;
    TEST_EXPECT_EQ(sys_loom_setup_for_proc(p, 4, LOOM_SETUP_SQPOLL, &kp, &fd), 0, "SQPOLL setup");
    struct Handle h;
    TEST_ASSERT(handle_get(p, fd, &h) == 0, "handle_get");
    struct Loom *l = (struct Loom *)h.obj;
    struct loom_ring_hdr *hdr = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    u32 cqe = l->cq_entries;   // = 2 * sq_entries (8); the SQ ring holds only sq_entries (4)
    u32 sqe = l->sq_entries;

    // Fill the CQ to cq_entries WITHOUT reaping, in sqe-sized batches (each fully
    // drained before the next, so the kthread parks-on-SQ-drained between them).
    u32 produced = 0;
    while (produced < cqe) {
        u32 batch = (cqe - produced < sqe) ? (cqe - produced) : sqe;
        for (u32 i = 0; i < batch; i++)
            loom_stage_sqe(l, i, LOOM_OP_NOP, 0, 0, 0, 0xA00u + produced + i);
        produced += batch;
        __atomic_store_n(&hdr->sq_tail, produced, __ATOMIC_RELEASE);
        TEST_EXPECT_EQ(sys_loom_enter_for_proc(p, fd, 0, 0, 0), 0, "ENTER (fill batch)");
        bool got = false;
        for (u32 r = 0; r < 100000u && !got; r++) {
            if (__atomic_load_n(&hdr->cq_tail, __ATOMIC_ACQUIRE) >= produced) got = true; else sched();
        }
        TEST_ASSERT(got, "batch drained into the CQ");
    }
    TEST_EXPECT_EQ(__atomic_load_n(&hdr->cq_tail, __ATOMIC_ACQUIRE), cqe, "CQ full (cq_entries unreaped)");

    // One more NOP, pending, with the CQ full + NOT reaped -> the kthread cannot
    // admit it and must PARK. Stage into ring slot (produced & (sqe-1)).
    loom_stage_sqe(l, produced & (sqe - 1u), LOOM_OP_NOP, 0, 0, 0, 0xAFFu);
    __atomic_store_n(&hdr->sq_tail, produced + 1u, __ATOMIC_RELEASE);
    TEST_EXPECT_EQ(sys_loom_enter_for_proc(p, fd, 0, 0, 0), 0, "ENTER (still CQ-full)");

    // l->sqpoll->state is scheduler-written (a best-effort coarse poll -- no clean
    // atomic accessor exists; the hardware load is atomic and TSan-pedantry aside
    // this is a liveness observation). Pre-F2 the kthread never reaches SLEEPING (the
    // always-true cond early-returns from sleep without sched()).
    bool parked = false;
    for (u32 r = 0; r < 100000u && !parked; r++) {
        if (__atomic_load_n(&l->sqpoll->state, __ATOMIC_ACQUIRE) == THREAD_SLEEPING) parked = true;
        else sched();
    }
    TEST_ASSERT(parked, "kthread parks on CQ-full backpressure (no busy-loop)");
    TEST_EXPECT_EQ(__atomic_load_n(&hdr->cq_tail, __ATOMIC_ACQUIRE), cqe,
                   "extra NOP NOT admitted while CQ full");

    // Reap everything + ENTER-wake: the park releases (cond now sees CQ room) -> the
    // extra NOP drains.
    __atomic_store_n(&hdr->cq_head, cqe, __ATOMIC_RELEASE);
    TEST_EXPECT_EQ(sys_loom_enter_for_proc(p, fd, 0, 0, 0), 0, "ENTER after reap");
    bool resumed = false;
    for (u32 r = 0; r < 100000u && !resumed; r++) {
        if (__atomic_load_n(&hdr->cq_tail, __ATOMIC_ACQUIRE) >= cqe + 1u) resumed = true; else sched();
    }
    TEST_ASSERT(resumed, "kthread resumes + drains the extra NOP after reap+ENTER");

    handle_put(&h);
    test_proc_drop(p);
}

// Fid-lift audit F1: an SQPOLL kthread charges the CREATOR's PROC_THREAD_MAX
// budget (I-32). The kthread runs under kproc (exempt), so pre-F1 an
// unprivileged Proc could mint one 32-KiB-kstack kthread per ring, bounded only
// by the handle table the fid lift just quadrupled. The budget is pre-loaded to
// the boundary (the test_resource.c idiom) so only two real kthreads spawn:
// refusal at the cap, no over-settle by the refused ring's free, plain rings
// uncharged, and the close -> uncharge -> fits-again round trip.
void test_loom_sqpoll_charges_thread_budget(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "proc_alloc");
    p->principal_id = 1000u;                  // a real user -- NOT I-32 exempt
    u64 destroyed0 = loom_total_destroyed();

    p->thread_count = PROC_THREAD_MAX - 1;    // boundary without 255 real threads

    // At cap-1: one SQPOLL setup fits and takes the charge.
    struct loom_params kp;
    hidx_t fd1 = -1;
    TEST_EXPECT_EQ(sys_loom_setup_for_proc(p, 8, LOOM_SETUP_SQPOLL, &kp, &fd1), 0,
                   "SQPOLL setup at cap-1 succeeds");
    TEST_EXPECT_EQ((u64)p->loom_sqpoll_count, (u64)1, "kthread charged to the creator");

    // At the cap: the next SQPOLL setup is REFUSED (pre-F1 it succeeded and the
    // kthread escaped the budget entirely).
    hidx_t fd2 = -1;
    TEST_ASSERT(sys_loom_setup_for_proc(p, 8, LOOM_SETUP_SQPOLL, &kp, &fd2) < 0,
                "SQPOLL setup at the cap refused");
    // The refused ring never took a charge, and its free must not settle one
    // (sqpoll_charged gates the uncharge): still exactly 1.
    TEST_EXPECT_EQ((u64)p->loom_sqpoll_count, (u64)1, "refusal did not over-settle");
    TEST_EXPECT_EQ(loom_total_destroyed() - destroyed0, (u64)1,
                   "the refused ring itself was reclaimed");

    // A plain (non-SQPOLL) ring is not thread-charged -- the budget bounds
    // kthreads, not rings.
    hidx_t fd3 = -1;
    TEST_EXPECT_EQ(sys_loom_setup_for_proc(p, 8, 0, &kp, &fd3), 0,
                   "a plain ring still fits at the thread cap");
    TEST_EXPECT_EQ((u64)p->loom_sqpoll_count, (u64)1, "plain ring took no charge");

    // Close the charged ring: loom_free joins the kthread and settles the
    // charge exactly once; the freed budget admits a new SQPOLL setup.
    TEST_EXPECT_EQ(handle_close(p, fd1), 0, "close the SQPOLL ring");
    TEST_EXPECT_EQ((u64)p->loom_sqpoll_count, (u64)0, "charge settled on close");
    hidx_t fd4 = -1;
    TEST_EXPECT_EQ(sys_loom_setup_for_proc(p, 8, LOOM_SETUP_SQPOLL, &kp, &fd4), 0,
                   "SQPOLL setup fits again after the close (uncharge proof)");

    p->thread_count = 0;                      // reset for proc_free's gate
    test_proc_drop(p);
}

// ---------------------------------------------------------------------------
// #130: the I-32 charge balances across the WHOLE Loom lifetime, in both
// directions.
//
// The ring is the one eager charger whose Burrow keeps a second owner past the
// VMA: loom_create holds a handle_count ref for the Loom's whole life so the
// ring pages cannot vanish under the kernel's ring_kva. That makes "the mapping
// went away" and "the pages went away" two DIFFERENT events, and #106-F1 got
// the accounting wrong on each of them in turn:
//
//   - refunding on the VMA's TYPE let EL0 detach the ring, take the refund,
//     keep the pages on the Loom's ref, and re-attach a full budget --
//     an unprivileged breach of the per-Proc floor;
//   - refunding on a handle_count SAMPLED BEFORE the drop then swung it the
//     other way: on the ordinary teardown order (detach, then close -- what
//     libthyla_rs' Ring::drop does) the sample reads 1, the refund is skipped,
//     and loom_free freed the pages with nothing uncharging -- a PERMANENT
//     over-charge.
//
// This test pins both halves, so it fails on EITHER of those states as well as
// on any future one that gets the pairing wrong: the detach must refund
// NOTHING, and the close must return the counter exactly to baseline.
// ---------------------------------------------------------------------------
void test_loom_ring_charge_balance(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "proc_alloc");

    u32 base = __atomic_load_n(&p->as->page_count, __ATOMIC_ACQUIRE);

    struct loom_params kp;
    hidx_t loom_fd = -1;
    TEST_ASSERT(sys_loom_setup_for_proc(p, 8, 0, &kp, &loom_fd) == 0, "setup");

    u32 ring_pages = (u32)burrow_backing_pages((size_t)kp.ring_size);
    TEST_ASSERT(ring_pages > 0, "the ring occupies at least one page");
    TEST_EXPECT_EQ((u64)__atomic_load_n(&p->as->page_count, __ATOMIC_ACQUIRE),
                   (u64)(base + ring_pages), "setup charged the ring's occupancy");

    // Detach the ring VA while the Loom still holds its handle_count ref. The
    // pages do NOT free here, so nothing may be refunded -- this is the leg
    // that, refunded, hands EL0 free budget.
    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(p, kp.ring_va, kp.ring_size), 0L,
                   "detach the ring VA");
    TEST_EXPECT_EQ((u64)__atomic_load_n(&p->as->page_count, __ATOMIC_ACQUIRE),
                   (u64)(base + ring_pages),
                   "a detach that frees nothing refunds nothing");

    // Close the Loom: loom_free drops the last ref, the pages free, and the
    // charge is settled -- exactly once, and only now.
    TEST_EXPECT_EQ(handle_close(p, loom_fd), 0, "close the loom fd");
    TEST_EXPECT_EQ((u64)__atomic_load_n(&p->as->page_count, __ATOMIC_ACQUIRE), (u64)base,
                   "the loom close settled the ring charge back to baseline");

    test_proc_drop(p);
}

// #130, the registered-buffer twin. A registered buffer is the user's OWN
// attach region -- so it can be the whole page budget, which is what makes the
// over-charge direction matter here more than on the (small) ring. Same shape:
// the table's pin outlives the VMA, so the detach must not refund and the pin's
// release must. Both release sites are covered: the re-register displacement
// (loom_register_buffers) and the teardown (loom_free).
void test_loom_regbuf_charge_balance(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "proc_alloc");

    struct loom_params kp;
    hidx_t loom_fd = -1;
    TEST_ASSERT(sys_loom_setup_for_proc(p, 8, 0, &kp, &loom_fd) == 0, "setup");

    // Baseline AFTER setup, so the ring's own charge is not in the arithmetic.
    u32 base = __atomic_load_n(&p->as->page_count, __ATOMIC_ACQUIRE);

    // --- leg 1: the pin is displaced by a re-register ---
    s64 bva = sys_burrow_attach_for_proc(p, PAGE_SIZE);
    TEST_ASSERT(bva > 0, "attach a buffer page");
    u32 bpages = (u32)burrow_backing_pages(PAGE_SIZE);
    TEST_EXPECT_EQ((u64)__atomic_load_n(&p->as->page_count, __ATOMIC_ACQUIRE),
                   (u64)(base + bpages), "the attach charged the buffer");

    struct loom_buf_reg br = { .va = (u64)bva, .len = PAGE_SIZE };
    TEST_ASSERT(sys_loom_register_buffers_for_proc(p, loom_fd, &br, 1) == 0, "register");

    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(p, (u64)bva, PAGE_SIZE), 0L, "detach the buffer VA");
    TEST_EXPECT_EQ((u64)__atomic_load_n(&p->as->page_count, __ATOMIC_ACQUIRE),
                   (u64)(base + bpages),
                   "detach with the table still pinning refunds nothing");

    TEST_ASSERT(sys_loom_register_buffers_for_proc(p, loom_fd, NULL, 0) == 0, "clear the table");
    TEST_EXPECT_EQ((u64)__atomic_load_n(&p->as->page_count, __ATOMIC_ACQUIRE), (u64)base,
                   "the displaced pin's release settled the buffer charge");

    // --- leg 2: the pin is released by the teardown ---
    s64 bva2 = sys_burrow_attach_for_proc(p, PAGE_SIZE);
    TEST_ASSERT(bva2 > 0, "attach a second buffer page");
    struct loom_buf_reg br2 = { .va = (u64)bva2, .len = PAGE_SIZE };
    TEST_ASSERT(sys_loom_register_buffers_for_proc(p, loom_fd, &br2, 1) == 0, "register #2");
    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(p, (u64)bva2, PAGE_SIZE), 0L, "detach #2's VA");
    TEST_EXPECT_EQ((u64)__atomic_load_n(&p->as->page_count, __ATOMIC_ACQUIRE),
                   (u64)(base + bpages), "still pinned, still charged");

    // Detach the ring too, so the close faces the FULL production teardown
    // order: every VA released first (what Ring::drop does), then the fd. Both
    // objects are then charged-but-unmapped, and the close must settle both.
    u32 ring_pages = (u32)burrow_backing_pages((size_t)kp.ring_size);
    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(p, kp.ring_va, kp.ring_size), 0L,
                   "detach the ring VA too");
    TEST_EXPECT_EQ((u64)__atomic_load_n(&p->as->page_count, __ATOMIC_ACQUIRE),
                   (u64)(base + bpages), "the ring detach refunds nothing either");

    TEST_EXPECT_EQ(handle_close(p, loom_fd), 0, "close the loom fd");
    // base was sampled AFTER setup, so it still contains the ring's charge;
    // settling both lands one ring below it.
    TEST_EXPECT_EQ((u64)__atomic_load_n(&p->as->page_count, __ATOMIC_ACQUIRE),
                   (u64)(base - ring_pages),
                   "the teardown settled both the buffer pin and the ring");

    test_proc_drop(p);
}

// ---------------------------------------------------------------------------
// loom.regbuf_foreign_charge_not_refunded (#132)
//
// A registered buffer is resolved out of ANY writable ANON VMA of the Loom's
// owner -- and a Weft ring that a SHARER allocated and burrow_share_into'd
// satisfies every loom_resolve_buf gate (ANON, writable, one VMA). Weft-6c-1's
// shipped path registers exactly that: WeftFlow::open maps the flow's ring and
// registers the WHOLE of it.
//
// So the owner can hold a Loom pin on a region it did not pay for, and when
// that pin is the drop that frees the pages, refunding the owner pays back
// somebody else's charge. That is an UNDER-count -- it inflates the owner's
// effective budget, the direction that breaks I-32 -- and it is what #130's
// "`p` is the charged Proc by construction" comment let through: that argument
// proves p owns the LOOM, not that p paid for the BUFFER.
//
// The discriminating assertion is that the user's page_count never dips below
// its own baseline. The baseline is sampled AFTER the loom setup so it carries
// the ring's charge, which is what makes a spurious one-page refund visible
// rather than clamped away at zero.
// ---------------------------------------------------------------------------
void test_loom_regbuf_foreign_charge_not_refunded(void) {
    struct Proc *payer = test_proc_make();   // allocates + pays for the region
    struct Proc *user  = test_proc_make();   // maps it and pins it in its Loom
    TEST_ASSERT(payer != NULL && user != NULL, "proc_alloc x2");

    // DISTINCT pids, or this test cannot discriminate. proc_alloc stamps pid 0
    // (rfork_internal assigns the real one), so two bare test Procs are both 0
    // and the charge record's owner check would match the wrong Proc trivially
    // -- the test would pass without proving attribution. Production Procs get
    // distinct monotonic pids; the fixture has to reproduce that, not assume it.
    payer->pid = 90001;
    user->pid  = 90002;

    struct loom_params kp;
    hidx_t loom_fd = -1;
    TEST_ASSERT(sys_loom_setup_for_proc(user, 8, 0, &kp, &loom_fd) == 0, "setup");
    u32 ubase = __atomic_load_n(&user->as->page_count, __ATOMIC_ACQUIRE);
    TEST_ASSERT(ubase > 0, "the baseline carries the ring charge, so a refund is visible");

    u32 pbase = __atomic_load_n(&payer->as->page_count, __ATOMIC_ACQUIRE);
    s64 va = sys_burrow_attach_for_proc(payer, PAGE_SIZE);
    TEST_ASSERT(va > 0, "the payer attaches the region");
    u32 pages = (u32)burrow_backing_pages(PAGE_SIZE);
    TEST_EXPECT_EQ((u64)__atomic_load_n(&payer->as->page_count, __ATOMIC_ACQUIRE),
                   (u64)(pbase + pages), "the attach charged the PAYER");

    spin_lock(&payer->as->lock);
    struct Vma *pv = vma_lookup(payer, (u64)va);
    struct Burrow *v = pv ? pv->burrow : NULL;
    spin_unlock(&payer->as->lock);
    TEST_ASSERT(v != NULL, "the region's Burrow");

    // Share it into the user (the SYS_WEFT_MAP half) and register the whole of
    // it as a Loom buffer (the WeftFlow::open half).
    // Inside [EXEC_USER_BURROW_BASE, TOP): SYS_BURROW_DETACH bounds-checks the
    // VA against that window before it even looks up the VMA, so a share placed
    // below 4 GiB is unreachable to the detach half of this test.
    const u64 uva = 0x140000000ull;
    spin_lock(&user->as->lock);
    TEST_EXPECT_EQ(burrow_share_into(user, v, uva, VMA_PROT_RW), 0, "share into the user");
    spin_unlock(&user->as->lock);
    TEST_EXPECT_EQ((u64)__atomic_load_n(&user->as->page_count, __ATOMIC_ACQUIRE),
                   (u64)ubase, "a share does not charge the user's page_count");

    struct loom_buf_reg br = { .va = uva, .len = PAGE_SIZE };
    TEST_ASSERT(sys_loom_register_buffers_for_proc(user, loom_fd, &br, 1) == 0,
                "the user registers the shared region as a Loom buffer");

    // The payer walks away (#131): its charge settles here, while the region
    // lives on under the user's mapping + the Loom pin.
    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(payer, (u64)va, PAGE_SIZE), 0L, "payer detaches");
    TEST_EXPECT_EQ((u64)__atomic_load_n(&payer->as->page_count, __ATOMIC_ACQUIRE),
                   (u64)pbase, "the payer's charge settled at its own detach");

    // The user drops its mapping. SHARED_IN, so no page_count movement -- and
    // the Loom pin is now the ONLY thing holding the region.
    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(user, uva, PAGE_SIZE), 0L, "user detaches");
    TEST_EXPECT_EQ((u64)__atomic_load_n(&user->as->page_count, __ATOMIC_ACQUIRE),
                   (u64)ubase, "detaching a SHARED_IN mapping refunds nothing");
    TEST_EXPECT_EQ(burrow_mapping_count(v), 0, "no mappings left -- only the Loom pin");

    // THE FIX: clearing the table drops that pin and FREES the pages. Pre-#132
    // this refunded the user for the payer's region.
    u64 destroyed_before = burrow_total_destroyed();
    TEST_ASSERT(sys_loom_register_buffers_for_proc(user, loom_fd, NULL, 0) == 0, "clear the table");
    TEST_EXPECT_EQ(burrow_total_destroyed(), destroyed_before + 1,
                   "the displaced pin was the drop that freed the pages");
    TEST_EXPECT_EQ((u64)__atomic_load_n(&user->as->page_count, __ATOMIC_ACQUIRE),
                   (u64)ubase,
                   "freeing a region the user never paid for refunds the user NOTHING");
    TEST_EXPECT_EQ((u64)__atomic_load_n(&payer->as->page_count, __ATOMIC_ACQUIRE),
                   (u64)pbase, "and does not refund the payer twice either");

    TEST_EXPECT_EQ(handle_close(user, loom_fd), 0, "close the loom fd");
    test_proc_drop(user);
    test_proc_drop(payer);
}

// LOOM.md 8.5.1 (audit F2): a blocking ENTER must not give up while a sibling
// driver sits between its consume and its disposition (`admitting`; loom.tla's
// CanStillComplete counts that phase). A kthread waits for one CQE on a ring
// whose only pending op is a phantom mid-submit; with admitting 0 -- the
// control, one variable away -- the same wait returns at once. The phantom's
// CQE is posted as a rescue BEFORE the verdict, so a failure never strands the
// waiter.
static struct Loom *g_lwa_ring;
static bool g_lwa_done, g_lwa_exited;

static void lwa_waiter_entry(void) {
    (void)loom_enter(g_lwa_ring, 0, 1, 0);
    __atomic_store_n(&g_lwa_done, true, __ATOMIC_RELEASE);
    // The terminal EXITING handshake (the loom_sqpoll_main idiom).
    (void)spin_lock_irqsave(NULL);
    current_thread()->state = THREAD_EXITING;
    __atomic_store_n(&g_lwa_exited, true, __ATOMIC_RELEASE);
    sched();
    extinction("lwa_waiter_entry: returned from terminal sched");
}

static struct Thread *lwa_spawn(void) {
    __atomic_store_n(&g_lwa_done, false, __ATOMIC_RELAXED);
    __atomic_store_n(&g_lwa_exited, false, __ATOMIC_RELAXED);
    struct Thread *t = thread_create(kproc(), lwa_waiter_entry);
    if (t) ready(t);
    return t;
}

void test_loom_wait_counts_admitting(void) {
    struct Loom *l = loom_create(4, 4, false);
    TEST_ASSERT(l != NULL, "loom_create");
    g_lwa_ring = l;
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);

    struct Thread *t = lwa_spawn();
    TEST_ASSERT(t != NULL, "spawn the control waiter");
    TEST_YIELD_UNTIL(__atomic_load_n(&g_lwa_done, __ATOMIC_ACQUIRE));
    TEST_YIELD_UNTIL(__atomic_load_n(&g_lwa_exited, __ATOMIC_ACQUIRE));
    thread_free(t);

    spin_lock(&l->lock);
    l->admitting = 1;                       // a sibling driver, mid-gate
    spin_unlock(&l->lock);
    t = lwa_spawn();
    TEST_ASSERT(t != NULL, "spawn the waiter");
    // The verdict needs the waiter to have REACHED the wait: parked in its sleep
    // (on the CQ wait-list AND sleeping), or returned. "Not returned yet" alone is
    // also what an undispatched waiter looks like. Soft, so the rescue below runs
    // on every verdict and a red one never strands the thread.
    TEST_YIELD_UNTIL_SOFT(__atomic_load_n(&g_lwa_done, __ATOMIC_ACQUIRE) ||
                          (!poll_waiter_list_empty(&l->cq_waiters) &&
                           t->state == THREAD_SLEEPING));
    bool parked  = !poll_waiter_list_empty(&l->cq_waiters) && t->state == THREAD_SLEEPING;
    bool gave_up = __atomic_load_n(&g_lwa_done, __ATOMIC_ACQUIRE);
    // The rescue, as a real disposition lands: a CQE (which wakes), the
    // reservation released, and loom_admitted's wake.
    int posted = loom_post_cqe(l, 0xF2F2u, 0, 0);
    spin_lock(&l->lock);
    l->admitting = 0;
    spin_unlock(&l->lock);
    poll_waiter_list_wake(&l->cq_waiters);
    TEST_YIELD_UNTIL_SOFT(__atomic_load_n(&g_lwa_exited, __ATOMIC_ACQUIRE));
    // A waiter that outlived the rescue still holds the ring: leak both rather
    // than free under it.
    TEST_ASSERT(__atomic_load_n(&g_lwa_exited, __ATOMIC_ACQUIRE), "the rescue ended the wait");
    thread_free(t);
    TEST_EXPECT_EQ((u64)posted, (u64)0, "post the phantom's CQE");
    TEST_ASSERT(!gave_up, "the wait held while a sibling was mid-submit");
    TEST_ASSERT(parked, "the waiter parked in the wait's sleep");
    TEST_EXPECT_EQ((u64)(h->cq_tail - h->cq_head), (u64)1, "the phantom's CQE ended the wait");
    loom_unref(l);
}

// Actual native registry + navigation handle + private owner, without a peer
// responder. Borrowing retains both exact target identity and the retiring ring.
static const char *loom_targets_fixture(void) {
    const char *error = NULL;
    struct Proc *p = test_proc_make(), *poster = test_proc_make();
    struct SrvRegistry *reg = NULL;
    struct Spoor *root = NULL;
    struct Loom *l = NULL;
    struct loom_service_ref ref = {0}, stale = {0}, provisional = {0};
    const struct SrvServiceTarget *borrow = NULL;
    bool pinned = false, owner_dropped = false;
    u64 goal = loom_private_retired(), destroyed = srv_registry_total_destroyed();
    u32 base = 0;
    struct loom_service_target in;
    for (u32 i = 0; i < sizeof(in); ++i) ((u8 *)&in)[i] = 0;
#define LT_CHECK(c,msg) do { if (!(c)) { error = msg; goto done; } } while (0)
    LT_CHECK(p && poster, "target procs allocated");
    base = p->as->page_count;
    proc_mark_may_post_service(poster);
    reg = srv_registry_create();
    LT_CHECK(reg, "target registry allocated");
    root = devsrv_attach_registry(reg);
    LT_CHECK(root, "target native root allocated");
    root->flag |= CWALKONLY;
    LT_CHECK(devsrv_post_listener(poster, root, "target", 6, SRV_MODE_9P,
                                  false, false, false) >= 0, "target strict native post");
    spoor_ref(root);
    int fd = handle_alloc(p, KOBJ_SPOOR, RIGHT_READ, root);
    if (fd < 0) spoor_clunk(root);
    LT_CHECK(fd >= 0, "target navigation fd installed");
    l = loom_create_private(p, 2, 2, true);
    if (l) goal++;
    LT_CHECK(l, "target ring allocated");
    in.size = sizeof(in); in.version = 1; in.registry_fd = fd; in.name_len = 6;
    const char *name = "target";
    for (u32 i = 0; i < 6; ++i) in.name[i] = name[i];
    LT_CHECK(loom_private_target_prepare(l, poster, &in, &ref) == -T_E_ACCES,
             "target foreign creator refused");
    in.name[255] = 1;
    LT_CHECK(loom_private_target_prepare(l, p, &in, &ref) == -T_E_INVAL,
             "target unused input tail refused");
    in.name[255] = 0;
    LT_CHECK(!loom_private_target_prepare(l, p, &in, &ref), "target provisionally captured");
    LT_CHECK(loom_private_target_get(l, ref, &borrow) == -T_E_NOENT,
             "provisional target cannot be borrowed");
    stale = ref;
    LT_CHECK(!loom_private_target_rollback(l, ref), "target copyout rollback");
    LT_CHECK(!loom_private_target_prepare(l, p, &in, &ref) &&
             ref.incarnation > stale.incarnation, "target rollback burns identity");
    LT_CHECK(!loom_private_target_publish(l, ref), "target publish");
    LT_CHECK(loom_private_target_get(l, stale, &borrow) == -T_E_NOENT,
             "stale target reference refused");
    LT_CHECK(!loom_private_target_get(l, ref, &borrow), "target borrow accepted");
    pinned = true;
    LT_CHECK(borrow->view == reg && borrow->service == srv_lookup_in(reg, name, 6) &&
             borrow->generation && borrow->name_len == 6,
             "target keeps exact native registry post identity");
    LT_CHECK(loom_private_target_reap(l, ref) == -T_E_BUSY,
             "borrow prevents target reap");
    LT_CHECK(!loom_private_target_put(l, ref), "target borrow release");
    pinned = false;
    LT_CHECK(!loom_private_target_reap(l, ref), "unborrowed target reaps");
    LT_CHECK(loom_private_target_put(l, ref) == -T_E_NOENT, "stale target put changes nothing");
    LT_CHECK(!loom_private_target_prepare(l, p, &in, &ref) &&
             !loom_private_target_publish(l, ref), "target republished for close");
    LT_CHECK(!loom_private_target_prepare(l, p, &in, &provisional), "second provisional target");
    LT_CHECK(!loom_private_target_get(l, ref, &borrow), "closing target retained");
    pinned = true;
    LT_CHECK(!handle_close(p, fd), "original navigation handle closed");
    spoor_clunk(root); root = NULL;
    test_proc_drop(poster); poster = NULL;
    srv_registry_unref(reg); reg = NULL;
    LT_CHECK(srv_registry_total_destroyed() == destroyed,
             "targets retain registry after navigation and poster leave");
    loom_exec_latch(l);
    LT_CHECK(loom_private_target_publish(l, provisional) == -T_E_CANCELED,
             "close wins publication");
    LT_CHECK(!loom_private_target_rollback(l, provisional), "closed target rollback progresses");
    LT_CHECK(loom_private_target_get(l, ref, &borrow) == -T_E_CANCELED,
             "closed owner rejects new target borrows");
    // Drop the original owner while the borrow remains; only that borrow may
    // now touch l. Final put enqueues cleanup of the still-live target itself.
    loom_unref(l); owner_dropped = true;
    LT_CHECK(loom_private_retired() < goal && borrow->view && borrow->generation,
             "borrow retains closed owner and native target");
    (void)loom_private_target_put(l, ref); l = NULL; pinned = false;
    LT_CHECK(lp_wait(goal), "final target borrower retires ring");
    LT_CHECK(srv_registry_total_destroyed() == destroyed + 1,
             "final target cleanup destroys registry exactly once");
    LT_CHECK(p->as->page_count == base && !p->as->private_rings,
             "target owner refunds all metadata and backing");
done:
    if (l) {
        if (pinned) (void)loom_private_target_put(l, ref);
        if (!owner_dropped) loom_unref(l);
    }
    if (goal && !lp_wait(goal) && !error) error = "target cleanup retirement timeout";
    if (root) spoor_clunk(root);
    test_proc_drop(p); test_proc_drop(poster);
    if (reg) srv_registry_unref(reg);
#undef LT_CHECK
    return error;
}
void test_loom_private_targets(void) {
    const char *error = loom_targets_fixture();
    TEST_ASSERT(!error, error);
}

// Actual native target, caller snapshot, weighted connection admission and
// charged preallocated client. No server thread: preparation must perform no
// transport I/O and must leave the backlog empty. This does not exercise CONNECT
// publication, request binding, handshake progress or retirement of active work.
static const char *loom_protocol_fixture(void) {
    const char *error = NULL;
    struct Proc *p = test_proc_make(), *poster = test_proc_make();
    struct SrvRegistry *reg = NULL;
    struct Spoor *root = NULL;
    struct Loom *l = NULL;
    struct loom_service_protocol *prepared = NULL;
    struct loom_service_ref refs[2] = {{0},{0}};
    u64 goal = loom_private_retired();
    u32 base = p ? p->as->page_count : 0;
    u32 old_budget = p ? p->as->page_budget : 0;
    bool owner_dropped = false;
#define PO(c,m) do { if (!(c)) { error=m; goto done; } } while (0)
    PO(p && poster,"protocol fixture processes");
    p->principal_id = 42;
    proc_mark_may_post_service(poster);
    reg = srv_registry_create();
    PO(reg,"protocol fixture registry");
    root = devsrv_attach_registry(reg);
    PO(root,"protocol navigation root");
    root->flag |= CWALKONLY;
    const char *names[2] = {"small","large"};
    for (u32 n=0;n<2;++n)
        PO(devsrv_post_listener(poster,root,names[n],5,SRV_MODE_9P,n==1,false,false)>=0,
           "protocol strict native posts");
    spoor_ref(root);
    int fd = handle_alloc(p,KOBJ_SPOOR,RIGHT_READ,root);
    if (fd < 0) spoor_clunk(root);
    PO(fd>=0,"protocol navigation descriptor");
    l=loom_create_private(p,2,2,true);
    if (l) ++goal;
    PO(l,"protocol private owner");
    for (u32 n=0;n<2;++n) {
        struct loom_service_target in = { .size=sizeof(in), .version=1,
                                          .registry_fd=fd, .name_len=5 };
        for (u32 i=0;i<5;++i) in.name[i]=names[n][i];
        PO(!loom_private_target_prepare(l,p,&in,&refs[n]),"protocol target prepare");
        PO(!loom_private_target_publish(l,refs[n]),"protocol target publish");
    }
    u32 owner_pages = p->as->page_count;
    u64 freed = srvconn_total_freed();
    loom_private_protocol_fault_arm_for_test(1);
    PO(loom_private_protocol_prepare(l,refs[0],10,10,&prepared)==-T_E_TIMEDOUT &&
       !prepared && !loom_private_protocol_fault_pending_for_test(),
       "expired preparation consumes fault without ownership");
    PO(srvconn_total_freed()==freed && p->as->page_count==owner_pages,
       "expired preparation allocates no connection or pages");
    p->state=PROC_STATE_ZOMBIE;
    int gone = loom_private_protocol_prepare(l,refs[0],0,0,&prepared);
    p->state=PROC_STATE_ALIVE;
    PO(gone==-T_E_ACCES && !prepared,"dead creator cannot borrow worker authority");
    for (u32 n=0;n<2;++n) {
        u32 msize = n ? SRVCONN_BULK_MSIZE : SRVCONN_MSIZE;
        u32 expected = (u32)burrow_backing_pages(sizeof(*prepared)) +
                       (u32)burrow_backing_pages(msize)*2u;
        struct SrvService *svc = srv_lookup_in(reg,names[n],5);
        for (u32 stage=1;stage<=3u;++stage) {
            loom_private_protocol_fault_arm_for_test(stage);
            PO(loom_private_protocol_prepare(l,refs[n],1000,1,&prepared)==-T_E_NOMEM &&
               !prepared && !loom_private_protocol_fault_pending_for_test(),
               "protocol allocation refusal leaves output untouched");
            ++freed;
            PO(srvconn_total_freed()==freed,"protocol failure frees native endpoint");
            PO(p->as->page_count==owner_pages && srv_backlog_depth(svc)==0,
               "protocol failure refunds complete charge without admission");
        }
        spin_lock(&p->as->lock);p->as->page_budget=owner_pages;spin_unlock(&p->as->lock);
        int limited=loom_private_protocol_prepare(l,refs[n],0,0,&prepared);
        spin_lock(&p->as->lock);p->as->page_budget=old_budget;spin_unlock(&p->as->lock);
        ++freed;
        PO(srvconn_total_freed()==freed,"budget refusal frees native endpoint");
        PO(limited==-T_E_NOMEM && !prepared && p->as->page_count==owner_pages,
           "captured ordinary principal pays image budget");
        PO(!loom_private_protocol_prepare(l,refs[n],1000,1,&prepared),
           "real preallocated protocol prepare");
        PO(prepared && prepared->identity.stripes==p->stripes &&
           prepared->identity.access.principal_id==42 &&
           prepared->admission.conn->peer_stripes==p->stripes &&
           prepared->admission.conn->peer_pid==p->pid,
           "protocol and native connection capture actual caller");
        PO(prepared->owner==l && !prepared->admission.published &&
           !srv_backlog_depth(svc) && !prepared->admission.conn->c2s.count &&
           !prepared->admission.conn->s2c.count,
           "preparation publishes no backlog or protocol bytes");
        PO(prepared->charged_pages==expected && p->as->page_count==owner_pages+expected,
           "entire owner and msize storage charge precedes allocation");
        PO(prepared->client.progress==&prepared->progress &&
           prepared->client.out_buf_cap==msize && !prepared->client.out_buf_owned &&
           prepared->client.transport.recv_buf==prepared->recv_storage &&
           prepared->client.out_buf==prepared->send_storage &&
           prepared->client.out_buf!=prepared->client.out_buf_inline,
           "exclusive client uses charged provided storage without degradation");
        PO(prepared->client.transport.ops.send(NULL,NULL,0)<0 &&
           prepared->client.transport.ops.recv(NULL,NULL,0)<0 &&
           prepared->client.transport.ops.close(NULL)<0,
           "legacy transport access fails locally");
        // This is a guard test, not an actual publication or retirement witness.
        prepared->admission.published=true;
        int busy=loom_private_protocol_release_unused(prepared);
        prepared->admission.published=false;
        PO(busy==-T_E_BUSY && p->as->page_count==owner_pages+expected,
           "unused cleanup cannot assert admitted protocol quiescence");
        PO(!loom_private_protocol_release_unused(prepared),"unused protocol releases locally");
        prepared=NULL;
        ++freed;
        PO(srvconn_total_freed()==freed,"unused release frees native endpoint");
        PO(p->as->page_count==owner_pages && !srv_backlog_depth(svc),
           "unused protocol refunds exact image after storage release");
    }
    PO(!loom_private_protocol_prepare(l,refs[0],0,0,&prepared),"last owner protocol prepare");
    loom_exec_latch(l);
    struct loom_service_protocol *refused=NULL;
    PO(loom_private_protocol_prepare(l,refs[0],0,0,&refused)<0 && !refused,
       "close refuses new protocol construction");
    loom_unref(l);owner_dropped=true;
    PO(loom_private_retired()<goal && p->as->private_rings,
       "prepared protocol retains image and closing owner");
    PO(!loom_private_protocol_release_unused(prepared),"last protocol drops owner last");
    prepared=NULL;l=NULL;
    PO(lp_wait(goal) && p->as->page_count==base && !p->as->private_rings,
       "final protocol owner retires and refunds ring");
done:
    loom_private_protocol_fault_arm_for_test(0);
    if (p) { spin_lock(&p->as->lock);p->as->page_budget=old_budget;spin_unlock(&p->as->lock); }
    if (prepared) (void)loom_private_protocol_release_unused(prepared);
    if (l && !owner_dropped) loom_unref(l);
    if (!lp_wait(goal) && !error) error="protocol cleanup retirement timeout";
    if (root) spoor_clunk(root);
    test_proc_drop(p);test_proc_drop(poster);
    if (reg) srv_registry_unref(reg);
#undef PO
    return error;
}
void test_loom_private_protocol(void);
void test_loom_private_protocol(void) {
    const char *error=loom_protocol_fixture();
    TEST_ASSERT(!error,error);
}
