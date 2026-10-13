#include <thylacine/page.h>
#include <thylacine/loom_service_frontend.h>
#include <thylacine/loom_service_fids.h>
#include <thylacine/loom_service_operation.h>
#include <thylacine/loom_service_control.h>
#include <thylacine/loom_service_connect.h>
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
void test_loom_setup_resource_refusals(void);
void test_loom_register_replaces(void);
void test_loom_post_cqe_back_pressure(void);
void test_loom_post_cqe_ignores_hostile_header(void);
void test_loom_dup_rejected(void);
void test_loom_enter_nop(void);
void test_loom_sq_release_after_copy(void);
void test_loom_sq_release_after_index(void);
void test_loom_sq_release_after_copy_chain(void);
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
#include "loom_bridge_fixture.h"

void test_loom_private_bridge(void);
void test_loom_private_bridge(void) {
    const char *error = loom_bridge_fixture_run();
    TEST_ASSERT(!error, error);
}
void test_loom_setup_rollback_settles(void);
void test_loom_setup_rollback_settles(void) {
    const char *error = loom_setup_rollback_fixture_run();
    TEST_ASSERT(!error, error);
}
#include "loom_slots_fixture.h"
#include "loom_nodes_fixture.h"

void test_loom_service_nodes(void);
void test_loom_service_nodes(void) {
    const char *error = loom_nodes_fixture();
    TEST_ASSERT(!error, error);
}

#include "loom_fids_fixture.h"
void test_loom_service_fid_reap(void);
void test_loom_service_fid_reap(void) {
    const char *error = loom_fids_fixture();
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
             (u32)burrow_backing_pages(sizeof(struct loom_service_target_table)) + 3u,
             "slot pool target node request and index pages included in metadata charge");
    SO_CHECK(l->service_engine && loom_engine_empty(l->service_engine),
             "private owner owns a zeroed protocol index page");
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
    TEST_EXPECT_EQ(sys_loom_setup_for_proc(p, 0, 0, &kp, &fd), -T_E_INVAL,
                   "entries 0 answers -EINVAL");
    TEST_EXPECT_EQ(sys_loom_setup_for_proc(p, 3, 0, &kp, &fd), -T_E_INVAL,
                   "non-pow2 answers -EINVAL");
    TEST_EXPECT_EQ(sys_loom_setup_for_proc(p, LOOM_MAX_ENTRIES * 2u, 0, &kp, &fd), -T_E_INVAL,
                   "over-max answers -EINVAL");
    TEST_EXPECT_EQ(sys_loom_setup_for_proc(p, 8, LOOM_SETUP_CQSIZE, &kp, &fd), -T_E_INVAL,
                   "still-reserved LOOM_SETUP_CQSIZE flag answers -EINVAL");
    TEST_EXPECT_EQ(sys_loom_setup_for_proc(p, 8, 0x80000000u, &kp, &fd), -T_E_INVAL,
                   "unknown high setup-flag bit answers -EINVAL");
    TEST_EXPECT_EQ(sys_loom_setup_for_proc(NULL, 8, 0, &kp, &fd), -T_E_INVAL,
                   "NULL proc answers -EINVAL");

    test_proc_drop(p);
}

// The two resource refusals a plain setup can meet after the ring exists: each
// answers its own errno and reclaims the ring, pages included.
void test_loom_setup_resource_refusals(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "proc_alloc returned NULL");
    p->principal_id = 1000u;                  // a real user -- NOT I-32 exempt
    struct loom_params kp;
    hidx_t fd = -1;
    u64 destroyed0 = loom_total_destroyed();

    u32 pages0 = p->as->page_count;
    p->as->page_count = p->as->page_budget;
    int cap_rc = sys_loom_setup_for_proc(p, 8, 0, &kp, &fd);
    u64 cap_freed = loom_total_destroyed() - destroyed0;
    p->as->page_count = pages0;

    hidx_t rd = -1, wr = -1;
    int pipe_rc = sys_pipe_for_proc(p, &rd, &wr);
    for (int i = 0; pipe_rc == 0 && i < PROC_HANDLE_MAX; i++)
        if (handle_dup(p, rd, RIGHT_READ) < 0) break;
    u32 pages1 = p->as->page_count;
    int full_rc = sys_loom_setup_for_proc(p, 8, 0, &kp, &fd);
    u64 full_freed = loom_total_destroyed() - destroyed0;
    u32 pages2 = p->as->page_count;
    test_proc_drop(p);

    TEST_EXPECT_EQ(cap_rc, -T_E_NOMEM, "the page cap answers -ENOMEM");
    TEST_EXPECT_EQ(cap_freed, (u64)1, "the ring refused at the page cap was reclaimed");
    TEST_EXPECT_EQ(pipe_rc, 0, "pipe pair");
    TEST_EXPECT_EQ(full_rc, -T_E_MFILE, "a full handle table answers -EMFILE");
    TEST_EXPECT_EQ(full_freed, (u64)2, "the ring refused for a slot was reclaimed");
    TEST_EXPECT_EQ((u64)pages2, (u64)pages1, "and its pages were refunded");
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
                   -T_E_INVAL, "LOOM_REGISTER_BUFFERS through the HANDLES entry answers -EINVAL");
    // nargs over the table cap.
    TEST_EXPECT_EQ(sys_loom_register_for_proc(p, loom_fd, LOOM_REGISTER_HANDLES, one,
                                              LOOM_MAX_REG_HANDLES + 1u),
                   -T_E_INVAL, "nargs over cap answers -EINVAL");
    // A listed descriptor that is not a Spoor (the loom fd itself), or names
    // nothing, answers -EBADF.
    hidx_t bad[1] = { loom_fd };
    TEST_EXPECT_EQ(sys_loom_register_for_proc(p, loom_fd, LOOM_REGISTER_HANDLES, bad, 1),
                   -T_E_BADF, "a listed non-Spoor answers -EBADF");
    hidx_t dead[2] = { rd, (hidx_t)999 };
    TEST_EXPECT_EQ(sys_loom_register_for_proc(p, loom_fd, LOOM_REGISTER_HANDLES, dead, 2),
                   -T_E_BADF, "a listed descriptor naming nothing answers -EBADF");
    TEST_EXPECT_EQ(sys_loom_enter_for_proc(p, loom_fd, 0, 0, 0x80000000u), -T_E_INVAL,
                   "ENTER with an unknown flag bit answers -EINVAL");
    // A bogus loom_fd is rejected.
    TEST_EXPECT_EQ(sys_loom_register_for_proc(p, (hidx_t)999, LOOM_REGISTER_HANDLES, one, 1),
                   -T_E_BADF, "bad loom_fd answers -EBADF");
    // A descriptor naming no Loom answers -EBADF too, and so does ENTER: a
    // client tells a ring closed under it from its own error by this answer.
    TEST_EXPECT_EQ(sys_loom_register_for_proc(p, rd, LOOM_REGISTER_HANDLES, one, 1),
                   -T_E_BADF, "non-Loom loom_fd answers -EBADF");
    TEST_EXPECT_EQ(sys_loom_enter_for_proc(p, (hidx_t)999, 0, 0, 0), -T_E_BADF,
                   "ENTER on a bad descriptor answers -EBADF");
    TEST_EXPECT_EQ(sys_loom_enter_for_proc(p, rd, 0, 0, 0), -T_E_BADF,
                   "ENTER on a non-Loom descriptor answers -EBADF");

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

// A sibling thread closes the new fd the instant setup installs it. That close
// drops the handle's reference; setup must still hold its own, so the Loom
// survives until setup's last touch, and setup's own drop then frees it.
static bool setup_race_closed;
static u64  setup_race_destroyed_at_close;
static void setup_race_close(struct Proc *p, hidx_t fd) {
    setup_race_closed = handle_close(p, fd) == 0;
    setup_race_destroyed_at_close = loom_total_destroyed();
}
void test_loom_setup_close_race(void);
void test_loom_setup_close_race(void) {
    struct Proc *p = test_proc_make();
    TEST_ASSERT(p != NULL, "proc_alloc");
    u32 base = p->as->page_count;
    u64 before = loom_total_destroyed();
    struct loom_params kp;
    hidx_t fd = -1;
    setup_race_closed = false;
    setup_race_destroyed_at_close = 0;
    loom_setup_installed_hook_for_test(setup_race_close);
    int rc = sys_loom_setup_for_proc(p, 4, 0, &kp, &fd);
    loom_setup_installed_hook_for_test(NULL);
    u64 after = loom_total_destroyed();
    // The ring mapping outlives the Loom; its detach is the final drop.
    s64 detached = rc == 0 ? sys_burrow_detach_for_proc(p, kp.ring_va, kp.ring_size) : -1;
    u32 settled = p->as->page_count;
    test_proc_drop(p);
    TEST_EXPECT_EQ(rc, 0, "setup completes although its fd was closed at install");
    TEST_ASSERT(setup_race_closed, "the racing close found the freshly installed fd");
    TEST_EXPECT_EQ(setup_race_destroyed_at_close, before,
                   "the Loom outlives a close that lands at install");
    TEST_EXPECT_EQ(after, before + 1, "setup's own last drop frees the closed Loom");
    TEST_EXPECT_EQ(detached, 0, "the orphaned ring mapping detaches");
    TEST_EXPECT_EQ(settled, base, "the detach settles the ring charge exactly");
}

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
                   LOOM_MAX_REG_BUFFERS + 1u), -T_E_INVAL, "n over cap answers -EINVAL");
    TEST_EXPECT_EQ(sys_loom_register_buffers_for_proc(p, loom_fd, &zlen, 1), -T_E_INVAL,
                   "len 0 answers -EINVAL");
    TEST_EXPECT_EQ(sys_loom_register_buffers_for_proc(p, loom_fd, &oob, 1), -T_E_INVAL,
                   "OOB len answers -EINVAL");
    TEST_EXPECT_EQ(sys_loom_register_buffers_for_proc(p, loom_fd, &um, 1), -T_E_INVAL,
                   "unmapped VA answers -EINVAL");
    TEST_EXPECT_EQ(sys_loom_register_buffers_for_proc(p, (hidx_t)999, &one, 1), -T_E_BADF,
                   "BUFFERS on a bad loom_fd answers -EBADF");
    hidx_t rd = -1, wr = -1;
    TEST_ASSERT(sys_pipe_for_proc(p, &rd, &wr) == 0, "pipe pair");
    TEST_EXPECT_EQ(sys_loom_register_buffers_for_proc(p, rd, &one, 1), -T_E_BADF,
                   "BUFFERS on a non-Loom loom_fd answers -EBADF");

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

// A one-entry SQ whose well-behaved producer refills the position the instant
// sq_head is published (the hook runs inside loom_sq_release). The kernel must
// have read the index and copied the SQE by then. Deterministic stand-in for an
// SQPOLL or sibling-thread producer; it does not exercise userspace scheduling.
struct sq_refill_obs {
    u32 visits, dropped, tail1, tail2, head;
    int n1, n2;
    u64 first_ud, refill_ud;
    s32 first_res;
};
// Atomic: while armed, the hook also runs from any other ring's SQPOLL drain.
static struct Loom *sq_refill_target;
static u32 sq_refill_visits;
static void sq_refill_producer(struct Loom *l) {
    if (l != __atomic_load_n(&sq_refill_target, __ATOMIC_ACQUIRE) || sq_refill_visits++) return;
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    loom_stage_sqe(l, 0, LOOM_OP_NOP, 0, 0, 0, 0x222ULL);
    __atomic_store_n(&h->sq_tail, 2u, __ATOMIC_RELEASE);
}
// Observes everything and drops the Loom before the caller asserts anything.
static bool sq_refill_run(bool bad_index, u8 flags, struct sq_refill_obs *o) {
    struct Loom *l = loom_create(1, 2, false);
    if (!l) return false;
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);
    u32 *sqa = (u32 *)(l->ring_kva + l->sq_array_off);
    loom_stage_sqe(l, 0, LOOM_OP_NOP, flags, 0, 0, 0x111ULL);
    if (bad_index) sqa[0] = 7;
    __atomic_store_n(&h->sq_tail, 1u, __ATOMIC_RELEASE);

    sq_refill_visits = 0;
    __atomic_store_n(&sq_refill_target, l, __ATOMIC_RELEASE);
    loom_sq_release_hook_for_test(sq_refill_producer);
    o->n1 = loom_enter(l, 1, 0, LOOM_ENTER_NONBLOCK);
    loom_sq_release_hook_for_test(NULL);
    __atomic_store_n(&sq_refill_target, NULL, __ATOMIC_RELEASE);
    o->visits    = sq_refill_visits;
    o->dropped   = h->dropped;
    o->tail1     = l->cq_tail;
    o->first_ud  = cqes[0].user_data;
    o->first_res = cqes[0].result;
    o->n2        = loom_enter(l, 1, 0, LOOM_ENTER_NONBLOCK);
    o->tail2     = l->cq_tail;
    o->refill_ud = cqes[bad_index ? 0 : 1].user_data;
    o->head      = h->sq_head;
    loom_unref(l);
    return true;
}

// A valid original completes with its own cookie, then the refill with its own.
void test_loom_sq_release_after_copy(void) {
    struct sq_refill_obs o;
    TEST_ASSERT(sq_refill_run(false, 0, &o), "loom_create(1,2)");
    TEST_EXPECT_EQ(o.visits, 1u, "the producer refilled at the first sq_head release");
    TEST_EXPECT_EQ(o.n1, 1, "the original SQE is consumed");
    TEST_EXPECT_EQ(o.dropped, 0u, "nothing dropped");
    TEST_EXPECT_EQ(o.tail1, 1u, "the original posts one CQE");
    TEST_EXPECT_EQ(o.first_ud, (u64)0x111ULL, "the original SQE was copied before its release");
    TEST_EXPECT_EQ(o.first_res, 0, "the original NOP result");
    TEST_EXPECT_EQ(o.n2, 1, "the refill is consumed by the next enter");
    TEST_EXPECT_EQ(o.tail2, 2u, "the refill posts exactly one CQE");
    TEST_EXPECT_EQ(o.refill_ud, (u64)0x222ULL, "the refill keeps its own cookie");
    TEST_EXPECT_EQ(o.head, 2u, "each position released exactly once");
}

// The same with a DRAIN original, so it is routed through the LINK/DRAIN chain:
// the held entry must carry the copy, not a later read of the refilled slot.
void test_loom_sq_release_after_copy_chain(void) {
    struct sq_refill_obs o;
    TEST_ASSERT(sq_refill_run(false, LOOM_SQE_DRAIN, &o), "loom_create(1,2)");
    TEST_EXPECT_EQ(o.visits, 1u, "the producer refilled at the first sq_head release");
    TEST_EXPECT_EQ(o.n1, 1, "the chain-routed original SQE is consumed");
    TEST_EXPECT_EQ(o.dropped, 0u, "nothing dropped");
    TEST_EXPECT_EQ(o.tail1, 1u, "the chain-routed original posts one CQE");
    TEST_EXPECT_EQ(o.first_ud, (u64)0x111ULL, "the chain-routed original SQE was copied before its release");
    TEST_EXPECT_EQ(o.first_res, 0, "the chain-routed original NOP result");
    TEST_EXPECT_EQ(o.n2, 1, "the refill is consumed by the next enter");
    TEST_EXPECT_EQ(o.tail2, 2u, "the refill posts exactly one CQE");
    TEST_EXPECT_EQ(o.refill_ud, (u64)0x222ULL, "the refill keeps its own cookie");
    TEST_EXPECT_EQ(o.head, 2u, "each position released exactly once");
}

// An out-of-range original index stays dropped: the refill's valid index, written
// after the release, must not be read in its place.
void test_loom_sq_release_after_index(void) {
    struct sq_refill_obs o;
    TEST_ASSERT(sq_refill_run(true, 0, &o), "loom_create(1,2)");
    TEST_EXPECT_EQ(o.visits, 1u, "the producer refilled at the first sq_head release");
    TEST_EXPECT_EQ(o.n1, 0, "an out-of-range original index is not consumed as the refill");
    TEST_EXPECT_EQ(o.dropped, 1u, "the out-of-range original index is counted dropped");
    TEST_EXPECT_EQ(o.tail1, 0u, "the dropped original posts no CQE");
    TEST_EXPECT_EQ(o.n2, 1, "the refill is consumed by the next enter");
    TEST_EXPECT_EQ(o.tail2, 1u, "the refill posts exactly one CQE");
    TEST_EXPECT_EQ(o.refill_ud, (u64)0x222ULL, "the refill keeps its own cookie");
    TEST_EXPECT_EQ(o.head, 2u, "each position released exactly once");
}

