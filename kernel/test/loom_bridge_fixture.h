// The private Loom syscall bridge (docs/ASYNC-SERVICE-ABI.md, Qualification
// bridge). One body per image: the default image must route nothing; the
// qualification image routes setup, ENTER and register ops 2..9.
#include <thylacine/loom_service_syscall.h>

static u8  bridge_out[sizeof(struct loom_service_pool_create)];
static u32 bridge_out_len;
#define BRIDGE_FAULT ((void *)1)
static int bridge_copyout(void *ctx, u32 offset, const void *src, u32 len) {
    if (ctx == BRIDGE_FAULT || offset + len > sizeof(bridge_out)) return -T_E_FAULT;
    for (u32 i = 0; i < len; i++) bridge_out[offset + i] = ((const u8 *)src)[i];
    bridge_out_len = offset + len;
    return 0;
}
static u8 bridge_record[sizeof(struct loom_service_pool_create)];
static void bridge_zero(void) {
    for (u32 i = 0; i < sizeof(bridge_record); i++) bridge_record[i] = 0;
    for (u32 i = 0; i < sizeof(bridge_out); i++) bridge_out[i] = 0;
    bridge_out_len = 0;
}

#ifndef LOOM_PRIVATE_QUALIFICATION

static const char *loom_bridge_fixture_run(void) {
    const char *error = NULL;
    struct Proc *p = test_proc_make();
    if (!p) return "bridge proc";
    u32 base = p->as->page_count;
    struct loom_params out;
    hidx_t fd = -1;
    int a = loom_private_setup_for_proc(p, 2, LOOM_SETUP_PRIVATE_SERVICE, &out, &fd);
    int b = loom_private_setup_for_proc(p, 2, LOOM_SETUP_PRIVATE_SERVICE |
                                              LOOM_SETUP_SERVICE_BUFFERS, &out, &fd);
    int c = loom_private_setup_for_proc(p, 2, LOOM_SETUP_SERVICE_BUFFERS, &out, &fd);
    u32 rings_after_setup = p->as->private_rings;
    u32 pages_after_setup = p->as->page_count;
    // Every private register op on an ordinary ring, and ENTER, refuse too.
    struct loom_params kp;
    hidx_t lfd = -1;
    struct Loom *ll = NULL;
    int legacy = sys_loom_setup_held_for_proc(p, 2, 0, &kp, &lfd, &ll);
    u32 refused = 0;
    int enter = 0;
    if (!legacy) {
        bridge_zero();
        for (u32 op = LOOM_REGISTER_SERVICE_TARGET; op <= LOOM_REGISTER_QUERY_SERVICE_POOL; op++)
            if (loom_private_register_record(ll, p, op, bridge_record,
                                             bridge_copyout, NULL) == -T_E_OPNOTSUPP) refused++;
        enter = loom_private_enter(ll, p, 0, 0, 0);
        sys_loom_setup_rollback(p, lfd, ll, kp.ring_va, kp.ring_size);
    }
    u32 settled = p->as->page_count;
    test_proc_drop(p);
#define BR(cond, msg) do { if (!error && !(cond)) error = msg; } while (0)
    BR(!loom_private_routed(), "the default image routes no private syscall");
    BR(a == -T_E_OPNOTSUPP && b == -T_E_OPNOTSUPP && c == -T_E_OPNOTSUPP,
       "default image: private setup refuses EOPNOTSUPP");
    BR(rings_after_setup == 0 && pages_after_setup == base,
       "default image: a refused private setup charges nothing");
    BR(legacy == 0, "an ordinary ring is still made");
    BR(refused == 8, "default image: every private register op 2..9 refuses EOPNOTSUPP");
    BR(enter == -T_E_OPNOTSUPP, "default image: private ENTER refuses EOPNOTSUPP");
    BR(settled == base, "the ordinary ring's rollback settles its charge exactly");
#undef BR
    return error;
}

#else