// The same handoff through the private frontend (loom_private_submit). A NOP is
// not a private opcode, so each consumed entry earns exactly one refusal CQE
// carrying the user_data that was copied -- the copy is what the CQE reports.
static bool sq_private_refill_run(bool bad_index, struct sq_refill_obs *o) {
    struct Proc *p = test_proc_make();
    if (!p) return false;
    u64 goal = loom_private_retired();
    struct Loom *l = loom_create_private(p, 1, 2, false);
    if (!l) { test_proc_drop(p); return false; }
    ++goal;
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    struct loom_cqe *cqes = (struct loom_cqe *)(l->ring_kva + l->cqe_off);
    u32 *sqa = (u32 *)(l->ring_kva + l->sq_array_off);
    loom_stage_sqe(l, 0, LOOM_OP_NOP, 0, 0, 0, 0x111ULL);
    if (bad_index) sqa[0] = 7;
    __atomic_store_n(&h->sq_tail, 1u, __ATOMIC_RELEASE);

    sq_refill_visits = 0;
    __atomic_store_n(&sq_refill_target, l, __ATOMIC_RELEASE);
    loom_sq_release_hook_for_test(sq_refill_producer);
    o->n1 = loom_private_submit(l, p, 1);
    loom_sq_release_hook_for_test(NULL);
    __atomic_store_n(&sq_refill_target, NULL, __ATOMIC_RELEASE);
    o->visits    = sq_refill_visits;
    o->dropped   = h->dropped;
    o->tail1     = l->cq_tail;
    o->first_ud  = cqes[0].user_data;
    o->first_res = cqes[0].result;
    o->n2        = loom_private_submit(l, p, 1);
    o->tail2     = l->cq_tail;
    o->refill_ud = cqes[bad_index ? 0 : 1].user_data;
    o->head      = h->sq_head;
    loom_unref(l);
    bool retired = lp_wait(goal);
    test_proc_drop(p);
    return retired;
}

void test_loom_private_sq_release_after_copy(void);
void test_loom_private_sq_release_after_copy(void) {
    struct sq_refill_obs o;
    TEST_ASSERT(sq_private_refill_run(false, &o), "private ring made and retired");
    TEST_EXPECT_EQ(o.visits, 1u, "the producer refilled at the first private sq_head release");
    TEST_EXPECT_EQ(o.n1, 1, "the private original SQE is consumed");
    TEST_EXPECT_EQ(o.dropped, 0u, "nothing dropped");
    TEST_EXPECT_EQ(o.tail1, 1u, "the private original earns one refusal CQE");
    TEST_EXPECT_EQ(o.first_ud, (u64)0x111ULL,
                   "the private original SQE was copied before its release");
    TEST_EXPECT_EQ(o.first_res, -T_E_INVAL, "a NOP is refused on a private ring");
    TEST_EXPECT_EQ(o.n2, 1, "the refill is consumed by the next submit");
    TEST_EXPECT_EQ(o.tail2, 2u, "the refill earns exactly one refusal CQE");
    TEST_EXPECT_EQ(o.refill_ud, (u64)0x222ULL, "the refill keeps its own cookie");
    TEST_EXPECT_EQ(o.head, 2u, "each private position released exactly once");
}

void test_loom_private_sq_release_after_index(void);
void test_loom_private_sq_release_after_index(void) {
    struct sq_refill_obs o;
    TEST_ASSERT(sq_private_refill_run(true, &o), "private ring made and retired");
    TEST_EXPECT_EQ(o.visits, 1u, "the producer refilled at the first private sq_head release");
    TEST_EXPECT_EQ(o.n1, 1, "the out-of-range private position is still claimed");
    TEST_EXPECT_EQ(o.dropped, 1u, "the out-of-range private original index is counted dropped");
    TEST_EXPECT_EQ(o.tail1, 0u, "a dropped private original earns no CQE, not the refill's");
    TEST_EXPECT_EQ(o.n2, 1, "the refill is consumed by the next submit");
    TEST_EXPECT_EQ(o.tail2, 1u, "the refill earns exactly one refusal CQE");
    TEST_EXPECT_EQ(o.refill_ud, (u64)0x222ULL, "the refill keeps its own cookie");
    TEST_EXPECT_EQ(o.head, 2u, "each private position released exactly once");
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
    TEST_EXPECT_EQ(sys_loom_setup_for_proc(p, 8, LOOM_SETUP_SQPOLL, &kp, &fd2), -T_E_AGAIN,
                   "SQPOLL setup at the thread cap answers -EAGAIN");
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
extern void proc_test_link(struct Proc *p);
extern void proc_test_unlink(struct Proc *p);
// Real admission and real preallocated handshake. The canned responder only
// supplies wire replies; READY, request completion and retirement use production
// code. CQ backpressure must preserve both success and cancelled obligations.
int canonical_responder(void *, const u8 *, size_t, u8 *, size_t);
static int lc_reply(struct SrvConn *cn, u8 type, u16 tag) {
    u8 req[7]={7,0,0,0,type,(u8)tag,(u8)(tag>>8)}, resp[64];
    int n=canonical_responder(NULL,req,sizeof(req),resp,sizeof(resp));
    return n>0 && srvconn_server_send(cn,resp,n)==n ? 0 : -1;
}
static const char *loom_connect_cases(struct Loom *l, struct Proc *creator,
                                      struct Proc *poster, struct SrvRegistry *reg,
                                      struct loom_service_ref targets[2]) {
    const char *error=NULL;
    struct loom_service_protocol *p=NULL, *duplicate=NULL;
    struct SrvConn *server=NULL;
    struct loom_service_ref scope={0};
    struct loom_service_reserve reserve={.size=sizeof(reserve),.version=1,.kind=LOOM_SERVICE_SCOPE};
    struct loom_ring_hdr *h=(void *)(l->ring_kva+l->hdr_off);
    struct loom_cqe *cq=(void *)(l->ring_kva+l->cqe_off);
    u32 pages=creator->as->page_count;
#define LC(c,m) do { if (!(c)) { error=m; goto done; } } while(0)
    // Success in both classes; cancellation, timeout and last-owner close.
    for (u32 mode=0;mode<6;++mode) {
        LC(!loom_private_node_prepare(l,creator,&reserve,&scope) &&
           !loom_private_node_publish(l,scope),"CONNECT destination reservation");
        if (!mode) {
            loom_private_protocol_fault_arm_for_test(1);
            LC(loom_private_connect_prepare(l,scope,targets[0],77,1000,1,&p)==-T_E_NOMEM && !p &&
               !l->service_requests.occupied && !l->service_slots->slots[scope.slot].pins &&
               creator->as->page_count==pages,"CONNECT failed allocation releases request and both pins");
            LC(!loom_private_connect_prepare(l,scope,targets[0],77,1000,1,&p),"CONNECT expired publisher prepare");
            LC(loom_private_connect_publish(p,1000)==-T_E_TIMEDOUT && !p->admission.published &&
               !p->admission.conn->c2s.count,"CONNECT expired publisher has no peer effects");
            LC(!loom_private_connect_rollback(p),"CONNECT expired publisher rollback");p=NULL;
        }
        LC(!loom_private_connect_prepare(l,scope,targets[mode==1],77,1000,1,&p),"CONNECT bound prepare");
        LC(l->service_slots->slots[scope.slot].pins==2 && l->service_requests.occupied==1,
           "CONNECT request and protocol independently retain destination");
        LC(loom_private_protocol_release_unused(p)==-T_E_BUSY,"CONNECT bound owner cannot bypass cleanup");
        LC(loom_private_connect_prepare(l,scope,targets[0],88,1000,1,&duplicate)==-T_E_BUSY && !duplicate,
           "CONNECT duplicate destination refused");
        LC(!loom_private_connect_publish(p,2),"CONNECT locked admission publishes");
        struct SrvService *svc=srv_lookup_in(reg,mode==1?"large":"small",5);
        LC(srv_backlog_depth(svc)==1 && !p->admission.conn->c2s.count &&
           l->service_nodes->nodes[scope.slot].state==LOOM_SERVICE_ADMITTED,
           "CONNECT published before first byte with reserved terminal");
        server=srv_accept_blocking(svc,poster->stripes);
        LC(server==p->admission.conn,"CONNECT native server endpoint accepted");
        if (mode<2) {
            LC(!lc_reply(server,P9_TVERSION,P9_NOTAG) && !lc_reply(server,P9_TATTACH,0),
               "CONNECT handshake wire replies");
            int rc=0;
            for (u32 visit=0;visit<32 && !rc;++visit) rc=loom_private_connect_step(p,3+visit);
            LC(rc==1 && p->progress.handshake.phase==P9_HS_READY &&
               l->service_nodes->nodes[scope.slot].state==LOOM_SERVICE_READY,
               "CONNECT actual version and attach establish READY");
            LC(loom_private_connect_step(p,1001)==1 && !p->abort_reason,
               "CONNECT deadline ends after committed success");
        }
        if (mode==4) {
            // Real Rlerror during version negotiation; ABI maps protocol failure
            // to EIO while retaining the attempted handshake phase.
            u8 denied[11]={11,0,0,0,P9_RLERROR,255,255,T_E_ACCES,0,0,0};
            LC(srvconn_server_send(server,denied,sizeof(denied))==(long)sizeof(denied),
               "CONNECT stages protocol error");
            int rc=0;
            for (u32 visit=0;visit<32 && !rc;++visit) rc=loom_private_connect_step(p,3+visit);
            LC(rc==-T_E_IO && p->locally_retired && p->failure_phase==P9_HS_VERSION_RECV,
               "CONNECT protocol failure records version phase and retires");
        } else if (mode==3) {
            LC(loom_private_connect_step(p,1000)==-T_E_TIMEDOUT && p->locally_retired,
               "CONNECT deadline retires incomplete handshake");
        } else {
            if (mode==5) loom_exec_latch(l);
            else {
                LC(!loom_private_connect_abort(p,-T_E_CANCELED) && !p->locally_retired &&
                   l->service_nodes->nodes[scope.slot].state==LOOM_SERVICE_ABORTING,
                   "CONNECT abort acknowledgement is not local retirement");
                LC(loom_private_connect_release(p)==-T_E_BUSY,"CONNECT refuses early protocol free");
                LC(loom_private_connect_abort(p,-T_E_IO)==1 && p->abort_reason==-T_E_CANCELED,
                   "CONNECT preserves first abort reason");
            }
            LC(loom_private_connect_step(p,10)==-T_E_CANCELED && p->locally_retired &&
               l->service_nodes->nodes[scope.slot].state==LOOM_SERVICE_RETIRED,
               "CONNECT cursor quiescence establishes local retirement");
        }
        struct loom_service_request *r=&l->service_requests.records[p->request.index];
        s32 expected=mode<2?(s32)scope.slot:mode==3?-T_E_TIMEDOUT:mode==4?-T_E_IO:-T_E_CANCELED;
        LC(r->phase==LREQ_TERMINAL && r->result==expected && !r->borrows,
           "CONNECT terminal success or cancellation immutable after retirement");
        // Fill the two-slot CQ with ordinary prior scalar completions.
        LC(!loom_post_cqe(l,1,0,0) && !loom_post_cqe(l,2,0,0),"CONNECT prior CQ fill");
        LC(loom_private_post_terminal(l,p->request)==-T_E_AGAIN && r->phase==LREQ_TERMINAL &&
           loom_private_connect_release(p)==-T_E_BUSY,"CONNECT full CQ retains terminal and protocol owner");
        h->cq_head=l->cq_tail;
        u32 at=l->cq_tail & (l->cq_entries-1);
        LC(!loom_private_post_terminal(l,p->request) && cq[at].user_data==77 && cq[at].result==expected,
           "CONNECT retained terminal posts exact result");
        LC(loom_private_connect_reap(l,scope)==-T_E_BUSY,"CONNECT retained protocol prevents scope reap");
        u64 freed=srvconn_total_freed();
        LC(!loom_private_connect_release(p),"CONNECT retired owner release");p=NULL;
        LC(creator->as->page_count==pages && !l->service_requests.occupied &&
           srvconn_total_freed()==freed,"CONNECT local storage refunded while server retains native endpoint");
        srvconn_unref(server);server=NULL;
        LC(srvconn_total_freed()==freed+1,"CONNECT last server endpoint releases native connection");
        LC(!loom_private_connect_reap(l,scope) && loom_private_connect_reap(l,scope)==-T_E_NOENT,
           "CONNECT exact retired scope reaps once");scope=(struct loom_service_ref){0};
        h->cq_head=l->cq_tail;
    }
done:
    loom_private_protocol_fault_arm_for_test(0);
    if (p) {
        if (!p->admission.published) (void)loom_private_connect_rollback(p);
        else {
            (void)loom_private_connect_abort(p,-T_E_CANCELED);
            (void)loom_private_connect_step(p,0);
            h->cq_head=l->cq_tail;
            (void)loom_private_post_terminal(l,p->request);
            (void)loom_private_connect_release(p);
        }
    }
    if (server) srvconn_unref(server);
    if (scope.incarnation) {
        (void)loom_private_connect_reap(l,scope);
        (void)loom_private_node_reap_unused(l,scope);
    }
#undef LC
    return error;
}

// Native ring index keeps two real peers independent and pins entry lifetime
// through terminal delivery, abort and CQ-independent whole-owner discard.
static const char *loom_engine_cases(struct Loom *l, struct Proc *creator,
                                     struct Proc *poster, struct SrvRegistry *reg,
                                     struct loom_service_ref target[2]) {
    const char *error=NULL;
    struct loom_service_ref scope[2]={{0},{0}};
    struct loom_service_protocol *p[2]={NULL,NULL},*held[2]={NULL,NULL};
    struct SrvConn *server[2]={NULL,NULL};
    struct loom_service_reserve reserve={.size=sizeof(reserve),.version=1,.kind=LOOM_SERVICE_SCOPE};
    struct loom_ring_hdr *h=(void *)(l->ring_kva+l->hdr_off);
    u32 pages=creator->as->page_count;
#define EI(c,m) do { if (!(c)) { error=m; goto done; } } while(0)
    // An expired indexed publication retains its negative terminal obligation.
    EI(!loom_private_node_prepare(l,creator,&reserve,&scope[0]) &&
       !loom_private_node_publish(l,scope[0]),"index expired scope reservation");
    EI(!loom_private_connect_prepare(l,scope[0],target[0],11,100,1,&p[0]),"index expired prepare");
    EI(!loom_engine_adopt(p[0],100),"index adopts expired publication as owned rejection");p[0]=NULL;
    EI(!loom_engine_get(l,scope[0],&held[0]) && held[0]->abort_reason==-T_E_TIMEDOUT &&
       !held[0]->admission.published,"index failed admission captured without publication");
    EI(loom_engine_visit(l,101)==1 && held[0]->locally_retired &&
       l->service_requests.records[held[0]->request.index].phase==LREQ_DELIVERED,
       "index rejected request is driven and delivered");
    EI(loom_engine_collect(l,scope[0])==-T_E_BUSY,"index entry pin blocks rejected storage free");
    EI(!loom_engine_put(l,scope[0],held[0]),"index rejected entry release");held[0]=NULL;
    EI(!loom_engine_collect(l,scope[0]) && !loom_private_connect_reap(l,scope[0]),
       "index rejected scope settles");scope[0]=(struct loom_service_ref){0};
    h->cq_head=l->cq_tail;
    EI(creator->as->page_count==pages && loom_engine_empty(l->service_engine),
       "index rejection refunds full protocol allocation");
    for (u32 i=0;i<2;++i) {
        EI(!loom_private_node_prepare(l,creator,&reserve,&scope[i]) &&
           !loom_private_node_publish(l,scope[i]),"index peer destination");
        EI(!loom_private_connect_prepare(l,scope[i],target[i],77+i,1000,1,&p[i]),"index peer prepare");
        EI(!loom_engine_adopt(p[i],2),"index publication adopts owner");p[i]=NULL;
        EI(!loom_engine_get(l,scope[i],&held[i]),"index exact peer lookup");
        struct SrvService *svc=srv_lookup_in(reg,i?"large":"small",5);
        EI(srv_backlog_depth(svc)==1,"index native backlog publication");
        server[i]=srv_accept_blocking(svc,poster->stripes);
        EI(server[i]==held[i]->admission.conn,"index real accepted peer");
    }
    EI(!lc_reply(server[1],P9_TVERSION,P9_NOTAG) && !lc_reply(server[1],P9_TATTACH,0),
       "index only second peer supplies handshake replies");
    for (u32 i=0;i<64 && l->service_nodes->nodes[scope[1].slot].state!=LOOM_SERVICE_READY;++i)
        EI(loom_engine_visit(l,3+i)==1,"index round-robin visit");
    EI(l->service_nodes->nodes[scope[0].slot].state==LOOM_SERVICE_VERSION &&
       l->service_nodes->nodes[scope[1].slot].state==LOOM_SERVICE_READY,
       "index stalled peer does not starve independent handshake");
    EI(l->service_requests.records[held[1]->request.index].phase==LREQ_DELIVERED,
       "index successful terminal delivered once");
    EI(!loom_post_cqe(l,123,0,0),"index fills remaining CQ slot");
    EI(!loom_engine_abort(l,scope[0],-T_E_CANCELED) && !loom_engine_abort(l,scope[1],-T_E_CANCELED),
       "index abort independent of CQ space");
    for (u32 i=0;i<4;++i) (void)loom_engine_visit(l,100);
    EI(held[0]->locally_retired && held[1]->locally_retired &&
       l->service_requests.records[held[0]->request.index].phase==LREQ_TERMINAL &&
       l->service_requests.records[held[1]->request.index].phase==LREQ_DELIVERED,
       "index local retirement with pending cancellation and earlier success");
    loom_exec_latch(l);
    EI(loom_engine_discard_closed(l)==-T_E_BUSY && !l->service_requests.detached,
       "index consumer remains attached while lookup entries remain");
    for (u32 i=0;i<2;++i) { EI(!loom_engine_put(l,scope[i],held[i]),"index final entry put");held[i]=NULL; }
    u64 freed=srvconn_total_freed();
    EI(!loom_engine_discard_closed(l) && l->service_requests.detached &&
       loom_engine_empty(l->service_engine) && !l->service_requests.occupied,
       "index close discards CQ-full terminal after actual local quiescence");
    EI(creator->as->page_count==pages && srvconn_total_freed()==freed,
       "index releases charged protocol while both server endpoints survive");
    for (u32 i=0;i<2;++i) {
        srvconn_unref(server[i]);server[i]=NULL;
        EI(!loom_private_connect_reap(l,scope[i]),"index retired metadata reaps");
        struct loom_service_protocol *absent=NULL;
        EI(loom_engine_get(l,scope[i],&absent)==-T_E_NOENT && !absent,"index stale lookup cannot recover freed owner");
        scope[i]=(struct loom_service_ref){0};
    }
    EI(srvconn_total_freed()==freed+2,"index native credits settle at last endpoints");
done:
    for (u32 i=0;i<2;++i) {
        if (held[i]) (void)loom_engine_put(l,scope[i],held[i]);
        if (p[i]) (void)loom_private_connect_rollback(p[i]);
    }
    loom_exec_latch(l);
    for (u32 i=0;i<2;++i) if (scope[i].incarnation) (void)loom_engine_abort(l,scope[i],-T_E_CANCELED);
    for (u32 i=0;i<4;++i) (void)loom_engine_visit(l,1001);
    (void)loom_engine_discard_closed(l);
    for (u32 i=0;i<2;++i) {
        if (server[i]) srvconn_unref(server[i]);
        if (scope[i].incarnation) {
            (void)loom_private_connect_reap(l,scope[i]);
            (void)loom_private_node_reap_unused(l,scope[i]);
        }
    }
#undef EI
    return error;
}

// Real handle-close/forced-exec owner retirement while accepted servers remain
// stalled and the CQ is full. Observer borrow protects only fixture inspection.
static bool loom_close_wait_state(struct Loom *l, u32 wanted) {
    u64 end=timer_now_ns()+5000000000ull;
    while (timer_now_ns()<end) {
        if (loom_private_close_observe_for_test(l,NULL)==wanted) return true;
        sched();
    }
    return false;
}
// Deterministic real-owner interleavings: execution pauses outside Ll while the
// independent retirement thread observes INSTALLING, driving or teardown users.
static struct {
    struct Loom *l;
    struct Proc *creator;
    struct loom_service_ref scope;
    hidx_t fd;
    u32 stage;
    bool seen;
    const char *error;
} close_boundary;
static void loom_close_boundary_hook(struct Loom *l,u32 stage) {
    if(l!=close_boundary.l || stage!=close_boundary.stage)return;
    loom_private_close_hook_for_test(NULL); // one exact owner boundary
    close_boundary.seen=true;
    if(stage==4) {
        // The handle is already closed. Force the actual parked worker to
        // revisit while unused protocol destruction is suspended outside Ll.
        u64 before=0,after=0;
        loom_private_close_observe_for_test(l,&before);
        loom_private_close_changed(l);
        u64 end=timer_now_ns()+5000000000ull;
        u32 phase=LCLOSE_NEW;
        do {
            phase=loom_private_close_observe_for_test(l,&after);
            if(after>before && (phase==LCLOSE_PARKED || phase==LCLOSE_DONE))break;
            sched();
        } while(timer_now_ns()<end);
        spin_lock(&l->lock);
        bool retained=!l->service_requests.detached && l->service_requests.occupied &&
            l->service_slots->slots[close_boundary.scope.slot].pins==2;
        spin_unlock(&l->lock);
        if(after<=before || phase!=LCLOSE_PARKED || !retained)
            close_boundary.error="rollback retains close barrier through physical protocol destruction";
        return;
    }
    if(handle_close(close_boundary.creator,close_boundary.fd)) {
        close_boundary.error="boundary actual handle close";return;
    }
    close_boundary.fd=-1;
    if(!loom_close_wait_state(l,LCLOSE_PARKED)) {
        close_boundary.error="boundary close parks on real suspended owner";return;
    }
    spin_lock(&l->lock);
    struct loom_engine_entry *e=&l->service_engine->entries[close_boundary.scope.slot];
    bool retained=e->protocol && !e->protocol->locally_retired &&
        !l->service_requests.detached && l->service_requests.occupied==1;
    bool held=stage==1 ? e->phase==LENGINE_INSTALLING && e->pins==1 :
        stage==2 ? e->protocol->driving : e->protocol->users!=0;
    spin_unlock(&l->lock);
    if(!retained || !held) {close_boundary.error="boundary retains actual indexed owner and consumer";return;}
    u64 before=0,after=0;loom_private_close_observe_for_test(l,&before);
    for(u32 i=0;i<64;++i)sched();
    if(loom_private_close_observe_for_test(l,&after)!=LCLOSE_PARKED || before!=after)
        close_boundary.error="boundary parked owner cannot self-reschedule";
}
static const char *loom_close_boundary_case(struct Loom **owner,struct Proc *creator,
        struct Proc *poster,struct SrvRegistry *reg,struct loom_service_ref target,
        u32 stage,u64 goal,u32 baseline) {
    const char *error=NULL;struct Loom *l=*owner;
    struct loom_service_ref scope={0};
    struct loom_service_protocol *p=NULL,*held=NULL;
    struct SrvConn *server=NULL;struct Handle observer={0};
    struct loom_service_reserve reserve={.size=sizeof(reserve),.version=1,.kind=LOOM_SERVICE_SCOPE};
    close_boundary.l=l;close_boundary.creator=creator;close_boundary.fd=-1;
    close_boundary.stage=stage;close_boundary.seen=false;close_boundary.error=NULL;
#define BC(c,m) do {if(!(c)){error=m;goto done;}}while(0)
    BC(!loom_private_node_prepare(l,creator,&reserve,&scope) && !loom_private_node_publish(l,scope),
       "boundary scope reservation");close_boundary.scope=scope;
    BC(!loom_private_connect_prepare(l,scope,target,199,0,0,&p),"boundary actual preparation");
    if(stage!=1) {
        BC(!loom_engine_adopt(p,0),"boundary actual adoption");p=NULL;
        BC(!loom_engine_get(l,scope,&held),"boundary external entry pin");
        server=srv_accept_blocking(srv_lookup_in(reg,"small",5),poster->stripes);
        BC(server==held->admission.conn,"boundary accepted stalled endpoint");
    }
    close_boundary.fd=handle_alloc(creator,KOBJ_LOOM,RIGHT_READ|RIGHT_WRITE,l);
    BC(close_boundary.fd>=0,"boundary handle owns original reference");*owner=NULL;
    BC(!handle_get(creator,close_boundary.fd,&observer),"boundary observer root");
    loom_private_close_hook_for_test(loom_close_boundary_hook);
    if(stage==1) {BC(!loom_engine_adopt(p,0),"closing installer completes ownership transfer");p=NULL;}
    else if(stage==2)(void)loom_engine_visit(l,0);
    else BC(!loom_engine_abort(l,scope,-T_E_CANCELED),"real teardown user returns after close");
    loom_private_close_hook_for_test(NULL);
    BC(close_boundary.seen && !close_boundary.error,close_boundary.error?close_boundary.error:"boundary hook reached");
    if(held){BC(!loom_engine_put(l,scope,held),"boundary releases external entry");held=NULL;}
    BC(loom_close_wait_state(l,LCLOSE_DONE),"last real boundary return resumes close to completion");
    BC(loom_engine_empty(l->service_engine) && !l->service_requests.occupied && l->service_requests.detached,
       "boundary close settled every owner");
    u64 freed=srvconn_total_freed();handle_put(&observer);l=NULL;
    BC(lp_wait(goal) && creator->as->page_count==baseline && !creator->as->private_rings,
       "boundary final observer return refunds exact image");
    BC(!server || srvconn_total_freed()==freed,"boundary peer survives local final reclamation");
done:
    loom_private_close_hook_for_test(NULL);
    if(l) {
        if(held)(void)loom_engine_put(l,scope,held);
        if(p)(void)loom_private_connect_rollback(p);
        if(close_boundary.fd>=0){handle_close(creator,close_boundary.fd);close_boundary.fd=-1;}
        else if(*owner)loom_handle_closed(l);
    }
    if(observer.obj)handle_put(&observer);
    if(server)srvconn_unref(server);
    close_boundary.l=NULL;
#undef BC
    return error;
}

static const char *loom_close_cases(struct Loom **owner, struct Proc *creator,
        struct Proc *poster, struct SrvRegistry *reg, struct loom_service_ref target[2],
        u32 kind, u64 goal, u32 baseline) {
    const char *error=NULL;
    struct Loom *l=*owner,*other=NULL;
    struct loom_service_ref scope[2]={{0},{0}},child={0};
    struct loom_service_protocol *prepared[2]={NULL,NULL},*held[2]={NULL,NULL};
    struct SrvConn *server[2]={NULL,NULL};
    struct Handle observer={0};
    struct loom_private_fixed fixed={0};
    hidx_t fd=-1;
    u64 va=0;
    u32 owner_pages=creator->as->page_count;
    struct loom_service_reserve reserve={.size=sizeof(reserve),.version=1,.kind=LOOM_SERVICE_SCOPE};
#define CL(c,m) do {if(!(c)){error=m;goto done;}}while(0)
    for (u32 i=0;i<2;++i) {
        CL(!loom_private_node_prepare(l,creator,&reserve,&scope[i]) &&
           !loom_private_node_publish(l,scope[i]),"close native scope reservation");
        CL(!loom_private_connect_prepare(l,scope[i],target[i],77+i,0,0,&prepared[i]),
           "close native preparation");
        if (kind==6 && i==1) continue; // real constructor owns a request but no index
        CL(!loom_engine_adopt(prepared[i],0),"close native adoption");prepared[i]=NULL;
        CL(!loom_engine_get(l,scope[i],&held[i]),"close native held entry");
        struct SrvService *svc=srv_lookup_in(reg,i?"large":"small",5);
        CL(srv_backlog_depth(svc)==1,"close native backlog");
        server[i]=srv_accept_blocking(svc,poster->stripes);
        CL(server[i]==held[i]->admission.conn,"close native accepted endpoint");
    }
    if (kind!=6) {
        CL(!lc_reply(server[1],P9_TVERSION,P9_NOTAG) && !lc_reply(server[1],P9_TATTACH,0),
           "close second peer handshake replies");
        for (u32 i=0;i<64 && l->service_nodes->nodes[scope[1].slot].state!=LOOM_SERVICE_READY;++i)
            (void)loom_engine_visit(l,i);
        CL(l->service_nodes->nodes[scope[1].slot].state==LOOM_SERVICE_READY,
           "close second peer READY while first stalls");
        reserve.kind=LOOM_SERVICE_FID;reserve.scope=scope[1];
        CL(!loom_private_node_prepare(l,creator,&reserve,&child),"close unused child holds retired parent metadata");
        if (kind==4) CL(!loom_private_node_publish(l,child),"exec native child publication");
        CL(!loom_post_cqe(l,123,0,0),"close fills CQ behind committed success");
    } else {
        CL(!loom_post_cqe(l,123,0,0) && !loom_post_cqe(l,124,0,0),"constructor close full CQ");
    }
    if (kind==5) {
        s64 attached=sys_burrow_attach_for_proc(creator,PAGE_SIZE);
        CL(attached>0,"close fixed backing attach");va=(u64)attached;
        struct loom_buf_reg b={va,PAGE_SIZE};
        struct loom_service_pool_member m={0,0,0,64};
        CL(!loom_private_register_buffers(l,creator,&b,1) &&
           !loom_private_fixed_acquire(l,creator,&m,&fixed),"close fixed accepted pin");
        CL(!sys_burrow_detach_for_proc(creator,va,PAGE_SIZE),"close fixed unmaps caller backing");va=0;
    }
    fd=handle_alloc(creator,KOBJ_LOOM,RIGHT_READ|RIGHT_WRITE,l);
    CL(fd>=0,"close actual handle owns original ring reference");*owner=NULL;
    CL(!handle_get(creator,fd,&observer),"close observer borrower retained");
    if (kind==4) {
        CL(!handle_set_cloexec(creator,fd,false),"exec private CLOEXEC deliberately cleared");
        handle_private_exec_latch(creator);
        CL(loom_private_close_observe_for_test(l,NULL)==LCLOSE_NEW,"exec commit latch starts no scheduler work");
        CL(handle_close_on_exec(creator)==1,"exec forces private handle removal");fd=-1;
    } else {CL(!handle_close(creator,fd),"actual final handle close returns");fd=-1;}
    CL(loom_close_wait_state(l,LCLOSE_PARKED),"close parks on retained local owner");
    spin_lock(&l->lock);
    bool retired=held[0]->locally_retired && (!held[1] || held[1]->locally_retired);
    bool attached=!l->service_requests.detached;
    bool immutable=!held[1] || l->service_requests.records[held[1]->request.index].result==(s32)scope[1].slot;
    spin_unlock(&l->lock);
    CL(retired && attached && immutable,"close ends actual cursors but preserves borrow and earlier success");
    u64 turns=0,after=0;loom_private_close_observe_for_test(l,&turns);
    for (u32 i=0;i<64;++i) sched();
    CL(loom_private_close_observe_for_test(l,&after)==LCLOSE_PARKED && after==turns,
       "parked close consumes no extra retirement turns");
    if(kind==3) {
        // A parked ring must not monopolize the one existing retirer thread.
        u32 charged=creator->as->page_count;
        other=loom_create_private(creator,2,2,true);
        CL(other,"close second ring construction");++goal;
        loom_handle_closed(other);
        CL(loom_close_wait_state(other,LCLOSE_DONE),"another ring completes while first parks");
        loom_unref(other);other=NULL;
        CL(lp_wait(goal-1) && creator->as->page_count==charged,
           "another ring final destruction is not starved by parked close");
        CL(loom_private_close_observe_for_test(l,NULL)==LCLOSE_PARKED,
           "first ring still waits for its own retained entry");
    }
    for (u32 i=0;i<2;++i) if(held[i]) {
        CL(!loom_engine_put(l,scope[i],held[i]),"close returns actual external entry");held[i]=NULL;
    }
    if (kind==5 || kind==6) {
        CL(loom_close_wait_state(l,LCLOSE_PARKED),"close waits for fixed or unindexed owner");
        spin_lock(&l->lock);attached=!l->service_requests.detached;spin_unlock(&l->lock);
        CL(attached,"physical borrower or constructor preserves consumer");
        if (kind==5) CL(!loom_private_fixed_release(l,&fixed),"last fixed return resumes real close worker");
        if (kind==6) {
            close_boundary.l=l;close_boundary.stage=4;close_boundary.scope=scope[1];
            close_boundary.seen=false;close_boundary.error=NULL;
            loom_private_close_hook_for_test(loom_close_boundary_hook);
            int rc=loom_private_connect_rollback(prepared[1]);
            loom_private_close_hook_for_test(NULL);
            if(!rc)prepared[1]=NULL;
            CL(!rc,"unindexed rollback resumes real close worker");
            CL(close_boundary.seen && !close_boundary.error,
               close_boundary.error?close_boundary.error:"rollback destruction boundary reached");
        }
    }
    CL(loom_close_wait_state(l,LCLOSE_DONE),"real close worker completes without peer or CQ progress");
    CL(loom_engine_empty(l->service_engine) && !l->service_requests.occupied &&
       l->service_requests.detached,"close removed every owned protocol and request");
    CL(creator->as->private_rings==1 && creator->as->page_count>=owner_pages,
       "observer retains image guard until its own final return");
    u64 freed=srvconn_total_freed();
    handle_put(&observer);l=NULL;
    CL(lp_wait(goal) && creator->as->page_count==baseline && !creator->as->private_rings,
       "close final free refunds exact creator image and child metadata");
    CL(srvconn_total_freed()==freed,"server endpoints survive complete local ring retirement");
done:
    if(other){loom_handle_closed(other);loom_unref(other);}
    // Every cleanup pointer still has either the original owner or observer root.
    if (l) {
        for (u32 i=0;i<2;++i) if(held[i]) {(void)loom_engine_put(l,scope[i],held[i]);held[i]=NULL;}
        for (u32 i=0;i<2;++i) if(prepared[i]) {(void)loom_private_connect_rollback(prepared[i]);prepared[i]=NULL;}
        if(fixed.owner)(void)loom_private_fixed_release(l,&fixed);
        if(va)(void)sys_burrow_detach_for_proc(creator,va,PAGE_SIZE);
        if(fd>=0){handle_close(creator,fd);fd=-1;}
        else if(*owner)loom_handle_closed(l);
    }
    if(observer.obj)handle_put(&observer);
    for(u32 i=0;i<2;++i)if(server[i])srvconn_unref(server[i]);
#undef CL
    return error;
}

// Actual private controls with native protocol ownership. Wire responder supplies
// only replies; production code owns authority, state, cancellation and cleanup.
static int control_native_query(struct Loom *l,struct Proc *p,
        struct loom_service_ref ref,struct loom_service_snapshot *out) {
    struct loom_service_snapshot in={.size=sizeof(in),.version=1,.object=ref};
    return loom_private_service_query(l,p,&in,out);
}
static struct {
    struct Loom *l;struct Proc *creator;struct loom_service_ref scope;
    bool seen,retained;
} control_boundary;
static void control_destruction_boundary(struct Loom *l,u32 stage) {
    if(stage!=4 || l!=control_boundary.l)return;
    loom_private_close_hook_for_test(NULL);
    struct loom_service_snapshot out={0};
    control_boundary.seen=true;
    int rc=control_native_query(l,control_boundary.creator,control_boundary.scope,&out);
    control_boundary.retained=!rc && out.state==LOOM_SERVICE_ABORTING &&
        out.active_ops==1 && !(out.status_flags&LOOM_SERVICE_LOCAL_RETIRED);
}
static const char *loom_control_cases(struct Loom *l,struct Proc *creator,
        struct Proc *poster,struct SrvRegistry *reg,struct loom_service_ref targets[2]) {
    const char *error=NULL;struct loom_service_protocol *p=NULL,*held=NULL;
    struct SrvConn *server=NULL;struct loom_service_ref scope={0},child={0};
    struct loom_service_snapshot out={0};
    struct loom_service_reserve reserve={.size=sizeof(reserve),.version=1,.kind=LOOM_SERVICE_SCOPE};
    struct loom_service_control control={.size=sizeof(control),.version=1};
    struct loom_ring_hdr *h=(void *)(l->ring_kva+l->hdr_off);
    u32 pages=creator->as->page_count;
#define CT(c,m) do {if(!(c)){error=m;goto done;}}while(0)
    CT(!control_native_query(l,creator,targets[0],&out) && out.kind==LOOM_SERVICE_TARGET &&
       out.state==LOOM_SERVICE_READY && !out.scope.incarnation,"control real target snapshot");
    struct loom_service_snapshot in={.size=sizeof(in),.version=1,.object=targets[0]};
    out.reason=123;
    CT(loom_private_service_query(l,poster,&in,&out)==-T_E_ACCES && out.reason==123,
       "control rejects actual foreign creator without output");
    ++in.object.incarnation;
    CT(loom_private_service_query(l,creator,&in,&out)==-T_E_NOENT && out.reason==123,
       "control stale query preserves output");in.object=targets[0];in.kind=1;
    CT(loom_private_service_query(l,creator,&in,&out)==-T_E_INVAL && out.reason==123,
       "control output-only input rejected");
    for(u32 mode=0;mode<4;++mode) {
        CT(!loom_private_node_prepare(l,creator,&reserve,&scope) && !loom_private_node_publish(l,scope),
           "control actual scope reservation");control.object=scope;
        CT(!control_native_query(l,creator,scope,&out) && out.state==LOOM_SERVICE_RESERVED &&
           !out.active_ops && !out.pending_terminals && !out.reason && !out.status_flags,
           "control RESERVED starts without diagnostics");
        if(mode==0) {
            CT(!loom_private_service_abort(l,creator,&control) &&
               loom_private_service_abort(l,creator,&control)==1,"control unused abort idempotence");
            CT(!control_native_query(l,creator,scope,&out) && out.state==LOOM_SERVICE_RETIRED &&
               out.reason==-T_E_CANCELED && out.status_flags==LOOM_SERVICE_LOCAL_RETIRED,
               "control unused abort locally retires");
        } else if(mode==1) {
            CT(!loom_private_connect_prepare(l,scope,targets[0],81,1000,1,&p),"control raw constructor");
            CT(!control_native_query(l,creator,scope,&out) && out.active_ops==1,
               "control counts PREPARED owner");
            CT(!loom_private_service_abort(l,creator,&control) &&
               loom_private_connect_publish(p,2)==-T_E_CANCELED &&
               loom_private_service_reap(l,creator,&control)==-T_E_BUSY,"control cancels constructing scope");
            control_boundary.l=l;control_boundary.creator=creator;control_boundary.scope=scope;
            control_boundary.seen=false;control_boundary.retained=false;
            loom_private_close_hook_for_test(control_destruction_boundary);
            int rc=loom_private_connect_rollback(p);
            loom_private_close_hook_for_test(NULL);if(!rc)p=NULL;
            CT(!rc && control_boundary.seen && control_boundary.retained,
               "control physical rollback boundary retains diagnostic owner");
            CT(!control_native_query(l,creator,scope,&out) && !out.active_ops &&
               out.state==LOOM_SERVICE_RETIRED && out.reason==-T_E_CANCELED &&
               out.status_flags==LOOM_SERVICE_LOCAL_RETIRED,"control rollback retains first reason");
        } else {
            CT(!loom_private_connect_prepare(l,scope,targets[mode-2],80+mode,1000,1,&p),
               "control normal/bulk protocol prepare");
            CT(!loom_engine_adopt(p,2),"control actual indexed admission");p=NULL;
            CT(!loom_engine_get(l,scope,&held),"control retains real entry for inspection");
            server=srv_accept_blocking(srv_lookup_in(reg,mode==2?"small":"large",5),poster->stripes);
            CT(server==held->admission.conn,"control accepted real endpoint");
            CT(!loom_post_cqe(l,1,0,0) && !loom_post_cqe(l,2,0,0),"control fills CQ before handshake");
            CT(!lc_reply(server,P9_TVERSION,P9_NOTAG) && !lc_reply(server,P9_TATTACH,0),
               "control wire version/attach replies");
            for(u32 n=0;n<64 && l->service_nodes->nodes[scope.slot].state!=LOOM_SERVICE_READY;++n)
                (void)loom_engine_visit(l,3+n);
            CT(!control_native_query(l,creator,scope,&out) && out.state==LOOM_SERVICE_READY &&
               !out.active_ops && out.pending_terminals==1 && out.status_flags==LOOM_SERVICE_BYTES_SENT,
               "control actual READY snapshot retains full-CQ terminal and sent bytes");
            struct loom_service_reserve fid={.size=sizeof(fid),.version=1,.kind=LOOM_SERVICE_FID,.scope=scope};
            CT(!loom_private_node_prepare(l,creator,&fid,&child) && !loom_private_node_publish(l,child),
               "control child retains actual READY parent");
            CT(!control_native_query(l,creator,child,&out) && out.kind==LOOM_SERVICE_FID &&
               out.scope.slot==scope.slot && out.scope.incarnation==scope.incarnation,
               "control child snapshot keeps exact parent");
            CT(!loom_private_service_abort(l,creator,&control) &&
               loom_private_service_abort(l,creator,&control)==1,"control CQ-independent abort and idempotence");
            (void)loom_engine_visit(l,100);
            CT(!control_native_query(l,creator,scope,&out) && out.state==LOOM_SERVICE_RETIRED &&
               out.reason==-T_E_CANCELED && out.status_flags==3 && out.pending_terminals==1 &&
               l->service_requests.records[held->request.index].result==(s32)scope.slot,
               "control actual abort preserves committed success and retirement diagnostics");
            CT(loom_private_service_reap(l,creator,&control)==-T_E_BUSY,"control entry/terminal pins block reap");
            h->cq_head=l->cq_tail;
            CT(!loom_private_post_terminal(l,held->request),"control publishes retained success");
            CT(!control_native_query(l,creator,scope,&out) && !out.active_ops && !out.pending_terminals,
               "control delivered storage is not a pending operation");
            CT(!loom_engine_put(l,scope,held),"control returns entry pin");held=NULL;
            u64 freed=srvconn_total_freed();
            CT(!loom_engine_collect(l,scope) && srvconn_total_freed()==freed && creator->as->page_count==pages,
               "control local protocol refund leaves server endpoint alive");
            CT(!control_native_query(l,creator,scope,&out) && out.state==LOOM_SERVICE_RETIRED &&
               out.reason==-T_E_CANCELED && out.status_flags==3,"control diagnostics survive physical collection");
            CT(loom_private_service_reap(l,creator,&control)==-T_E_BUSY,"control child blocks parent reap");
            struct loom_service_control child_control={.size=sizeof(child_control),.version=1,.object=child};
            CT(!loom_private_service_reap(l,creator,&child_control),"control unused child reap after parent abort");
            child=(struct loom_service_ref){0};srvconn_unref(server);server=NULL;
        }
        CT(!loom_private_service_reap(l,creator,&control) &&
           loom_private_service_reap(l,creator,&control)==-T_E_NOENT,"control reap consumes exact identity once");
        scope=(struct loom_service_ref){0};h->cq_head=l->cq_tail;
        CT(creator->as->page_count==pages && !l->service_requests.occupied,
           "control refunds all protocol pages and obligations");
    }
done:
    loom_private_close_hook_for_test(NULL);
    if(held)(void)loom_engine_put(l,scope,held);
    if(p)(void)loom_private_connect_rollback(p);
    if(l->service_engine->count) {
        loom_exec_latch(l);
        for(u32 i=0;i<64;++i)(void)loom_engine_visit(l,10000);
        (void)loom_engine_discard_closed(l);
    }
    if(server)srvconn_unref(server);
#undef CT
    return error;
}

// Actual existing SQPOLL thread, native poll hooks and creator ticket. The
// responder injects a partial first response and complete second handshake in
// the registered-before-sleep window; no explicit engine visit drives either.
static struct {
    struct Loom *l;
    struct SrvConn *server[2];
    u64 parks;
    bool injected, done;
    const char *error;
} executor_probe;
static void loom_executor_boundary(struct Loom *l,u32 stage) {
    if (stage!=5 || l!=executor_probe.l) return;
    __atomic_fetch_add(&executor_probe.parks,1,__ATOMIC_RELEASE);
    if (__atomic_exchange_n(&executor_probe.injected,true,__ATOMIC_ACQ_REL)) return;
    u8 partial=19;
    if (srvconn_server_send(executor_probe.server[0],&partial,1)!=1 ||
        lc_reply(executor_probe.server[1],P9_TVERSION,P9_NOTAG) ||
        lc_reply(executor_probe.server[1],P9_TATTACH,0))
        executor_probe.error="executor real peer injection failed";
    __atomic_store_n(&executor_probe.done,true,__ATOMIC_RELEASE);
}
static bool loom_executor_wait_node(struct Loom *l,struct loom_service_ref ref,u32 state) {
    u64 end=timer_now_ns()+5000000000ull;
    do {
        spin_lock(&l->lock);
        bool found=l->service_nodes->nodes[ref.slot].state==state;
        spin_unlock(&l->lock);
        if(found)return true;
        sched();
    } while(timer_now_ns()<end);
    return false;
}
static const char *loom_executor_case(struct Loom **owner,struct Proc *creator,
    struct Proc *poster,struct SrvRegistry *reg,struct loom_service_ref targets[2],
    u64 goal,u32 baseline) {
    const char *error=NULL;
    struct Loom *l=*owner;
    struct loom_service_ref scope[2]={{0},{0}};
    struct loom_service_protocol *prepared=NULL,*held[2]={NULL,NULL};
    struct SrvConn *server[2]={NULL,NULL};
    struct loom_service_reserve reserve={.size=sizeof(reserve),.version=1,.kind=LOOM_SERVICE_SCOPE};
    struct Handle observer={0};int fd=-1;
    int workers=creator->loom_sqpoll_count;
    struct loom_ring_hdr *h=(void *)(l->ring_kva+l->hdr_off);
#define EX(c,m) do {if(!(c)){error=m;goto done;}}while(0)
    for (u32 i=0;i<2;++i) {
        EX(!loom_private_node_prepare(l,creator,&reserve,&scope[i]) &&
           !loom_private_node_publish(l,scope[i]),"executor reserves exact scopes");
        u64 now=timer_now_ns();
        EX(!loom_private_connect_prepare(l,scope[i],targets[i],77+i,
           i?0:now+2000000000ull,now,&prepared),"executor charged protocol preparation");
        EX(!loom_engine_adopt(prepared,now),"executor indexed adoption");prepared=NULL;
        EX(!loom_engine_get(l,scope[i],&held[i]),"executor fixture retains observation pin");
        server[i]=srv_accept_blocking(srv_lookup_in(reg,i?"large":"small",5),poster->stripes);
        EX(server[i]==held[i]->admission.conn,"executor retained real peer");
    }
    EX(!loom_post_cqe(l,900,0,0) && !loom_post_cqe(l,901,0,0),"executor starts behind full CQ");
    fd=handle_alloc(creator,KOBJ_LOOM,RIGHT_READ|RIGHT_WRITE,l);
    EX(fd>=0,"executor handle adopts ring root");*owner=NULL;
    EX(!handle_get(creator,fd,&observer),"executor observer lifetime");
    executor_probe.l=l;executor_probe.server[0]=server[0];executor_probe.server[1]=server[1];
    executor_probe.parks=0;executor_probe.injected=false;executor_probe.done=false;executor_probe.error=NULL;
    loom_private_close_hook_for_test(loom_executor_boundary);
    EX(loom_start_sqpoll(l)==-T_E_OPNOTSUPP,"ordinary private worker activation remains refused");
    EX(!loom_private_start_executor(l) && creator->loom_sqpoll_count==workers+1 &&
       l->service_engine->worker_ticket.stripes==creator->stripes,"existing creator budget charges actual private worker");
    EX(loom_private_start_executor(l)==-T_E_BUSY,"second private executor refused");
    EX(loom_executor_wait_node(l,scope[1],LOOM_SERVICE_READY),"registered peer wake drives second scope without explicit progress");
    EX(__atomic_load_n(&executor_probe.done,__ATOMIC_ACQUIRE) && !executor_probe.error,
       executor_probe.error?executor_probe.error:"executor injected real registration-window replies");
    spin_lock(&l->lock);
    bool fair=l->service_nodes->nodes[scope[0].slot].state!=LOOM_SERVICE_RETIRED &&
        l->service_requests.records[held[1]->request.index].phase==LREQ_TERMINAL;
    spin_unlock(&l->lock);
    EX(fair,"partial first peer cannot starve second and full CQ retains success");
    u64 parks=__atomic_load_n(&executor_probe.parks,__ATOMIC_ACQUIRE);
    for(u32 i=0;i<64;++i)sched();
    EX(__atomic_load_n(&executor_probe.parks,__ATOMIC_ACQUIRE)<=parks+1,
       "READY and full CQ park without repeated self wake");
    __atomic_store_n(&h->cq_head,1,__ATOMIC_RELEASE);
    wakeup(&l->sqpoll_park); // internal equivalent of the future ENTER wake
    u64 end=timer_now_ns()+5000000000ull;bool delivered=false;
    do {
        spin_lock(&l->lock);
        delivered=l->service_requests.records[held[1]->request.index].phase==LREQ_DELIVERED;
        spin_unlock(&l->lock);
        if(delivered)break;sched();
    }while(timer_now_ns()<end);
    EX(delivered,"consumer drain wakes retained terminal delivery");
    EX(loom_executor_wait_node(l,scope[0],LOOM_SERVICE_RETIRED),"absolute timeout retires partial peer without a native edge");
    spin_lock(&l->lock);
    bool timed=l->service_requests.records[held[0]->request.index].result==-T_E_TIMEDOUT &&
        l->service_requests.records[held[0]->request.index].phase==LREQ_TERMINAL;
    spin_unlock(&l->lock);
    EX(timed,"timeout terminal survives refilled CQ");
    EX(!handle_close(creator,fd),"actual handle close starts local retirement");fd=-1;
    for(u32 i=0;i<2;++i) {EX(!loom_engine_put(l,scope[i],held[i]),"fixture returns exact entry pins");held[i]=NULL;}
    EX(loom_close_wait_state(l,LCLOSE_DONE) && __atomic_load_n(&l->sqpoll_exited,__ATOMIC_ACQUIRE),
       "close completion includes actual private executor exit");
    EX(!l->service_engine->wait_active && !l->service_engine->wait_mask &&
       loom_engine_empty(l->service_engine),"close removes all wait hooks and protocol owners");
    loom_private_close_hook_for_test(NULL);executor_probe.l=NULL;
    u64 freed=srvconn_total_freed();
    handle_put(&observer);l=NULL;
    EX(lp_wait(goal) && creator->loom_sqpoll_count==workers && creator->as->page_count==baseline &&
       !creator->as->private_rings,"actual worker join precedes exact ticket and image refund");
    EX(srvconn_total_freed()==freed,"peer endpoints survive worker and local ring reclamation");
done:
    loom_private_close_hook_for_test(NULL);
    if(l) {
        if(prepared)(void)loom_private_connect_rollback(prepared);
        for(u32 i=0;i<2;++i)if(held[i])(void)loom_engine_put(l,scope[i],held[i]);
        if(fd>=0)(void)handle_close(creator,fd);
        else if(*owner)loom_handle_closed(l);
        // An already executing boundary callback may outlive clearing the hook.
        // Retain its peer refs and globals until the actual executor exits.
        if(l->sqpoll) {
            u64 stop=timer_now_ns()+5000000000ull;
            while(!__atomic_load_n(&l->sqpoll_exited,__ATOMIC_ACQUIRE) && timer_now_ns()<stop)sched();
            if(!__atomic_load_n(&l->sqpoll_exited,__ATOMIC_ACQUIRE))
                extinction("executor fixture could not quiesce its callback");
        }
        if(observer.obj)handle_put(&observer);
    }
    for(u32 i=0;i<2;++i)if(server[i])srvconn_unref(server[i]);
    executor_probe.l=NULL;
#undef EX
    return error;
}


// Real private protocol/storage owner. No general RPC submission is enabled.
static const char *loom_operation_storage_case(struct Loom *l,struct Proc *creator,
    struct Proc *poster,struct SrvRegistry *reg,struct loom_service_ref target,bool bulk,bool frontend) {
    const char *error=NULL;
    struct loom_service_ref scope={0},child={0};
    bool root_borrow=false;
    struct loom_service_protocol *prepared=NULL,*held=NULL;
    struct loom_service_operation *op=NULL,*other=NULL;
    struct SrvConn *server=NULL;
    struct loom_service_reserve reserve={.size=sizeof(reserve),.version=1,.kind=LOOM_SERVICE_SCOPE};
    u32 old_budget=creator->as->page_budget;
#define OS(c,m) do{if(!(c)){error=m;goto done;}}while(0)
    OS(!loom_private_node_prepare(l,creator,&reserve,&scope) && !loom_private_node_publish(l,scope),
       "operation storage exact scope reservation");
    if(frontend) {
        struct loom_ring_hdr *h=(void *)(l->ring_kva+l->hdr_off);
        struct loom_sqe *sq=(void *)(l->ring_kva+l->sqe_off);
        u32 *indices=(void *)(l->ring_kva+l->sq_array_off);
        sq[0]=(struct loom_sqe){.opcode=LOOM_OP_SERVICE_CONNECT,.handle_idx=target.slot,
            .offset=scope.slot,.user_data=901,._resv1={0,target.incarnation,scope.incarnation,0}};
        indices[0]=0;__atomic_store_n(&h->sq_tail,1,__ATOMIC_RELEASE);
        OS(loom_private_submit(l,creator,1)==1 && l->sq_head==1 && l->service_engine->count==1,
           "frontend real constructor and indexed adoption");
        sq[0].user_data=999; // already copied; cannot change admitted correlation
    } else {
        OS(!loom_private_connect_prepare(l,scope,target,901,0,0,&prepared),"operation storage protocol");
        OS(!loom_engine_adopt(prepared,0),"operation storage adopts protocol");prepared=NULL;
    }
    OS(!loom_engine_get(l,scope,&held),"operation storage observes exact protocol");
    server=srv_accept_blocking(srv_lookup_in(reg,bulk?"large":"small",5),poster->stripes);
    OS(server==held->admission.conn,"operation storage actual native peer");
    OS(!lc_reply(server,P9_TVERSION,P9_NOTAG) && !lc_reply(server,P9_TATTACH,0),
       "operation storage handshake replies");
    for(u32 i=0;i<32 && l->service_nodes->nodes[scope.slot].state!=LOOM_SERVICE_READY;++i)
        (void)loom_engine_visit(l,1+i);
    OS(l->service_nodes->nodes[scope.slot].state==LOOM_SERVICE_READY,
       "operation storage actual READY protocol");
    OS(l->service_nodes->nodes[scope.slot].fid_flags==LFID_BOUND &&
       l->service_nodes->nodes[scope.slot].wire_fid==held->client.session.root_fid,
       "actual attach publishes exact root fid mapping");
    OS(!loom_engine_put(l,scope,held),"operation storage observer release");held=NULL;
    if(frontend) {
        struct loom_ring_hdr *h=(void *)(l->ring_kva+l->hdr_off);
        struct loom_cqe *cq=(void *)(l->ring_kva+l->cqe_off);
        struct loom_sqe *sq=(void *)(l->ring_kva+l->sqe_off);
        u32 *indices=(void *)(l->ring_kva+l->sq_array_off);
        OS(l->cq_tail==1 && cq[0].user_data==901 && cq[0].result==(s32)scope.slot,
           "frontend native handshake exact copied completion");
        sq[1]=(struct loom_sqe){.opcode=LOOM_OP_SERVICE_CONNECT,.flags=LOOM_SQE_LINK,.user_data=902};
        indices[1]=1;__atomic_store_n(&h->sq_tail,2,__ATOMIC_RELEASE);
        OS(loom_private_submit(l,creator,1)==1 && l->cq_tail==2 && cq[1].user_data==902 && cq[1].result==-T_E_INVAL,
           "frontend malformed shape returns one native CQ refusal");
        u64 epoch=l->service_engine->change;
        __atomic_store_n(&h->cq_head,l->cq_tail,__ATOMIC_RELEASE);
        OS(loom_private_submit(l,creator,0)==0 && l->service_engine->change!=epoch,
           "frontend zero-submit CQ acknowledgement changes native wait epoch");
    }
    u32 baseline=creator->as->page_count;
    loom_private_operation_fault_arm_for_test();
    OS(loom_private_operation_prepare(l,scope,902,40,40,&op)==-T_E_TIMEDOUT && !op &&
       !loom_private_operation_fault_pending_for_test() &&
       creator->as->page_count==baseline,"operation storage expired preallocation refusal");
    spin_lock(&creator->as->lock);creator->as->page_budget=baseline;spin_unlock(&creator->as->lock);
    int limited=loom_private_operation_prepare(l,scope,903,0,0,&op);
    spin_lock(&creator->as->lock);creator->as->page_budget=old_budget;spin_unlock(&creator->as->lock);
    OS(limited==-T_E_NOMEM && !op && creator->as->page_count==baseline &&
       l->service_requests.occupied==1 && !l->service_engine->entries[scope.slot].pins,
       "operation storage actual charge refusal releases terminal and entry");
    loom_private_operation_fault_arm_for_test();
    OS(loom_private_operation_prepare(l,scope,907,0,0,&op)==-T_E_NOMEM && !op &&
       !loom_private_operation_fault_pending_for_test() && creator->as->page_count==baseline &&
       l->service_requests.occupied==1 && !l->service_engine->entries[scope.slot].pins,
       "operation storage actual allocation refusal refunds and unpins");
    OS(!loom_private_operation_prepare(l,scope,904,0,0,&op),"operation storage allocates");
    OS(creator->as->page_count==baseline+1 && l->service_requests.occupied==2 &&
       l->service_requests.records[op->request.index].owners==1 &&
       l->service_engine->entries[scope.slot].pins==1,
       "operation storage exact buddy charge and physical owner");
    OS(loom_private_operation_prepare(l,scope,905,0,0,&other)==-T_E_NOSPC && !other &&
       creator->as->page_count==baseline+1 && l->service_engine->entries[scope.slot].pins==1,
       "operation storage capacity refuses before allocation");
    struct loom_request_ref old=op->request;
    OS(!loom_private_operation_release_unused(op),"operation storage unused destruction");op=NULL;
    OS(creator->as->page_count==baseline && l->service_requests.occupied==1 &&
       !l->service_engine->entries[scope.slot].pins,
       "operation storage exact page refund and pin return");
    OS(!loom_private_operation_prepare(l,scope,906,0,0,&op) && op->request.identity!=old.identity,
       "operation storage burns identity before slot reuse");
    reserve.kind=LOOM_SERVICE_FID;reserve.scope=scope;
    OS(!loom_private_node_prepare(l,creator,&reserve,&child) && !loom_private_node_publish(l,child),
       "root retains unused child through close");
    u32 wire=UINT32_MAX;
    spin_lock(&l->lock);
    int borrowed=loom_fid_borrow(l->service_slots,l->service_nodes,scope,scope,0,&wire);
    root_borrow=borrowed==0;
    spin_unlock(&l->lock);
    OS(root_borrow && wire==op->protocol->client.session.root_fid,
       "root source borrow retains actual attached mapping");
    loom_handle_closed(l);
    OS(loom_close_wait_state(l,LCLOSE_PARKED),"operation storage close parks on physical owner");
    OS(creator->as->page_count==baseline+1 && l->service_requests.records[op->request.index].owners==1 &&
       l->service_engine->entries[scope.slot].pins==1,
       "operation storage close retains actual storage and owner");
    spin_lock(&l->lock);
    int put=loom_fid_put(l->service_slots,l->service_nodes,scope);
    if(!put)root_borrow=false;
    spin_unlock(&l->lock);
    OS(!put,"root borrow returns after actual close abort");
    OS(!loom_private_operation_release_unused(op),"operation storage cleanup after close");op=NULL;
    OS(loom_close_wait_state(l,LCLOSE_DONE) && loom_engine_empty(l->service_engine) &&
       !l->service_requests.occupied && !l->service_fixed_count,
       "operation storage final owner release unblocks real close worker");
done:
    if(root_borrow) {
        spin_lock(&l->lock);
        (void)loom_fid_put(l->service_slots,l->service_nodes,scope);
        spin_unlock(&l->lock);
        loom_private_close_changed(l);
    }
    spin_lock(&creator->as->lock);creator->as->page_budget=old_budget;spin_unlock(&creator->as->lock);
    if(other)(void)loom_private_operation_release_unused(other);
    if(op)(void)loom_private_operation_release_unused(op);
    if(held)(void)loom_engine_put(l,scope,held);
    if(prepared)(void)loom_private_connect_rollback(prepared);
    loom_handle_closed(l);
    if(!loom_close_wait_state(l,LCLOSE_DONE) && !error)error="operation storage cleanup timeout";
    if(server)srvconn_unref(server);
#undef OS
    return error;
}

// Real SrvConn framing and installed engine, with a deterministic wire peer.
// The peer reads every emitted request before replying; it is not a filesystem
// server. The actual private worker owns progress; the fixture only yields,
// reads peer frames and acknowledges CQEs through the creator frontend.
struct loom_native_peer {
    u8 request[512];
    u32 used, seen, stop; // optional one-based request at which peer sleeps
    u32 owners; // expected CONNECT request owners (zero means one)
    u32 root; // wire fid TATTACH bound; only it is a directory
    struct Proc *creator;
};
static int loom_native_peer_step(struct SrvConn *cn,struct loom_native_peer *peer) {
    static const u8 sequence[]={P9_TVERSION,P9_TATTACH,P9_TGETATTR,P9_TWALK,
        P9_TGETATTR,P9_TLOPEN,P9_TREAD,P9_TWRITE,
        P9_TREAD,P9_TREAD,P9_TREAD,P9_TREAD,P9_TCLUNK,P9_TGETATTR,P9_TWALK};
    long n=srvconn_server_recv(cn,peer->request+peer->used,sizeof(peer->request)-peer->used);
    if(n<0)return -1;
    peer->used+=(u32)n;
    if(peer->used<P9_HDR_LEN)return 0;
    u32 size;u8 type;u16 tag;
    if(p9_peek_header(peer->request,peer->used,&size,&type,&tag)<0 || size>sizeof(peer->request))return -1;
    if(peer->used<size)return 0;
    if(peer->seen>=sizeof(sequence) || type!=sequence[peer->seen])return -1;
    if(type==P9_TWALK && (size!=23 || peer->request[15]!=1 || peer->request[17]!=4 ||
       peer->request[19]!='f' || peer->request[20]!='i' || peer->request[21]!='l' || peer->request[22]!='e'))return -1;
    if(type==P9_TWRITE && (size!=28 || peer->request[19]!=5 ||
       peer->request[23]!='h' || peer->request[24]!='e' || peer->request[25]!='l' ||
       peer->request[26]!='l' || peer->request[27]!='o'))return -1;
    if(peer->stop==peer->seen+1u)return 2; // complete request observed, no reply
    u32 fid=0;
    if((type==P9_TATTACH || type==P9_TGETATTR) && p9_unpack_u32(peer->request+P9_HDR_LEN,size-P9_HDR_LEN,&fid)<0)
        return -1;
    if(type==P9_TATTACH)peer->root=fid;
    u8 reply[256];
    int count=canonical_responder(NULL,peer->request,size,reply,sizeof(reply));
    if(count<=0)return -1;
    if(type==P9_TGETATTR) {
        reply[15]=fid==peer->root?P9_QTDIR:P9_QTFILE; // a WALK's stat names its source
        (void)p9_pack_u32(reply+28,sizeof(reply)-28,0777);
        (void)p9_pack_u32(reply+32,sizeof(reply)-32,42);
    }
    if(type==P9_TREAD && peer->seen==11) {
        count=11;(void)p9_pack_u32(reply,sizeof(reply),11);
        (void)p9_pack_u32(reply+7,sizeof(reply)-7,0); // terminal EOF
    }
    if(srvconn_server_send(cn,reply,count)!=count)return -1;
    for(u32 i=size;i<peer->used;++i)peer->request[i-size]=peer->request[i];
    peer->used-=size;++peer->seen;return 1;
}
static bool loom_native_completion_flags(struct Loom *l,struct SrvConn *cn,
    struct loom_native_peer *peer,u64 data,s32 result,u32 flags,
    struct loom_service_buffer_receipt *receipt) {
    struct loom_ring_hdr *h=(void *)(l->ring_kva+l->hdr_off);
    struct loom_cqe *cq=(void *)(l->ring_kva+l->cqe_off);
    u32 head=__atomic_load_n(&h->cq_head,__ATOMIC_ACQUIRE);
    u64 end=timer_now_ns()+2000000000ull;
    do {
        sched();
        if(loom_native_peer_step(cn,peer)<0)return false;
        u32 tail=__atomic_load_n(&h->cq_tail,__ATOMIC_ACQUIRE);
        if(tail!=head) {
            struct loom_cqe copy=cq[head&(l->cq_entries-1u)];
            if(tail!=head+1 || copy.user_data!=data || copy.result!=result || copy.flags!=flags)return false;
            if(receipt) {
                struct loom_service_buffer_receipt *paired=(void *)(l->ring_kva+l->receipt_off);
                *receipt=paired[head&(l->cq_entries-1u)];
            }
            __atomic_store_n(&h->cq_head,head+1,__ATOMIC_RELEASE);
            if(loom_private_submit(l,peer->creator,0)<0)return false;
            // A visible terminal does not prove its storage has been freed.
            // Wait for the actual worker to return the request quota. MORE
            // keeps its owner, and a retained payload must not be returned here.
            if(flags&LOOM_CQE_MORE)return true;
            do {
                spin_lock(&l->lock);
                bool retired=l->service_requests.occupied==(peer->owners?peer->owners:1u) && !l->service_fixed_count;
                spin_unlock(&l->lock);
                if(retired)return true;
                sched();
            }while(timer_now_ns()<end);
            return false;
        }
        sched();
    }while(timer_now_ns()<end);
    return false;
}
static bool loom_native_completion(struct Loom *l,struct SrvConn *cn,
    struct loom_native_peer *peer,u64 data,s32 result) {
    return loom_native_completion_flags(l,cn,peer,data,result,0,NULL);
}
static bool loom_native_submit(struct Loom *l,struct Proc *creator,struct loom_sqe s) {
    struct loom_ring_hdr *h=(void *)(l->ring_kva+l->hdr_off);
    struct loom_sqe *sq=(void *)(l->ring_kva+l->sqe_off);
    u32 *indices=(void *)(l->ring_kva+l->sq_array_off);
    u32 at=l->sq_head&(l->sq_entries-1u);
    sq[at]=s;indices[at]=at;
    __atomic_store_n(&h->sq_tail,l->sq_head+1,__ATOMIC_RELEASE);
    int rc=loom_private_submit(l,creator,1);
    sq[at].user_data=0xdead; // no completion may consult the shared copy later
    return rc==1;
}
static u64 loom_native_epoch(struct Loom *l) {
    spin_lock(&l->lock);u64 epoch=l->service_engine->change;
    spin_unlock(&l->lock);return epoch;
}
static bool loom_native_peer_empty(struct SrvConn *server) {
    spin_lock(&server->c2s.lock);bool empty=!server->c2s.count;
    spin_unlock(&server->c2s.lock);return empty;
}
// Each fault starts after a real worker-driven CONNECT/WALK/LOPEN. Hold the
// server endpoint and observation pin until local retirement is witnessed;
// otherwise ordinary endpoint destruction could hide a stuck owner.
static const char *loom_native_fault(struct Loom *l,struct Proc *creator,
    struct SrvConn *server,struct loom_service_ref scope,
    struct loom_service_ref child,u8 *bytes,u32 fault) {
    struct loom_ring_hdr *h=(void *)(l->ring_kva+l->hdr_off);
    struct loom_cqe *cq=(void *)(l->ring_kva+l->cqe_off);
    u32 head=__atomic_load_n(&h->cq_head,__ATOMIC_ACQUIRE);
    bytes[64]=0xa5;bytes[68]=0x5a;
    bool partial=fault==9,writing=fault==7 || partial,timeout=fault==8;
    u32 prefix_at=0;u64 produced=0;
    if(partial) {
        // Controlled transport-pressure injection, not a valid peer transcript:
        // leave five bytes free so the actual private progress adapter must
        // emit a strict prefix of TWRITE and park before sending its remainder.
        u8 fill[64];for(u32 i=0;i<sizeof(fill);++i)fill[i]=0xd3;
        if(!loom_native_peer_empty(server))return "midwrite requires empty output channel";
        u32 left=server->c2s.cap-5u;
        while(left) {
            u32 n=left<sizeof(fill)?left:sizeof(fill);
            if(srvconn_client_send(server,fill,n)!=(long)n)return "midwrite pressure injection";
            left-=n;
        }
        spin_lock(&server->c2s.lock);
        prefix_at=server->c2s.head;produced=server->c2s.produced;
        spin_unlock(&server->c2s.lock);
    }
    u64 deadline=timeout?timer_now_ns()+500000000ull:0;
    if(!loom_native_submit(l,creator,(struct loom_sqe){.opcode=writing?LOOM_OP_WRITE:LOOM_OP_READ,
       .handle_idx=child.slot,.len=5,.user_data=900,
       ._resv1={64,child.incarnation,0,deadline}}))return "fault IO submission";
    u64 end=timer_now_ns()+2000000000ull;int send=0;
    if(partial) {
        bool emitted=false;
        do {
            spin_lock(&server->c2s.lock);
            emitted=server->c2s.count==server->c2s.cap;
            bool exact=server->c2s.produced==produced+5u;
            static const u8 prefix[5]={28,0,0,0,P9_TWRITE};
            if(emitted)for(u32 i=0;i<5;++i)
                if(server->c2s.buf[(prefix_at+i)%server->c2s.cap]!=prefix[i])exact=false;
            spin_unlock(&server->c2s.lock);
            if(emitted) {
                if(!exact)return "midwrite actual emitted prefix mismatch";
                break;
            }
            sched();
        }while(timer_now_ns()<end);
        if(!emitted)return "midwrite did not reach partial send boundary";
    } else {
        u8 request[64];u32 used=0,size=0;u8 type=0;u16 tag=0;
        end=timer_now_ns()+2000000000ull;
        do {
            long n=srvconn_server_recv(server,request+used,sizeof(request)-used);
            if(n<0)return "fault peer lost request";
            used+=(u32)n;
            if(used>=P9_HDR_LEN) {
                if(p9_peek_header(request,used,&size,&type,&tag)<0 || size>sizeof(request))
                    return "fault request framing";
                if(used==size)break;
            }
            sched();
        }while(timer_now_ns()<end);
        if(used!=(writing?28u:23u) || type!=(writing?P9_TWRITE:P9_TREAD) || size!=used)
            return "fault waits for emitted IO";
        if(writing && (request[23]!=0xa5 || request[27]!=0x5a))return "fault WRITE payload bytes";
        u8 reply[256];int count=canonical_responder(NULL,request,used,reply,sizeof(reply));
        if(count!=(writing?11:16))return "fault canonical reply shape";
        // 1: no response, 2: partial header, 3: partial body, 4: impossible
        // frame length, 5: unexpected reply type, 6: cancellation with full CQ,
        // 7: emitted WRITE without acknowledgement, 8: READ deadline expiry.
        if(fault==2)send=3;
        if(fault==3)send=13;
        if(fault==4){(void)p9_pack_u32(reply,sizeof(reply),6);send=7;}
        if(fault==5){reply[4]=P9_RWRITE;send=count;}
        if(send && srvconn_server_send(server,reply,send)!=send)return "fault response injection";
    }
    bool malformed=fault==4 || fault==5;
    if(!malformed && !timeout) {
        // For fragments, observe actual transport consumption before abort.
        // A yield count alone does not establish a mid-frame receive boundary.
        if(send) {
            bool drained=false;end=timer_now_ns()+2000000000ull;
            do {
                spin_lock(&server->s2c.lock);drained=!server->s2c.count;
                spin_unlock(&server->s2c.lock);
                if(drained)break;
                sched();
            }while(timer_now_ns()<end);
            if(!drained)return "fault fragment was not consumed";
        }
        if(__atomic_load_n(&h->cq_tail,__ATOMIC_ACQUIRE)!=head)
            return "stalled response completed prematurely";
        if(fault==6 && (loom_post_cqe(l,800,0,0) || loom_post_cqe(l,801,0,0)))
            return "fault fills CQ";
        struct loom_service_control control={.size=sizeof(control),.version=1,.object=scope};
        if(loom_private_service_abort(l,creator,&control)<0)return "fault abort control";
    }
    struct loom_service_snapshot snap;
    bool retired=false;
    end=timer_now_ns()+2000000000ull;
    do {
        if(control_native_query(l,creator,scope,&snap))return "fault scope query";
        retired=snap.state==LOOM_SERVICE_RETIRED && (snap.status_flags&LOOM_SERVICE_LOCAL_RETIRED);
        if(retired)break;
        sched();
    }while(timer_now_ns()<end);
    if(!retired)return "fault local retirement stalls with retained endpoint";
    s32 expected=timeout?-T_E_TIMEDOUT:-T_E_CANCELED;
    if(snap.reason>=0 || (!malformed && snap.reason!=expected))return "fault retirement reason";
    if(writing && !(snap.status_flags&LOOM_SERVICE_BYTES_SENT))return "canceled WRITE loses unknown-outcome diagnostic";
    if(fault==6) {
        if(__atomic_load_n(&h->cq_tail,__ATOMIC_ACQUIRE)!=head+2 || !snap.pending_terminals)
            return "full CQ must retain canceled operation terminal";
        head+=2;__atomic_store_n(&h->cq_head,head,__ATOMIC_RELEASE);
        if(loom_private_submit(l,creator,0)<0)return "fault CQ wake";
    }
    bool completed=false;
    end=timer_now_ns()+2000000000ull;
    do {
        u32 tail=__atomic_load_n(&h->cq_tail,__ATOMIC_ACQUIRE);
        if(tail!=head) {
            struct loom_cqe value=cq[head&(l->cq_entries-1u)];
            if(tail!=head+1 || value.user_data!=900 || value.result>=0 || value.flags ||
               (!malformed && value.result!=expected))return "fault terminal identity or status";
            completed=true;break;
        }
        sched();
    }while(timer_now_ns()<end);
    if(!completed)return "fault terminal not delivered";
    __atomic_store_n(&h->cq_head,head+1,__ATOMIC_RELEASE);
    if(loom_private_submit(l,creator,0)<0)return "fault terminal acknowledgement";
    bool released=false;
    do {
        spin_lock(&l->lock);
        released=!l->service_fixed_count && l->service_requests.occupied==1;
        spin_unlock(&l->lock);
        if(released)break;
        sched();
    }while(timer_now_ns()<end);
    if(!released)return "fault physical READ owner retained";
    if(bytes[64]!=0xa5 || bytes[68]!=0x5a)return "failed READ published partial payload";
    if(loom_private_register_buffers(l,creator,NULL,0))return "fault payload unregister";
    return NULL;
}
// Sleep at an exact emitted protocol request while all preceding exchanges
// use canonical replies. Cancellation must retire without this peer replying.
static const char *loom_native_stage_cancel(struct Loom *l,struct SrvConn *server,
    struct loom_native_peer *peer,struct loom_service_ref scope,u64 data,
    struct loom_service_protocol **held,u32 close_kind) {
    struct loom_ring_hdr *h=(void *)(l->ring_kva+l->hdr_off);
    struct loom_cqe *cq=(void *)(l->ring_kva+l->cqe_off);
    u32 head=__atomic_load_n(&h->cq_head,__ATOMIC_ACQUIRE);
    u64 end=timer_now_ns()+2000000000ull;bool paused=false;
    do {
        sched();int step=loom_native_peer_step(server,peer);
        if(step<0)return "stage peer request mismatch";
        if(step==2){paused=true;break;}
    }while(timer_now_ns()<end);
    if(!paused || __atomic_load_n(&h->cq_tail,__ATOMIC_ACQUIRE)!=head)
        return "stage did not stop before operation completion";
    if(close_kind) {
        // The outer fixture retains its creation ref. A separate owned handle
        // invokes the production close/exec/exit-table path while the actual
        // private worker has an emitted request and the peer never replies.
        loom_ref(l);
        hidx_t fd=handle_alloc(peer->creator,KOBJ_LOOM,RIGHT_READ|RIGHT_WRITE,l);
        if(fd<0){loom_unref(l);return "stage close handle allocation";}
        if(close_kind==2) {
            if(handle_set_cloexec(peer->creator,fd,false)) {
                (void)handle_close(peer->creator,fd);return "stage exec CLOEXEC setup";
            }
            handle_private_exec_latch(peer->creator);
            if(handle_close_on_exec(peer->creator)!=1)return "stage forced private exec close";
        } else if(close_kind==3) {
            // Isolated fixture has no creator user threads. This is the
            // production last-live-thread handle-table destruction boundary,
            // not an assertion that the whole exits syscall has run.
            struct HandleTable *table=peer->creator->handles;
            peer->creator->handles=NULL;handle_table_free(table);
        } else if(handle_close(peer->creator,fd))return "stage final handle close";
        if(!loom_close_wait_state(l,LCLOSE_PARKED))return "stage close did not park on observation pin";
        spin_lock(&l->lock);
        bool local=(*held)->locally_retired && !l->service_requests.detached;
        spin_unlock(&l->lock);
        if(!local || srvconn_is_live(server))return "stage close retained a local transport borrower";
        if(loom_engine_put(l,scope,*held))return "stage close observation return";
        *held=NULL;
        if(!loom_close_wait_state(l,LCLOSE_DONE))return "stage close failed without CQ or peer progress";
        spin_lock(&l->lock);
        bool clean=!l->service_requests.occupied && l->service_requests.detached &&
            !l->service_fixed_count && loom_engine_empty(l->service_engine);
        spin_unlock(&l->lock);
        if(!clean || (l->sqpoll && !__atomic_load_n(&l->sqpoll_exited,__ATOMIC_ACQUIRE)))
            return "stage close did not end worker and physical payload owners";
        return NULL;
    }
    struct loom_service_control control={.size=sizeof(control),.version=1,.object=scope};
    if(loom_private_service_abort(l,peer->creator,&control)<0)return "stage abort";
    bool retired=false;end=timer_now_ns()+2000000000ull;
    do {
        struct loom_service_snapshot snap;
        if(control_native_query(l,peer->creator,scope,&snap))return "stage snapshot";
        if(snap.state==LOOM_SERVICE_RETIRED) {
            if(snap.reason!=-T_E_CANCELED || !(snap.status_flags&LOOM_SERVICE_LOCAL_RETIRED))
                return "stage retirement reason";
            retired=true;break;
        }
        sched();
    }while(timer_now_ns()<end);
    if(!retired || srvconn_is_live(server))return "stage local retirement failed with live peer ref";
    bool completed=false;
    do {
        u32 tail=__atomic_load_n(&h->cq_tail,__ATOMIC_ACQUIRE);
        if(tail!=head) {
            struct loom_cqe value=cq[head&(l->cq_entries-1u)];
            if(tail!=head+1 || value.user_data!=data || value.result!=-T_E_CANCELED || value.flags)
                return "stage cancellation must not publish successful descriptor";
            completed=true;break;
        }
        sched();
    }while(timer_now_ns()<end);
    if(!completed)return "stage cancellation terminal missing";
    __atomic_store_n(&h->cq_head,head+1,__ATOMIC_RELEASE);
    if(loom_private_submit(l,peer->creator,0)<0)return "stage terminal acknowledgement";
    bool released=false;
    do {
        spin_lock(&l->lock);
        released=l->service_requests.occupied==1 && !l->service_fixed_count;
        spin_unlock(&l->lock);
        if(released)break;
        sched();
    }while(timer_now_ns()<end);
    if(!released || loom_private_register_buffers(l,peer->creator,NULL,0))
        return "stage physical operation payload retained";
    return NULL;
}
static const char *loom_native_operations_case(struct Loom *l,struct Proc *creator,
    struct Proc *poster,struct SrvRegistry *reg,struct loom_service_ref target,bool bulk,u32 fault) {
    const char *error=NULL;u64 va=0;
    bool independent=fault==40;
    if(independent)fault=0;
    struct loom_service_ref stalled={0};struct SrvConn *stalled_server=NULL;
    u32 close_kind=fault>=20?1+(fault-20)/6:0;
    if(close_kind)fault=9+(fault-20)%6;
    struct SrvConn *server=NULL;struct loom_service_protocol *held=NULL;
    struct loom_service_ref scope={0},child={0};
    struct loom_native_peer peer;
    for(u32 i=0;i<sizeof(peer);++i)((u8 *)&peer)[i]=0;
    peer.creator=creator;peer.stop=(fault>=9 && fault<=14)?fault-8:0;
    struct loom_service_reserve reserve={.size=sizeof(reserve),.version=1,.kind=LOOM_SERVICE_SCOPE};
#define NO(c,m) do{if(!(c)){error=m;goto done;}}while(0)
    NO(!loom_private_node_prepare(l,creator,&reserve,&scope) && !loom_private_node_publish(l,scope),
       "native operation scope reservation");
    NO(loom_native_submit(l,creator,(struct loom_sqe){.opcode=LOOM_OP_SERVICE_CONNECT,
       .handle_idx=target.slot,.offset=scope.slot,.user_data=100,
       ._resv1={0,target.incarnation,scope.incarnation,0}}),"native CONNECT submission");
    NO(!loom_engine_get(l,scope,&held),"native operation observation pin");
    server=srv_accept_blocking(srv_lookup_in(reg,bulk?"large":"small",5),poster->stripes);
    NO(!loom_private_start_executor(l),"native operation worker starts");
    NO(server==held->admission.conn,"stage native accepted peer identity");
    if(fault==9 || fault==10) {error=loom_native_stage_cancel(l,server,&peer,scope,100,&held,close_kind);goto done;}
    NO(loom_native_completion(l,server,&peer,100,(s32)scope.slot),
       "native emitted version and attach complete CONNECT");
    s64 attached=sys_burrow_attach_for_proc(creator,PAGE_SIZE);
    NO(attached>0,"native operation backing");va=(u64)attached;
    struct Burrow *backing=vma_lookup(creator,va)->burrow;
    u8 *bytes=pa_to_kva(page_to_pa(backing->pages));
    struct loom_buf_reg buf={va,PAGE_SIZE};
    NO(!loom_private_register_buffers(l,creator,&buf,1),"native registered payload");
    bytes[0]='f';bytes[1]='i';bytes[2]='l';bytes[3]='e';
    reserve.kind=LOOM_SERVICE_FID;reserve.scope=scope;
    NO(!loom_private_node_prepare(l,creator,&reserve,&child) && !loom_private_node_publish(l,child),
       "native child reservation");
    NO(loom_native_submit(l,creator,(struct loom_sqe){.opcode=LOOM_OP_WALK,
       .handle_idx=scope.slot,.offset=child.slot,.len=4,.user_data=101,
       ._resv1={0,scope.incarnation,child.incarnation,0}}),"native WALK submission");
    bytes[0]='X'; // accepted WALK must own its component copy
    if(fault==11 || fault==12) {error=loom_native_stage_cancel(l,server,&peer,scope,101,&held,close_kind);goto done;}
    NO(loom_native_completion(l,server,&peer,101,(s32)child.slot),"native search and copied WALK completion");
    NO(loom_native_submit(l,creator,(struct loom_sqe){.opcode=LOOM_OP_LOPEN,
       .handle_idx=child.slot,.offset=2,.user_data=102,._resv1={0,child.incarnation,0,0}}),"native LOPEN submission");
    if(fault==13 || fault==14) {error=loom_native_stage_cancel(l,server,&peer,scope,102,&held,close_kind);goto done;}
    NO(loom_native_completion(l,server,&peer,102,0),"native DAC and read-write LOPEN completion");
    NO(l->service_nodes->nodes[child.slot].fid_iounit==4096,"native negotiated iounit retained");
    if(fault) {error=loom_native_fault(l,creator,server,scope,child,bytes,fault==15?9:fault);goto done;}
    if(independent) {
        struct loom_service_reserve another={.size=sizeof(another),.version=1,.kind=LOOM_SERVICE_SCOPE};
        NO(!loom_private_node_prepare(l,creator,&another,&stalled) && !loom_private_node_publish(l,stalled),
           "isolation second scope reservation");
        NO(loom_native_submit(l,creator,(struct loom_sqe){.opcode=LOOM_OP_SERVICE_CONNECT,
           .handle_idx=target.slot,.offset=stalled.slot,.user_data=900,
           ._resv1={0,target.incarnation,stalled.incarnation,0}}),"isolation stalled CONNECT submission");
        stalled_server=srv_accept_blocking(srv_lookup_in(reg,bulk?"large":"small",5),poster->stripes);
        NO(stalled_server,"isolation second peer acceptance");
        struct loom_native_peer quiet;
        for(u32 i=0;i<sizeof(quiet);++i)((u8 *)&quiet)[i]=0;
        quiet.creator=creator;quiet.stop=1;
        bool emitted=false;u64 end=timer_now_ns()+2000000000ull;
        do {
            sched();int step=loom_native_peer_step(stalled_server,&quiet);
            NO(step>=0,"isolation sleeping peer wire mismatch");
            if(step==2){emitted=true;break;}
        }while(timer_now_ns()<end);
        NO(emitted,"isolation second scope must actually stall on VERSION reply");
        peer.owners=2;
    }
    NO(loom_native_submit(l,creator,(struct loom_sqe){.opcode=LOOM_OP_READ,
       .handle_idx=child.slot,.len=5,.user_data=103,._resv1={64,child.incarnation,0,0}}) &&
       loom_native_completion(l,server,&peer,103,5),"native fixed READ completion");
    NO(bytes[64]=='h' && bytes[65]=='e' && bytes[66]=='l' && bytes[67]=='l' && bytes[68]=='o',
       "native READ lands in exact retained registered slice");
    if(independent) {
        struct loom_service_control abort={.size=sizeof(abort),.version=1,.object=stalled};
        struct loom_ring_hdr *h=(void *)(l->ring_kva+l->hdr_off);
        struct loom_cqe *cq=(void *)(l->ring_kva+l->cqe_off);
        u32 head=__atomic_load_n(&h->cq_head,__ATOMIC_ACQUIRE);
        NO(!loom_private_service_abort(l,creator,&abort),"isolation cancels only sleeping scope");
        bool terminal=false;u64 end=timer_now_ns()+2000000000ull;
        do {
            u32 tail=__atomic_load_n(&h->cq_tail,__ATOMIC_ACQUIRE);
            if(tail!=head) {
                struct loom_cqe value=cq[head&(l->cq_entries-1u)];
                NO(tail==head+1 && value.user_data==900 && value.result==-T_E_CANCELED && !value.flags,
                   "isolation exact canceled CONNECT terminal");
                terminal=true;break;
            }
            sched();
        }while(timer_now_ns()<end);
        NO(terminal,"isolation canceled terminal missing");
        __atomic_store_n(&h->cq_head,head+1,__ATOMIC_RELEASE);
        NO(loom_private_submit(l,creator,0)>=0,"isolation canceled terminal consumption");
        bool settled=false;
        do {
            spin_lock(&l->lock);settled=l->service_requests.occupied==1 && !l->service_fixed_count;
            spin_unlock(&l->lock);if(settled)break;sched();
        }while(timer_now_ns()<end);
        struct loom_service_snapshot dead,live;
        NO(settled && !control_native_query(l,creator,stalled,&dead) &&
           dead.state==LOOM_SERVICE_RETIRED && (dead.status_flags&LOOM_SERVICE_LOCAL_RETIRED) &&
           !srvconn_is_live(stalled_server),"isolation only canceled scope locally retires");
        NO(!control_native_query(l,creator,scope,&live) && live.state==LOOM_SERVICE_READY &&
           srvconn_is_live(server),"isolation live media scope retains authority and transport");
        NO(!loom_private_service_reap(l,creator,&abort),"isolation canceled scope reap");
        peer.owners=1;
        // Retain the torn peer during original-scope WRITE/provided/MORE/EOF/CLUNK.
    }
    NO(loom_native_submit(l,creator,(struct loom_sqe){.opcode=LOOM_OP_WRITE,
       .handle_idx=child.slot,.len=5,.user_data=104,._resv1={64,child.incarnation,0,0}}) &&
       loom_native_completion(l,server,&peer,104,5),"native WRITE emits exact read bytes");
    struct loom_service_pool_create pool_in;
    for(u32 i=0;i<sizeof(pool_in);++i)((u8 *)&pool_in)[i]=0;
    pool_in.size=sizeof(pool_in);pool_in.version=1;pool_in.count=1;
    pool_in.members[0]=(struct loom_service_pool_member){.buffer_index=0,.offset=128,.length=5};
    struct loom_service_ref pool={0};
    NO(!loom_private_pool_prepare(l,creator,&pool_in,&pool) && !loom_private_pool_publish(l,pool),
       "native one-member provided pool");
    struct loom_sqe pooled={.opcode=LOOM_OP_READ,.flags=LOOM_SQE_BUFFER_SELECT,
       .handle_idx=child.slot,.buf_idx_or_off=pool.slot,.len=5,.user_data=106,
       ._resv1={0,child.incarnation,pool.incarnation,0}};
    struct loom_service_buffer_receipt receipt={0};
    NO(loom_native_submit(l,creator,pooled) &&
       loom_native_completion_flags(l,server,&peer,106,5,LOOM_CQE_SERVICE_BUFFER,&receipt),"native single provided READ");
    NO(receipt.pool.slot==pool.slot && receipt.pool.incarnation==pool.incarnation &&
       !receipt.member && receipt.lease && bytes[128]=='h' && bytes[132]=='o',
       "native paired receipt names exact payload lease");
    NO(loom_private_pool_reap(l,pool)==-T_E_BUSY,"CQ consumption leaves payload leased");
    pooled.flags|=LOOM_SQE_MULTISHOT;pooled.user_data=107;
    NO(loom_native_submit(l,creator,pooled),"native MORE stream accepted with empty pool");
    for(u32 i=0;i<32;++i)sched();
    NO(peer.seen==9 && loom_native_peer_empty(server),"empty pool emits no premature next READ");
    struct loom_service_buffer_return give={.size=sizeof(give),.version=1,.receipt=receipt};
    NO(!loom_private_service_return(l,creator,&give),"native payload return releases next shot");
    NO(loom_private_service_return(l,creator,&give)<0,"native duplicate return refused before reuse");
    NO(loom_native_completion_flags(l,server,&peer,107,5,LOOM_CQE_MORE|LOOM_CQE_SERVICE_BUFFER,&receipt) &&
       receipt.lease!=give.receipt.lease,"native MORE shot acquires fresh receipt");
    give.receipt=receipt;
    for(u32 i=0;i<l->cq_entries;++i)
        NO(!loom_post_cqe(l,800+i,0,0),"native fills CQ before RETURN");
    u64 epoch=loom_native_epoch(l);
    NO(!loom_private_service_return(l,creator,&give) && loom_native_epoch(l)!=epoch,
       "native RETURN changes wake epoch independently of full CQ");
    for(u32 i=0;i<32;++i)sched();
    NO(peer.seen==10 && loom_native_peer_empty(server),"full CQ prevents next provided dispatch");
    struct loom_ring_hdr *h=(void *)(l->ring_kva+l->hdr_off);
    __atomic_store_n(&h->cq_head,__atomic_load_n(&h->cq_tail,__ATOMIC_ACQUIRE),__ATOMIC_RELEASE);
    NO(loom_private_submit(l,creator,0)==0 &&
       loom_native_completion_flags(l,server,&peer,107,5,LOOM_CQE_MORE|LOOM_CQE_SERVICE_BUFFER,&receipt) &&
       receipt.lease!=give.receipt.lease,"native CQ drain resumes next MORE shot");
    give.receipt=receipt;
    NO(!loom_private_service_return(l,creator,&give) &&
       loom_native_completion_flags(l,server,&peer,107,0,0,&receipt) && !receipt.lease,
       "native EOF ends MORE without transferring an empty payload lease");
    NO(!loom_private_pool_reap(l,pool),"native completed stream releases every pool hold");
    struct loom_service_control child_control={.size=sizeof(child_control),.version=1,.object=child};
    NO(loom_private_service_reap(l,creator,&child_control)==-T_E_BUSY,"native open FID refuses REAP before CLUNK");
    // Hold TCLUNK at the peer: while the CLUNK runs, REAP refuses the FID and
    // QUERY, which reads LIVE slots only, does not see it.
    peer.stop=13;
    NO(loom_native_submit(l,creator,(struct loom_sqe){.opcode=LOOM_OP_CLUNK,
       .handle_idx=child.slot,.user_data=105,._resv1={0,child.incarnation,0,0}}),
       "native CLUNK submitted");
    int held=0;
    for(u64 end=timer_now_ns()+2000000000ull;!held && timer_now_ns()<end;) {
        sched();held=loom_native_peer_step(server,&peer);
    }
    // Read, release, then assert: a failed check must not leave TCLUNK held.
    bool was_held=held==2 && peer.seen==12;
    struct loom_service_snapshot clunking;
    int reap_mid=loom_private_service_reap(l,creator,&child_control);
    int query_mid=control_native_query(l,creator,child,&clunking);
    peer.stop=0;
    NO(loom_native_completion(l,server,&peer,105,0),"native CLUNK completes");
    NO(was_held,"the peer holds TCLUNK unanswered");
    NO(reap_mid==-T_E_BUSY,"native REAP of a FID whose CLUNK is in flight answers -EBUSY");
    NO(query_mid==-T_E_NOENT,"native QUERY of a FID whose CLUNK is in flight answers -ENOENT");
    NO(peer.seen==13 && !peer.used && !l->service_fixed_count && l->service_requests.occupied==1,
       "native exact wire sequence frees general request and fixed owners");
    NO(!loom_private_service_reap(l,creator,&child_control) &&
       loom_private_service_reap(l,creator,&child_control)==-T_E_NOENT,
       "native clunked FID reaps once while its ring lives");
    // A walked FID that is never clunked: its scope's abort ends the session, the
    // FID reaps once the scope has retired, and the scope reaps after it.
    struct loom_service_ref orphan={0};
    NO(!loom_private_node_prepare(l,creator,&reserve,&orphan) && !loom_private_node_publish(l,orphan),
       "native second child reservation");
    bytes[0]='f';bytes[1]='i';bytes[2]='l';bytes[3]='e';
    NO(loom_native_submit(l,creator,(struct loom_sqe){.opcode=LOOM_OP_WALK,
       .handle_idx=scope.slot,.offset=orphan.slot,.len=4,.user_data=108,
       ._resv1={0,scope.incarnation,orphan.incarnation,0}}) &&
       loom_native_completion(l,server,&peer,108,(s32)orphan.slot),"native second WALK completion");
    struct loom_service_control orphan_control={.size=sizeof(orphan_control),.version=1,.object=orphan};
    struct loom_service_control scope_control={.size=sizeof(scope_control),.version=1,.object=scope};
    NO(loom_private_service_reap(l,creator,&orphan_control)==-T_E_BUSY,
       "native walked FID of a live scope refuses REAP");
    NO(!loom_engine_put(l,scope,held),"native returns observation pin");held=NULL;
    NO(!loom_private_service_abort(l,creator,&scope_control),"native scope abort");
    {
        int reaped=-T_E_BUSY;u64 end=timer_now_ns()+2000000000ull;
        do {
            reaped=loom_private_service_reap(l,creator,&orphan_control);
            if(reaped!=-T_E_BUSY)break;
            sched();
        }while(timer_now_ns()<end);
        struct loom_service_snapshot after;
        NO(!reaped && !control_native_query(l,creator,scope,&after) && after.state==LOOM_SERVICE_RETIRED,
           "native walked FID reaps once its aborted scope retires");
        do {
            reaped=loom_private_service_reap(l,creator,&scope_control);
            if(reaped!=-T_E_BUSY)break;
            sched();
        }while(timer_now_ns()<end);
        NO(!reaped && loom_private_service_reap(l,creator,&scope_control)==-T_E_NOENT,
           "native scope reaps after its FIDs");
    }
    NO(!loom_private_register_buffers(l,creator,NULL,0),"native unregister after physical retirement");
    NO(!sys_burrow_detach_for_proc(creator,va,PAGE_SIZE),"native payload mapping release");va=0;
done:
    if(held)(void)loom_engine_put(l,scope,held);
    loom_handle_closed(l);
    if(!loom_close_wait_state(l,LCLOSE_DONE) && !error)error="native operation close timeout";
    if(l->sqpoll && !__atomic_load_n(&l->sqpoll_exited,__ATOMIC_ACQUIRE))
        extinction("native operation fixture worker failed to quiesce");
    if(va)(void)sys_burrow_detach_for_proc(creator,va,PAGE_SIZE);
    if(server)srvconn_unref(server);
    if(stalled_server)srvconn_unref(stalled_server);
#undef NO
    return error;
}

// Repeated failure before server accept returns local metadata/protocol storage.
// The backlog endpoint remains charged until a late accept drops its last ref.
// Alternate ordinary/bulk credits and keep
// the same ring/worker/registry alive across all1000 attempts.
static const char *loom_native_reconnects(struct Loom *l,struct Proc *creator,
    struct Proc *poster,struct SrvRegistry *reg,struct loom_service_ref targets[2],bool retain) {
    if(loom_private_start_executor(l))return "reconnect worker start";
    spin_lock(&creator->as->lock);u32 pages=creator->as->page_count;
    spin_unlock(&creator->as->lock);
    struct loom_ring_hdr *h=(void *)(l->ring_kva+l->hdr_off);
    struct loom_cqe *cq=(void *)(l->ring_kva+l->cqe_off);
    struct loom_service_ref previous={0};
    struct srv_admission_snapshot initial; srv_domain_snapshot(NULL,&initial);
    u64 initial_freed=srvconn_total_freed();
    for(u32 i=0;i<1000;++i) {
        struct loom_service_ref scope={0},target=targets[i&1u];
        struct loom_service_reserve reserve={.size=sizeof(reserve),.version=1,.kind=LOOM_SERVICE_SCOPE};
        if(loom_private_node_prepare(l,creator,&reserve,&scope) || loom_private_node_publish(l,scope))
            return "reconnect scope reservation leaked capacity";
        if(previous.incarnation && scope.incarnation==previous.incarnation)
            return "reconnect reused incarnation";
        u32 head=__atomic_load_n(&h->cq_head,__ATOMIC_ACQUIRE);
        struct SrvService *svc=srv_lookup_in(reg,(i&1u)?"large":"small",5);
        u32 depth=(u32)srv_backlog_depth(svc);
        bool full=depth==SRV_ACCEPT_BACKLOG;
        s32 expected=full?-T_E_NOSPC:-T_E_CANCELED;
        u64 freed=srvconn_total_freed();
        struct srv_admission_snapshot before,retained,after;
        srv_domain_snapshot(NULL,&before);
        if(!loom_native_submit(l,creator,(struct loom_sqe){.opcode=LOOM_OP_SERVICE_CONNECT,
           .handle_idx=target.slot,.offset=scope.slot,.user_data=10000+i,
           ._resv1={0,target.incarnation,scope.incarnation,0}}))return "reconnect CONNECT submission";
        if((u32)srv_backlog_depth(svc)!=depth+(full?0u:1u))return "reconnect bounded native publication";
        struct loom_service_control control={.size=sizeof(control),.version=1,.object=scope};
        if(!full && loom_private_service_abort(l,creator,&control)<0)return "reconnect abort before accept";
        u64 end=timer_now_ns()+2000000000ull;bool completed=false;
        do {
            u32 tail=__atomic_load_n(&h->cq_tail,__ATOMIC_ACQUIRE);
            if(tail!=head) {
                struct loom_cqe value=cq[head&(l->cq_entries-1u)];
                if(tail!=head+1 || value.user_data!=10000+i || value.result!=expected || value.flags)
                    return "reconnect exact single canceled completion";
                completed=true;break;
            }
            sched();
        }while(timer_now_ns()<end);
        if(!completed)return "reconnect cancellation stalls";
        __atomic_store_n(&h->cq_head,head+1,__ATOMIC_RELEASE);
        if(loom_private_submit(l,creator,0)<0)return "reconnect CQ acknowledgement";
        int reaped=-T_E_BUSY;
        do {
            struct loom_service_snapshot snap;
            if(control_native_query(l,creator,scope,&snap))return "reconnect retirement query";
            if(snap.state==LOOM_SERVICE_RETIRED) {
                if(snap.reason!=expected || !(snap.status_flags&LOOM_SERVICE_LOCAL_RETIRED))
                    return "reconnect retirement diagnostics";
                reaped=loom_private_service_reap(l,creator,&control);
                if(!reaped)break;
                if(reaped!=-T_E_BUSY)return "reconnect unexpected reap failure";
            }
            sched();
        }while(timer_now_ns()<end);
        if(reaped)return "reconnect physical protocol owner retained";
        bool settled=false;
        do {
            spin_lock(&l->lock);bool empty=!l->service_requests.occupied && !l->service_engine->count;
            spin_unlock(&l->lock);
            spin_lock(&creator->as->lock);bool refunded=creator->as->page_count==pages;
            spin_unlock(&creator->as->lock);
            settled=empty && refunded;
            if(settled)break;
            sched();
        }while(timer_now_ns()<end);
        if(!settled)return "reconnect attempt leaks local protocol charge or request";
        srv_domain_snapshot(NULL,&retained);
        u32 added=full?0u:1u,weight=(i&1u)?4u:1u;
        if(srvconn_total_freed()!=freed+(full?1u:0u) ||
           (u32)srv_backlog_depth(svc)!=depth+added ||
           retained.global_count!=before.global_count+added ||
           retained.global_credits!=before.global_credits+added*weight)
            return "reconnect retained backlog or rejected admission charge";
        if(!retain) {
            struct SrvConn *late=srv_accept_blocking(svc,poster->stripes);
            if(!late)return "reconnect late accept";
            bool torn=!srvconn_is_live(late);srvconn_unref(late);
            if(!torn)return "reconnect locally retired transport remained live";
            srv_domain_snapshot(NULL,&after);
            if(srvconn_total_freed()!=freed+1 || srv_backlog_depth(svc) ||
               after.global_count!=before.global_count || after.global_credits!=before.global_credits)
                return "reconnect final endpoint must refund native charge";
        }
        if(loom_private_service_abort(l,creator,&control)!=-T_E_NOENT)
            return "reconnect stale scope retains authority";
        previous=scope;
    }
    if(retain) {
        //1000failures with no server acceptance:32torn queued endpoints remain,
        //968new attempts must fail admission without growing retained storage.
        struct srv_admission_snapshot held; srv_domain_snapshot(NULL,&held);
        if(held.global_count!=initial.global_count+2*SRV_ACCEPT_BACKLOG ||
           held.global_credits!=initial.global_credits+5*SRV_ACCEPT_BACKLOG)
            return "reconnect1000 attempts exceed bounded held endpoint budget";
        for(u32 n=0;n<2;++n) {
            struct SrvService *svc=srv_lookup_in(reg,n?"large":"small",5);
            if(srv_backlog_depth(svc)!=SRV_ACCEPT_BACKLOG)return "reconnect expected saturated backlog";
            for(u32 i=0;i<SRV_ACCEPT_BACKLOG;++i) {
                struct SrvConn *late=srv_accept_blocking(svc,poster->stripes);
                if(!late)return "reconnect drain retained endpoint";
                bool torn=!srvconn_is_live(late);srvconn_unref(late);
                if(!torn)return "reconnect saturated backlog endpoint is live";
            }
        }
        struct srv_admission_snapshot final; srv_domain_snapshot(NULL,&final);
        if(final.global_count!=initial.global_count || final.global_credits!=initial.global_credits ||
           srvconn_total_freed()!=initial_freed+1000)
            return "reconnect complete retained endpoint drain does not refund";
    }
    return NULL;
}

static const char *loom_protocol_fixture(u32 kind) {
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
    bool owner_dropped = false, creator_linked = false;
#define PO(c,m) do { if (!(c)) { error=m; goto done; } } while (0)
    PO(p && poster,"protocol fixture processes");
    p->principal_id = 42;
    // proc_alloc alone is deliberately invisible to the production tree walk.
    // Unlike the older direct-Proc Loom fixtures, admission must exercise that
    // lookup, so publish the isolated creator through the existing test hook.
    proc_test_link(p);creator_linked=true;
    struct ProcServiceIdentity initial_identity;
    PO(proc_service_snapshot_by_stripes(p->stripes,p->as,&initial_identity) &&
       initial_identity.access.principal_id==42,"protocol creator is live in process tree");
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
    if (kind==2) {
        u32 before=p->as->page_count;
        loom_private_engine_fault_arm_for_test();
        struct Loom *failed=loom_create_private(p,2,2,true);
        if (failed) { ++goal; loom_unref(failed); }
        PO(!failed && !loom_private_engine_fault_armed_for_test() &&
           p->as->page_count==before && !p->as->private_rings,
           "protocol index allocation failure unwinds every charged owner");
    }
    l=loom_create_private(p,2,(kind==100 || kind==101)?4:2,true);
    if (l) ++goal;
    PO(l,"protocol private owner");
    for (u32 n=0;n<2;++n) {
        struct loom_service_target in;
        for (u32 i=0;i<sizeof(in);++i) ((u8 *)&in)[i]=0;
        in.size=sizeof(in);in.version=1;in.registry_fd=fd;in.name_len=5;
        for (u32 i=0;i<5;++i) in.name[i]=names[n][i];
        PO(!loom_private_target_prepare(l,p,&in,&refs[n]),"protocol target prepare");
        PO(!loom_private_target_publish(l,refs[n]),"protocol target publish");
    }
    if (kind==1) { error=loom_connect_cases(l,p,poster,reg,refs); goto done; }
    if (kind==2) { error=loom_engine_cases(l,p,poster,reg,refs); goto done; }
    if (kind==10) {error=loom_control_cases(l,p,poster,reg,refs);goto done;}
    if (kind==11) {error=loom_executor_case(&l,p,poster,reg,refs,goal,base);goto done;}
    if (kind==12) {
        PO(!loom_private_start_executor(l),"empty private executor starts once");
        bool armed=false;u64 end=timer_now_ns()+5000000000ull;
        do {
            spin_lock(&l->lock);armed=l->service_engine->wait_active;spin_unlock(&l->lock);
            if(armed)break;sched();
        }while(timer_now_ns()<end);
        PO(armed,"empty private executor parks with a retained wait owner");
        // No handle-close root: the actual last creation reference enqueues
        // zero-reference destruction while the worker is still parked/alive.
        loom_unref(l);l=NULL;
        PO(lp_wait(goal) && p->loom_sqpoll_count==0 && p->as->page_count==base && !p->as->private_rings,
           "zero-reference retirer wakes joins and refunds the live private worker");
        goto done;
    }
    if (kind==102 || kind==103) {
        bool bulk=kind==103;
        error=loom_native_operations_case(l,p,poster,reg,refs[bulk],bulk,15);
        goto done;
    }
    if (kind==100 || kind==101) {
        bool bulk=kind==101;
        error=loom_native_operations_case(l,p,poster,reg,refs[bulk],bulk,40);
        goto done;
    }
    if (kind>=60 && kind<=95) {
        bool bulk=(kind-60)&1u;
        error=loom_native_operations_case(l,p,poster,reg,refs[bulk],bulk,20+(kind-60)/2);
        goto done;
    }
    if (kind>=40 && kind<=51) {
        bool bulk=(kind-40)&1u;
        error=loom_native_operations_case(l,p,poster,reg,refs[bulk],bulk,9+(kind-40)/2);
        goto done;
    }
    if (kind==35 || kind==36) {error=loom_native_reconnects(l,p,poster,reg,refs,kind==36);goto done;}
    if (kind>=19 && kind<=34) {
        bool bulk=(kind-19)&1u;
        error=loom_native_operations_case(l,p,poster,reg,refs[bulk],bulk,1+(kind-19)/2);
        goto done;
    }
    if (kind==17 || kind==18) {error=loom_native_operations_case(l,p,poster,reg,refs[kind==18],kind==18,0);goto done;}
    if (kind==15 || kind==16) {error=loom_operation_storage_case(l,p,poster,reg,refs[kind==16],kind==16,true);goto done;}
    if (kind==13 || kind==14) {error=loom_operation_storage_case(l,p,poster,reg,refs[kind==14],kind==14,false);goto done;}
    if (kind>=7) {error=loom_close_boundary_case(&l,p,poster,reg,refs[0],kind-6,goal,base);goto done;}
    if (kind>=3) {error=loom_close_cases(&l,p,poster,reg,refs,kind,goal,base);goto done;}
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
    if(p && !error) {
        spin_lock(&p->as->lock);
        bool refunded=p->as->page_count==base && !p->as->private_rings;
        spin_unlock(&p->as->lock);
        if(!refunded)error="protocol cleanup leaves image pages or private ring charge";
    }
    if (root) spoor_clunk(root);
    if (creator_linked) proc_test_unlink(p);
    test_proc_drop(p);test_proc_drop(poster);
    if (reg) srv_registry_unref(reg);
#undef PO
    return error;
}
void test_loom_private_protocol(void);
void test_loom_private_protocol(void) {
    const char *error=loom_protocol_fixture(false);
    TEST_ASSERT(!error,error);
}

void test_loom_private_connect(void);
void test_loom_private_connect(void) {
    const char *error=loom_protocol_fixture(true);
    TEST_ASSERT(!error,error);
}

void test_loom_private_engine(void);
void test_loom_private_engine(void) {
    const char *error=loom_protocol_fixture(2);
    TEST_ASSERT(!error,error);
}

void test_loom_private_close(void);
void test_loom_private_close(void) {
    for (u32 kind=3;kind<=9;++kind) {
        const char *error=loom_protocol_fixture(kind);
        TEST_ASSERT(!error,error);
    }
}

void test_loom_private_control(void);
void test_loom_private_control(void) {
    const char *error=loom_protocol_fixture(10);
    TEST_ASSERT(!error,error);
}

void test_loom_private_executor(void);
void test_loom_private_executor(void) {
    const char *error=loom_protocol_fixture(11);
    TEST_ASSERT(!error,error);
    error=loom_protocol_fixture(12);
    TEST_ASSERT(!error,error);
}

void test_loom_private_operation_storage(void);
void test_loom_private_operation_storage(void) {
    const char *error=loom_protocol_fixture(13);
    TEST_ASSERT(!error,error);
    error=loom_protocol_fixture(14);
    TEST_ASSERT(!error,error);
}

void test_loom_private_frontend(void);
void test_loom_private_frontend(void) {
    const char *error=loom_protocol_fixture(15);
    if(!error)error=loom_protocol_fixture(16);
    TEST_ASSERT(!error,error);
}

void test_loom_private_native_operations(void);
void test_loom_private_native_operations(void) {
    const char *error=loom_protocol_fixture(17);
    if(!error)error=loom_protocol_fixture(18);
    TEST_ASSERT(!error,error);
}

void test_loom_private_native_faults(void);
void test_loom_private_native_faults(void) {
    for(u32 kind=19;kind<=34;++kind) {
        const char *error=loom_protocol_fixture(kind);
        TEST_ASSERT(!error,error);
    }
    for(u32 kind=102;kind<=103;++kind) {
        const char *error=loom_protocol_fixture(kind);
        TEST_ASSERT(!error,error);
    }
}

void test_loom_private_native_reconnects(void);
void test_loom_private_native_reconnects(void) {
    const char *error=loom_protocol_fixture(35);
    if(!error)error=loom_protocol_fixture(36);
    TEST_ASSERT(!error,error);
}

void test_loom_private_native_stages(void);
void test_loom_private_native_stages(void) {
    for(u32 kind=40;kind<=51;++kind) {
        const char *error=loom_protocol_fixture(kind);
        TEST_ASSERT(!error,error);
    }
}

void test_loom_private_native_stage_close(void);
void test_loom_private_native_stage_close(void) {
    for(u32 kind=60;kind<=95;++kind) {
        const char *error=loom_protocol_fixture(kind);
        TEST_ASSERT(!error,error);
    }
}

void test_loom_private_native_isolation(void);
void test_loom_private_native_isolation(void) {
    for(u32 kind=100;kind<=101;++kind) {
        const char *error=loom_protocol_fixture(kind);
        TEST_ASSERT(!error,error);
    }
}