#include <thylacine/caps.h>
#include <thylacine/weft.h>

extern void proc_test_link(struct Proc *p);
extern void proc_test_unlink(struct Proc *p);
extern s64 sys_weft_share_for_proc(struct Proc *p, u64 ring_va, u64 ring_size_raw);

// Weft-share [va, va+len) with CAP_HW_CREATE lent for the call, then consume
// any share it registered so nothing outlives the probe. The share's id, or -1.
static s64 bridge_weft_share(struct Proc *p, u64 va, u64 len) {
    u64 caps = __atomic_load_n(&p->caps, __ATOMIC_ACQUIRE);
    __atomic_store_n(&p->caps, caps | CAP_HW_CREATE, __ATOMIC_RELEASE);
    s64 id = sys_weft_share_for_proc(p, va, len);
    __atomic_store_n(&p->caps, caps, __ATOMIC_RELEASE);
    if (id > 0) {
        struct Burrow *v = weft_share_claim((u64)id);
        if (v) burrow_unref(v);
    }
    return id;
}

static struct loom_service_ref bridge_ref_at(u32 offset) {
    struct loom_service_ref r;
    for (u32 i = 0; i < sizeof(r); i++) ((u8 *)&r)[i] = bridge_out[offset + i];
    return r;
}

static int bridge_reg(struct Loom *l, struct Proc *p, u32 op, void *fault) {
    for (u32 i = 0; i < sizeof(bridge_out); i++) bridge_out[i] = 0;
    return loom_private_register_record(l, p, op, bridge_record, bridge_copyout, fault);
}
static void bridge_header(u32 size) {
    bridge_zero();
    bridge_record[0] = (u8)size; bridge_record[1] = (u8)(size >> 8);
    bridge_record[4] = (u8)LOOM_SERVICE_ABI_VERSION;
}
static void bridge_ref_into(u32 offset, struct loom_service_ref r) {
    for (u32 i = 0; i < sizeof(r); i++) bridge_record[offset + i] = ((const u8 *)&r)[i];
}
static u32 bridge_live_slots(struct Loom *l) {
    u32 n = 0;
    spin_lock(&l->lock);
    for (u32 i = 0; i < LOOM_SERVICE_SLOT_COUNT; i++)
        if (l->service_slots->slots[i].phase != LSLOT_FREE) n++;
    spin_unlock(&l->lock);
    return n;
}

static const char *loom_bridge_fixture_run(void) {
    const char *error = NULL;
    struct Proc *p = test_proc_make(), *q = test_proc_make(), *poster = test_proc_make();
    struct SrvRegistry *reg = NULL;
    struct Spoor *root = NULL;
    struct Loom *l = NULL, *pl = NULL;
    struct Handle h;
    bool held = false, linked = false, ring_open = false;
    struct loom_params out;
    hidx_t fd = -1;
    s64 buf_va = 0;
    u64 goal = loom_private_retired();
    u32 base = p ? p->as->page_count : 0;
#define BQ(cond, msg) do { if (!(cond)) { error = msg; goto done; } } while (0)
    BQ(p && q && poster, "bridge procs");
    BQ(loom_private_routed(), "the qualification image routes the private syscalls");
    proc_test_link(p); linked = true;   // the executor's thread ticket walks the tree

    // Setup refusals charge nothing.
    const u32 P = LOOM_SETUP_PRIVATE_SERVICE, B = LOOM_SETUP_SERVICE_BUFFERS;
    BQ(loom_private_setup_for_proc(p, 2, B, &out, &fd) == -T_E_INVAL,
       "SERVICE_BUFFERS without PRIVATE_SERVICE refused");
    BQ(loom_private_setup_for_proc(p, 2, P | LOOM_SETUP_SQPOLL, &out, &fd) == -T_E_INVAL,
       "SQPOLL with a private ring refused (its executor is implicit)");
    BQ(loom_private_setup_for_proc(p, 2, P | 0x100u, &out, &fd) == -T_E_INVAL,
       "an unknown setup bit refused");
    BQ(loom_private_setup_for_proc(p, 0, P, &out, &fd) == -T_E_INVAL &&
       loom_private_setup_for_proc(p, 3, P, &out, &fd) == -T_E_INVAL &&
       loom_private_setup_for_proc(p, LOOM_MAX_ENTRIES * 2u, P, &out, &fd) == -T_E_INVAL,
       "zero, non-power-of-two and oversized entries refused");
    BQ(p->as->page_count == base && !p->as->private_rings, "refused setups charge nothing");

    // A private ring with receipts.
    BQ(loom_private_setup_held_for_proc(p, 2, P | B, &out, &fd, &pl) == 0, "private setup succeeds");
    ++goal; ring_open = true;
    BQ(handle_get(p, fd, &h) == 0 && h.kind == KOBJ_LOOM, "the private ring's handle");
    held = true; l = (struct Loom *)h.obj;
    BQ(loom_is_private(l) && l->sqpoll, "the ring is private and its executor runs");
    BQ(out.flags == (P | B) && out.sq_entries == 2 && out.cq_entries == 4 &&
       out.ring_size == l->ring_size && out.cqe_off == l->cqe_off,
       "params echo the granted flags and geometry");
    BQ(out._resv1[0] == l->receipt_off && out._resv1[1] == l->receipt_size &&
       out._resv1[2] == sizeof(struct loom_service_buffer_receipt) &&
       out._resv1[1] == 4u * sizeof(struct loom_service_buffer_receipt) && !out._resv1[3],
       "params carry the receipt geometry");
    spin_lock(&p->as->lock);
    struct Vma *rv = vma_lookup_in(p->as, out.ring_va);
    bool tagged = rv && rv->burrow == l->ring && rv->vaddr_start == out.ring_va &&
                  (rv->flags & VMA_FLAG_PRIVATE_RING);
    spin_unlock(&p->as->lock);
    BQ(tagged, "the ring mapping is tagged private, so fork omits it");

    // Nor can it reach another Proc through a weft share, while the same caller
    // still shares an ordinary region of its own.
    s64 plain_va = sys_burrow_attach_for_proc(p, PAGE_SIZE);
    s64 plain_share = plain_va > 0 ? bridge_weft_share(p, (u64)plain_va, PAGE_SIZE) : -1;
    if (plain_va > 0) (void)sys_burrow_detach_for_proc(p, (u64)plain_va, PAGE_SIZE);
    s64 ring_share = bridge_weft_share(p, out.ring_va, out.ring_size);
    BQ(plain_share > 0, "an ordinary region of the same caller weft-shares");
    BQ(ring_share == -1, "a private ring cannot be weft-shared");

    // ENTER never sleeps, and belongs to the creator image.
    BQ(loom_private_enter(l, p, 0, 1, 0) == -T_E_INVAL, "ENTER with min_complete refused");
    BQ(loom_private_enter(l, p, 0, 0, LOOM_ENTER_GETEVENTS) == -T_E_INVAL,
       "ENTER with GETEVENTS refused");
    BQ(loom_private_enter(l, p, 0, 0, LOOM_ENTER_NONBLOCK) == 0, "a zero-submit kick");
    BQ(loom_private_enter(l, q, 0, 0, 0) == -T_E_ACCES, "another image cannot ENTER");
    BQ(sys_loom_register_for_proc(p, fd, LOOM_REGISTER_HANDLES, NULL, 0) == -T_E_OPNOTSUPP,
       "HANDLES on a private ring refused");

    // An ordinary ring in the same image stays out of the private inners.
    struct loom_params kp;
    hidx_t lfd = -1;
    struct Loom *ll = NULL;
    int legacy = sys_loom_setup_held_for_proc(p, 2, 0, &kp, &lfd, &ll);
    int legacy_enter = 0, legacy_reg = 0;
    if (!legacy) {
        bridge_header(sizeof(struct loom_service_reserve));
        legacy_enter = loom_private_enter(ll, p, 0, 0, 0);
        legacy_reg = bridge_reg(ll, p, LOOM_REGISTER_SERVICE_SLOT, NULL);
        sys_loom_setup_rollback(p, lfd, ll, kp.ring_va, kp.ring_size);
    }
    BQ(legacy == 0, "an ordinary ring beside the private one");
    BQ(legacy_enter == -T_E_INVAL, "private ENTER refuses an ordinary ring");
    BQ(legacy_reg == -T_E_OPNOTSUPP, "a private register op refuses an ordinary ring");

    // SLOT: published only once the caller holds its result; a fault burns it.
    u32 slots0 = bridge_live_slots(l);
    bridge_header(sizeof(struct loom_service_reserve));
    bridge_record[8] = (u8)LOOM_SERVICE_SCOPE;
    BQ(bridge_reg(l, p, LOOM_REGISTER_SERVICE_SLOT, NULL) == 0 &&
       bridge_out_len == sizeof(struct loom_service_reserve),
       "a scope reservation copies its result out");
    struct loom_service_ref first = bridge_ref_at(32);
    BQ(first.incarnation && bridge_live_slots(l) == slots0 + 1, "the scope is published");
    BQ(bridge_reg(l, p, LOOM_REGISTER_SERVICE_SLOT, BRIDGE_FAULT) == -T_E_FAULT &&
       bridge_live_slots(l) == slots0 + 1, "a faulted result copyout rolls the scope back");
    BQ(bridge_reg(l, p, LOOM_REGISTER_SERVICE_SLOT, NULL) == 0, "the next scope reservation");
    struct loom_service_ref third = bridge_ref_at(32);
    BQ(third.incarnation == first.incarnation + 2,
       "the rolled-back reservation burned its incarnation");
    bridge_record[4] = 2;
    BQ(bridge_reg(l, p, LOOM_REGISTER_SERVICE_SLOT, NULL) == -T_E_INVAL,
       "an unknown record version refused");
    BQ(bridge_reg(l, p, 10, NULL) == -T_E_INVAL, "an op past 9 refused");

    // QUERY writes only on success; REAP removes an unused scope.
    bridge_header(sizeof(struct loom_service_snapshot));
    bridge_ref_into(8, first);
    BQ(bridge_reg(l, p, LOOM_REGISTER_QUERY_SERVICE_SLOT, NULL) == 0 &&
       bridge_out_len == sizeof(struct loom_service_snapshot) &&
       bridge_out[24] == LOOM_SERVICE_SCOPE && bridge_out[28] == LOOM_SERVICE_RESERVED,
       "QUERY reports the reserved scope");
    BQ(bridge_reg(l, p, LOOM_REGISTER_QUERY_SERVICE_SLOT, BRIDGE_FAULT) == -T_E_FAULT,
       "a faulted QUERY copyout reports EFAULT");
    bridge_header(sizeof(struct loom_service_control));
    bridge_ref_into(8, first);
    BQ(bridge_reg(l, p, LOOM_REGISTER_REAP_SERVICE_SLOT, NULL) == 0, "REAP an unused scope");
    BQ(bridge_reg(l, p, LOOM_REGISTER_ABORT_SCOPE, NULL) == -T_E_NOENT,
       "ABORT of a reaped scope finds nothing");
    bridge_ref_into(8, third);
    BQ(bridge_reg(l, p, LOOM_REGISTER_REAP_SERVICE_SLOT, NULL) == 0, "REAP the second scope");

    // POOL: a faulted create leaves no pool behind (the buffer table stays replaceable).
    buf_va = sys_burrow_attach_for_proc(p, PAGE_SIZE);
    BQ(buf_va > 0, "a buffer page");
    struct loom_buf_reg br = { .va = (u64)buf_va, .len = PAGE_SIZE };
    BQ(loom_private_register_buffers_routed(l, p, &br, 1) == 0, "private buffer registration");
    bridge_header(sizeof(struct loom_service_pool_create));
    bridge_record[8] = 1;                                   // count
    for (u32 i = 0; i < 8; i++) bridge_record[32 + 16 + i] = (u8)((u64)PAGE_SIZE >> (8 * i));
    BQ(bridge_reg(l, p, LOOM_REGISTER_SERVICE_POOL, BRIDGE_FAULT) == -T_E_FAULT,
       "a faulted pool result copyout");
    BQ(loom_private_register_buffers_routed(l, p, &br, 1) == 0,
       "no pool survives a faulted create");
    BQ(bridge_reg(l, p, LOOM_REGISTER_SERVICE_POOL, NULL) == 0, "pool create");
    struct loom_service_ref pool = bridge_ref_at(16);
    BQ(pool.incarnation && loom_private_register_buffers_routed(l, p, &br, 1) == -T_E_BUSY,
       "the published pool holds the buffer table");
    bridge_header(sizeof(struct loom_service_pool_snapshot));
    bridge_ref_into(8, pool);
    BQ(bridge_reg(l, p, LOOM_REGISTER_QUERY_SERVICE_POOL, NULL) == 0 &&
       bridge_out[24] == 1 && bridge_out[28] == 1, "POOL QUERY: one member, available");
    bridge_header(sizeof(struct loom_service_buffer_return));
    bridge_ref_into(8, pool);
    bridge_record[32] = 1;                                  // a lease never issued
    BQ(bridge_reg(l, p, LOOM_REGISTER_RETURN_SERVICE_BUFFER, NULL) == -T_E_NOENT,
       "RETURN of a lease never issued finds nothing");
    bridge_header(sizeof(struct loom_service_control));
    bridge_ref_into(8, pool);
    BQ(bridge_reg(l, p, LOOM_REGISTER_REAP_SERVICE_SLOT, NULL) == 0, "REAP the pool");
    BQ(loom_private_register_buffers_routed(l, p, NULL, 0) == 0, "the buffer table released");

    // TARGET resolves an actual registry entry; a fault burns it.
    proc_mark_may_post_service(poster);
    reg = srv_registry_create();
    BQ(reg, "bridge registry");
    root = devsrv_attach_registry(reg);
    BQ(root, "bridge registry root");
    root->flag |= CWALKONLY;
    BQ(devsrv_post_listener(poster, root, "small", 5, SRV_MODE_9P, false, false, false) >= 0,
       "a strict native post");
    spoor_ref(root);
    int rfd = handle_alloc(p, KOBJ_SPOOR, RIGHT_READ, root);
    if (rfd < 0) spoor_clunk(root);
    BQ(rfd >= 0, "the registry navigation descriptor");
    bridge_header(sizeof(struct loom_service_target));
    for (u32 i = 0; i < 4; i++) bridge_record[8 + i] = (u8)((u32)rfd >> (8 * i));
    bridge_record[12] = 5;
    for (u32 i = 0; i < 5; i++) bridge_record[16 + i] = (u8)"small"[i];
    u32 slots1 = bridge_live_slots(l);
    BQ(bridge_reg(l, p, LOOM_REGISTER_SERVICE_TARGET, BRIDGE_FAULT) == -T_E_FAULT &&
       bridge_live_slots(l) == slots1, "a faulted target copyout rolls the target back");
    BQ(bridge_reg(l, p, LOOM_REGISTER_SERVICE_TARGET, NULL) == 0, "target registration");
    struct loom_service_ref target = bridge_ref_at(272);
    bridge_header(sizeof(struct loom_service_snapshot));
    bridge_ref_into(8, target);
    BQ(bridge_reg(l, p, LOOM_REGISTER_QUERY_SERVICE_SLOT, NULL) == 0 &&
       bridge_out[24] == LOOM_SERVICE_TARGET && bridge_out[28] == LOOM_SERVICE_READY,
       "QUERY reports the ready target");
    bridge_header(sizeof(struct loom_service_control));
    bridge_ref_into(8, target);
    BQ(bridge_reg(l, p, LOOM_REGISTER_REAP_SERVICE_SLOT, NULL) == 0, "REAP the target");
    BQ(bridge_live_slots(l) == slots0, "every slot this fixture took is free again");

done:
    if (held) handle_put(&h);
    if (ring_open) sys_loom_setup_rollback(p, fd, pl, out.ring_va, out.ring_size);
    if (buf_va > 0) (void)sys_burrow_detach_for_proc(p, (u64)buf_va, PAGE_SIZE);
    if (!lp_wait(goal) && !error) error = "bridge ring retirement timeout";
    if (p && !error && (p->as->page_count != base || p->as->private_rings))
        error = "the private ring's rollback settles its charge exactly";
    if (root) spoor_clunk(root);
    if (linked) proc_test_unlink(p);
    test_proc_drop(p); test_proc_drop(q); test_proc_drop(poster);
    if (reg) srv_registry_unref(reg);
#undef BQ
    return error;
}

#endif

// The setup rollback that runs when params cannot be copied out. It must settle
// the ring's charge however its close and its detach interleave.
static const char *loom_setup_rollback_fixture_run(void) {
    struct Proc *p = test_proc_make();
    if (!p) return "rollback proc";
    u32 base = p->as->page_count;
    struct loom_params kp;
    hidx_t fd = -1;
    struct Loom *l = NULL;
    int rc = sys_loom_setup_held_for_proc(p, 4, 0, &kp, &fd, &l);
    u32 charged = p->as->page_count;
    if (!rc) sys_loom_setup_rollback(p, fd, l, kp.ring_va, kp.ring_size);
    struct Handle h;
    bool closed = !rc && handle_get(p, fd, &h) != 0;
    if (!closed && !rc) handle_put(&h);
    u32 settled = p->as->page_count;
    test_proc_drop(p);
    if (rc) return "an ordinary ring to roll back";
    if (charged == base) return "the ordinary ring charged its pages";
    if (!closed) return "the rollback closes the handle";
    if (settled != base) return "the rollback settles the ring's charge exactly";
    return NULL;
}

// The rollback after a setup published its descriptor and its ring. A sibling
// thread may close and reuse the number, or detach the ring and map its own
// region at the VA, before the rollback runs: it must undo only what is still
// this ring's. And a setup refused after the map must leave a ring mapping it
// could not detach still charged, settling when that mapping's last drop goes.
static u64 setup_trim_va;
static s64 setup_trim_rc;
static void setup_trim_ring(struct Proc *p, u64 va) {
    setup_trim_va = va;
    setup_trim_rc = sys_burrow_detach_for_proc(p, va, PAGE_SIZE);
}
static bool setup_maps_region_at(struct Proc *p, u64 va, const struct Burrow *ring) {
    spin_lock(&p->as->lock);
    struct Vma *v = vma_lookup_in(p->as, va);
    bool yes = v && v->vaddr_start == va && v->burrow && v->burrow != ring;
    spin_unlock(&p->as->lock);
    return yes;
}
static const char *loom_setup_identity_fixture_run(void) {
    const char *error = NULL;
    struct Proc *p = test_proc_make();
    if (!p) return "identity proc";
    hidx_t rd = -1, wr = -1;
    int pipe_rc = sys_pipe_for_proc(p, &rd, &wr);
    u32 base = p->as->page_count;
    u64 destroyed0 = loom_total_destroyed();
    struct loom_params kp = { 0 };
    hidx_t fd = -1;
    struct Loom *l = NULL;

    // A: the number closed and reused for another file.
    int a_rc = pipe_rc ? -1 : sys_loom_setup_held_for_proc(p, 4, 0, &kp, &fd, &l);
    hidx_t a_fd = fd, a_dup = -1;
    u64 a_va = kp.ring_va;
    bool a_kept = false, a_unmapped = false;
    if (!a_rc) {
        (void)handle_close(p, fd);
        a_dup = handle_dup(p, rd, RIGHT_READ);
        sys_loom_setup_rollback(p, fd, l, kp.ring_va, kp.ring_size);
        struct Handle h;
        if (handle_get(p, fd, &h) == 0) { a_kept = h.kind == KOBJ_SPOOR; handle_put(&h); }
        a_unmapped = vma_lookup(p, a_va) == NULL;
        if (a_dup >= 0) (void)handle_close(p, a_dup);
    }
    u64 a_destroyed = loom_total_destroyed() - destroyed0;
    u32 a_settled = p->as->page_count;

    // B: the ring detached and another region mapped at its VA.
    int b_rc = a_rc ? -1 : sys_loom_setup_held_for_proc(p, 4, 0, &kp, &fd, &l);
    s64 b_va = -1;
    bool b_closed = false, b_kept = false;
    u64 b_ring_va = kp.ring_va;
    if (!b_rc) {
        (void)sys_burrow_detach_for_proc(p, kp.ring_va, kp.ring_size);
        b_va = sys_burrow_attach_for_proc(p, kp.ring_size);
        struct Burrow *ring = l->ring;
        sys_loom_setup_rollback(p, fd, l, kp.ring_va, kp.ring_size);
        struct Handle h;
        b_closed = handle_get(p, fd, &h) != 0;
        if (!b_closed) handle_put(&h);
        b_kept = setup_maps_region_at(p, kp.ring_va, ring);
        if (b_va > 0) (void)sys_burrow_detach_for_proc(p, (u64)b_va, kp.ring_size);
    }
    u64 b_destroyed = loom_total_destroyed() - destroyed0;
    u32 b_settled = p->as->page_count;

    // C: a setup refused for a slot after a sibling trimmed its ring.
    for (int i = 0; !pipe_rc && i < PROC_HANDLE_MAX; i++)
        if (handle_dup(p, rd, RIGHT_READ) < 0) break;
    setup_trim_va = 0;
    setup_trim_rc = -1;
    loom_setup_mapped_hook_for_test(setup_trim_ring);
    int c_rc = sys_loom_setup_for_proc(p, 64, 0, &kp, &fd);
    loom_setup_mapped_hook_for_test(NULL);
    u64 c_destroyed = loom_total_destroyed() - destroyed0;
    u64 rest = setup_trim_va + PAGE_SIZE, rest_end = 0;
    spin_lock(&p->as->lock);
    struct Vma *cv = setup_trim_va ? vma_lookup_in(p->as, rest) : NULL;
    if (cv && cv->vaddr_start == rest && cv->burrow) rest_end = cv->vaddr_end;
    spin_unlock(&p->as->lock);
    u32 c_charged = p->as->page_count;
    u32 c_want = rest_end ? base + (u32)burrow_backing_pages((size_t)(rest_end - setup_trim_va)) : 0;
    s64 c_detached = rest_end ? sys_burrow_detach_for_proc(p, rest, rest_end - rest) : -1;
    u32 c_settled = p->as->page_count;
    test_proc_drop(p);

#define BR(cond, msg) do { if (!error && !(cond)) error = msg; } while (0)
    BR(pipe_rc == 0 && a_rc == 0, "an ordinary ring to roll back");
    BR(a_dup == a_fd, "the sibling's file took the ring's number");
    BR(a_kept, "the rollback spares a file that reused the ring's number");
    BR(a_unmapped, "the rollback detaches the ring it still maps");
    BR(a_destroyed == 1 && a_settled == base, "the rollback frees the ring and settles its charge");
    BR(b_rc == 0 && b_va > 0 && (u64)b_va == b_ring_va, "the sibling's region took the ring's VA");
    BR(b_closed, "the rollback closes the ring's own descriptor");
    BR(b_kept, "the rollback spares a region mapped at the ring's VA");
    BR(b_destroyed == 2 && b_settled == base, "a ring detached by a sibling settles at its last drop");
    BR(c_rc == -T_E_MFILE && setup_trim_rc == 0, "a refused setup whose ring a sibling trimmed");
    BR(c_destroyed == 3, "the refused setup frees its Loom");
    BR(rest_end > rest, "the trimmed ring's remainder stays mapped");
    BR(c_charged == c_want, "a refused setup's ring stays charged while a mapping of it survives");
    BR(c_detached == 0 && c_settled == base, "the surviving mapping's last drop settles the ring");
#undef BR
    return error;
}

// The same rollback for a private ring. The default image refuses the held
// setup as it refuses every private one.
static const char *loom_private_setup_identity_fixture_run(void) {
    struct loom_params kp = { 0 };
    hidx_t fd = -1;
    struct Loom *l = NULL;
#ifndef LOOM_PRIVATE_QUALIFICATION
    struct Proc *p = test_proc_make();
    if (!p) return "identity proc";
    u32 base = p->as->page_count;
    int rc = loom_private_setup_held_for_proc(p, 2, LOOM_SETUP_PRIVATE_SERVICE, &kp, &fd, &l);
    bool untouched = p->as->page_count == base && !p->as->private_rings && !l;
    test_proc_drop(p);
    if (rc != -T_E_OPNOTSUPP) return "default image: a held private setup refuses EOPNOTSUPP";
    if (!untouched) return "default image: a refused held private setup charges nothing";
    return NULL;
#else
    const char *error = NULL;
    struct Proc *p = test_proc_make();
    if (!p) return "identity proc";
    proc_test_link(p);
    hidx_t rd = -1, wr = -1;
    int pipe_rc = sys_pipe_for_proc(p, &rd, &wr);
    u32 base = p->as->page_count;
    u64 goal = loom_private_retired();
    int rc = pipe_rc ? -1 : loom_private_setup_held_for_proc(p, 2, LOOM_SETUP_PRIVATE_SERVICE,
                                                             &kp, &fd, &l);
    hidx_t dup = -1;
    s64 va = -1;
    bool kept_fd = false, kept_va = false;
    if (!rc) {
        ++goal;
        (void)handle_close(p, fd);
        dup = handle_dup(p, rd, RIGHT_READ);
        (void)sys_burrow_detach_for_proc(p, kp.ring_va, kp.ring_size);
        va = sys_burrow_attach_for_proc(p, kp.ring_size);
        struct Burrow *ring = l->ring;
        sys_loom_setup_rollback(p, fd, l, kp.ring_va, kp.ring_size);
        struct Handle h;
        if (handle_get(p, fd, &h) == 0) { kept_fd = h.kind == KOBJ_SPOOR; handle_put(&h); }
        kept_va = setup_maps_region_at(p, kp.ring_va, ring);
        if (va > 0) (void)sys_burrow_detach_for_proc(p, (u64)va, kp.ring_size);
    }
    bool retired = lp_wait(goal);
    u32 settled = p->as->page_count;
    u32 rings = p->as->private_rings;
    proc_test_unlink(p);
    test_proc_drop(p);
#define BR(cond, msg) do { if (!error && !(cond)) error = msg; } while (0)
    BR(pipe_rc == 0 && rc == 0, "a private ring to roll back");
    BR(dup == fd && va > 0 && (u64)va == kp.ring_va,
       "the sibling's file and region took the private ring's number and VA");
    BR(kept_fd, "the private rollback spares a file that reused the ring's number");
    BR(kept_va, "the private rollback spares a region mapped at the ring's VA");
    BR(retired && settled == base && !rings, "the rolled-back private ring retires and settles");
#undef BR
    return error;
#endif
}
