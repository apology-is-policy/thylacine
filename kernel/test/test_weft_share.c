// Weft-6a-2: the share_id registry + the SYS_WEFT_SHARE handler + the per-flow
// ring binding lifetime (kernel/weft.c + kernel/syscall.c; NET-THROUGHPUT.md
// section 6).
//
//   weft.share_register_claim
//     register -> a fresh share_id; claim consumes it exactly once (a replay
//     returns NULL); the claim TRANSFERS the registration pin (handle_count) to
//     the caller, who then frees the ring with the final unref.
//
//   weft.share_full
//     The registry is bounded: fill it, the next register returns 0 (the flow
//     stays byte-copy) without leaking a pin. release_owner GCs every entry.
//
//   weft.share_owner_gc
//     release_owner(A) drops the pins of A's un-claimed shares ONLY -- a second
//     owner B's shares survive (the netd-death backstop, weft.tla
//     ShareBoundedByFlow).
//
//   weft.syscall_share
//     sys_weft_share_for_proc over a real attached ring: the happy path mints a
//     share_id whose claim returns the VMA's Burrow; ring_va not a VMA start and
//     a size mismatch are each rejected (-1), touching no registry slot.
//
//   weft.map_binding_lifetime
//     The full claim->share_into->bind->release path: the weft_binding OWNS the
//     registration pin (handle_count), the guest's ring mapping is reclaimed by
//     vma_drain (mapping_count) -- the ring frees only when BOTH drop (the
//     cross-Proc #847 dual count, the Loom-ring teardown precedent).

#include "test.h"

#include <thylacine/burrow.h>
#include <thylacine/proc.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>
#include <thylacine/weft.h>

void test_weft_share_register_claim(void);
void test_weft_share_full(void);
void test_weft_share_owner_gc(void);
void test_weft_syscall_share(void);
void test_weft_map_binding_lifetime(void);

// Non-static syscall inner (kernel/syscall.c). ring_va (x0), ring_size (x1).
s64 sys_weft_share_for_proc(struct Proc *p, u64 ring_va, u64 ring_size_raw);
s64 sys_burrow_attach_for_proc(struct Proc *p, u64 length_raw);

#define WEFT_TEST_VA 0x12000000ull

static struct Proc *make_proc(void) {
    return proc_alloc();
}

static void drop_proc(struct Proc *p) {
    if (!p) return;
    p->state = 2;            // PROC_STATE_ZOMBIE
    proc_free(p);
}

void test_weft_share_register_claim(void) {
    struct Proc *netd = make_proc();
    TEST_ASSERT(netd != NULL, "proc_alloc failed");

    struct Burrow *v = burrow_create_anon(PAGE_SIZE);   // {h:1, m:0}
    TEST_ASSERT(v != NULL, "burrow_create_anon failed");

    int h_before = burrow_handle_count(v);

    u64 id = weft_share_register(netd, v);
    TEST_ASSERT(id != 0, "register should mint a non-zero share_id");
    TEST_EXPECT_EQ(burrow_handle_count(v), h_before + 1,
        "register takes the registration pin (handle_count + 1)");

    // A bad / never-minted id claims nothing.
    TEST_ASSERT(weft_share_claim(0) == NULL, "share_id 0 is never valid");
    TEST_ASSERT(weft_share_claim(id + 1) == NULL, "an un-minted id claims nothing");

    // The real id claims the burrow exactly once; the pin is TRANSFERRED (no
    // count change -- ownership moved from the registry to us).
    struct Burrow *claimed = weft_share_claim(id);
    TEST_EXPECT_EQ(claimed, v, "claim returns the registered Burrow");
    TEST_EXPECT_EQ(burrow_handle_count(v), h_before + 1,
        "claim transfers the pin -- handle_count unchanged");

    // Consume-once: a replay of a claimed id finds nothing.
    TEST_ASSERT(weft_share_claim(id) == NULL, "consume-once: replay claims nothing");

    // We own the transferred pin now; drop it + the construction handle -> free.
    u64 destroyed_before = burrow_total_destroyed();
    burrow_unref(v);   // drop the (claimed) registration pin: h_before+1 -> h_before
    TEST_EXPECT_EQ(burrow_total_destroyed(), destroyed_before,
        "construction handle still holds v");
    burrow_unref(v);   // drop the construction handle: -> {h:0, m:0} free
    TEST_EXPECT_EQ(burrow_total_destroyed(), destroyed_before + 1,
        "v frees when the last ref drops");

    drop_proc(netd);
}

void test_weft_share_full(void) {
    struct Proc *netd = make_proc();
    TEST_ASSERT(netd != NULL, "proc_alloc failed");

    // The registry holds WEFT_MAX_SHARES (64). Register that many + assert the
    // next one is refused (0) without leaking a pin. We register over ONE shared
    // Burrow (the pin count is the witness): each register takes a pin.
    struct Burrow *v = burrow_create_anon(PAGE_SIZE);
    TEST_ASSERT(v != NULL, "burrow_create_anon failed");
    int h0 = burrow_handle_count(v);

    int registered = 0;
    for (int i = 0; i < 80; i++) {
        u64 id = weft_share_register(netd, v);
        if (id == 0) break;
        registered++;
    }
    TEST_ASSERT(registered > 0, "at least one register succeeds");
    TEST_EXPECT_EQ(burrow_handle_count(v), h0 + registered,
        "each successful register took exactly one pin");

    // The next register over the (now full) table returns 0 + leaks no pin.
    int h_full = burrow_handle_count(v);
    u64 over = weft_share_register(netd, v);
    TEST_EXPECT_EQ(over, 0u, "a full registry refuses with 0");
    TEST_EXPECT_EQ(burrow_handle_count(v), h_full,
        "a refused register drops the pin it tentatively took");

    // release_owner GCs every one of netd's entries -> all pins dropped.
    weft_share_release_owner(netd);
    TEST_EXPECT_EQ(burrow_handle_count(v), h0,
        "release_owner drops every registered pin");

    burrow_unref(v);   // drop the construction handle
    drop_proc(netd);
}

void test_weft_share_owner_gc(void) {
    struct Proc *a = make_proc();
    struct Proc *b = make_proc();
    TEST_ASSERT(a != NULL && b != NULL, "proc_alloc failed");

    struct Burrow *va = burrow_create_anon(PAGE_SIZE);
    struct Burrow *vb = burrow_create_anon(PAGE_SIZE);
    TEST_ASSERT(va != NULL && vb != NULL, "burrow_create_anon failed");
    int ha = burrow_handle_count(va);
    int hb = burrow_handle_count(vb);

    TEST_ASSERT(weft_share_register(a, va) != 0, "register A");
    TEST_ASSERT(weft_share_register(b, vb) != 0, "register B");
    TEST_EXPECT_EQ(burrow_handle_count(va), ha + 1, "A's pin taken");
    TEST_EXPECT_EQ(burrow_handle_count(vb), hb + 1, "B's pin taken");

    // GC owner A -> A's pin dropped, B's untouched (the per-owner backstop).
    weft_share_release_owner(a);
    TEST_EXPECT_EQ(burrow_handle_count(va), ha, "A's pin GC'd");
    TEST_EXPECT_EQ(burrow_handle_count(vb), hb + 1, "B's pin survives A's GC");

    weft_share_release_owner(b);
    TEST_EXPECT_EQ(burrow_handle_count(vb), hb, "B's pin GC'd");

    burrow_unref(va);
    burrow_unref(vb);
    drop_proc(a);
    drop_proc(b);
}

void test_weft_syscall_share(void) {
    struct Proc *netd = make_proc();
    TEST_ASSERT(netd != NULL, "proc_alloc failed");

    // netd attaches a ring (the SYS_BURROW_ATTACH shape: {h:0, m:1} -- the VMA
    // alone holds it). sys_weft_share_for_proc resolves the VA -> that Burrow.
    s64 va = sys_burrow_attach_for_proc(netd, PAGE_SIZE);
    TEST_ASSERT(va > 0, "burrow_attach a ring");
    struct Vma *vma = vma_lookup(netd, (u64)va);
    TEST_ASSERT(vma != NULL && vma->burrow != NULL, "ring VMA present");
    struct Burrow *v = vma->burrow;
    int h_before = burrow_handle_count(v);

    // Rejects: a VA that is not a VMA start; a size that is not the whole ring.
    TEST_EXPECT_EQ(sys_weft_share_for_proc(netd, 0, PAGE_SIZE), -1,
        "ring_va not a VMA start rejected");
    TEST_EXPECT_EQ(sys_weft_share_for_proc(netd, (u64)va, 2u * PAGE_SIZE), -1,
        "ring_size != the Burrow size rejected (whole-ring contract)");
    TEST_EXPECT_EQ(burrow_handle_count(v), h_before,
        "rejected shares take no pin");

    // Happy path: a share_id whose claim returns this exact Burrow + holds a pin.
    s64 id = sys_weft_share_for_proc(netd, (u64)va, PAGE_SIZE);
    TEST_ASSERT(id > 0, "share mints a positive share_id");
    TEST_EXPECT_EQ(burrow_handle_count(v), h_before + 1,
        "the share holds the registration pin");
    struct Burrow *claimed = weft_share_claim((u64)id);
    TEST_EXPECT_EQ(claimed, v, "claim returns the ring's Burrow");

    // Drop the claimed pin; vma_drain drops the mapping -> the ring frees.
    u64 destroyed_before = burrow_total_destroyed();
    burrow_unref(v);                 // the claimed pin
    vma_drain(netd);                 // the ring VMA mapping
    TEST_EXPECT_EQ(burrow_total_destroyed(), destroyed_before + 1,
        "ring frees once the pin + the mapping both drop");

    drop_proc(netd);
}

void test_weft_map_binding_lifetime(void) {
    struct Proc *netd  = make_proc();   // owns the ring (a mapping in its own AS)
    struct Proc *guest = make_proc();   // receives the share (a mapping only)
    TEST_ASSERT(netd != NULL && guest != NULL, "proc_alloc failed");

    // Build the ring with the real refcount shape: netd maps it whole, then the
    // construction handle drops (the SYS_BURROW_ATTACH posture: {h:0, m:1}).
    struct Burrow *v = burrow_create_anon(PAGE_SIZE);   // {h:1, m:0}
    TEST_ASSERT(v != NULL, "burrow_create_anon failed");
    TEST_EXPECT_EQ(burrow_map(netd, v, WEFT_TEST_VA, PAGE_SIZE, VMA_PROT_RW), 0,
        "netd maps the ring");
    burrow_unref(v);                                     // drop construction -> {h:0, m:1}
    TEST_EXPECT_EQ(burrow_handle_count(v), 0, "no construction handle (attach shape)");
    TEST_EXPECT_EQ(burrow_mapping_count(v), 1, "netd's mapping holds the ring");

    // netd registers it (the registration pin) + the kernel claims it.
    u64 id = weft_share_register(netd, v);              // {h:1, m:1}
    TEST_ASSERT(id != 0, "register");
    struct Burrow *claimed = weft_share_claim(id);       // pin transferred to us
    TEST_EXPECT_EQ(claimed, v, "claim");
    TEST_EXPECT_EQ(burrow_handle_count(v), 1, "the registration pin (now ours)");

    // The kernel shares the ring into the guest + binds it (the binding OWNS the
    // pin). {h:1, m:2}.
    spin_lock(&guest->vma_lock);
    TEST_EXPECT_EQ(burrow_share_into(guest, v, WEFT_TEST_VA, VMA_PROT_RW), 0,
        "share the ring into the guest");
    spin_unlock(&guest->vma_lock);
    TEST_EXPECT_EQ(burrow_mapping_count(v), 2, "netd + guest mappings");

    struct weft_binding *b = weft_binding_alloc(v, WEFT_TEST_VA, (u32)PAGE_SIZE);
    TEST_ASSERT(b != NULL, "binding alloc");

    u64 destroyed_before = burrow_total_destroyed();

    // dev9p_close -> weft_binding_release drops the registration pin (handle_-
    // count), NOT the guest mapping. The ring stays alive on the two mappings.
    weft_binding_release(b);                             // {h:0, m:2}
    TEST_EXPECT_EQ(burrow_handle_count(v), 0, "binding release drops the pin");
    TEST_EXPECT_EQ(burrow_total_destroyed(), destroyed_before,
        "ring alive on the two mappings (the pin is gone)");

    // The guest's ring mapping is reclaimed by vma_drain at guest exit (the
    // Loom-ring precedent) -- NOT by the binding release.
    vma_drain(guest);                                    // {h:0, m:1}
    TEST_EXPECT_EQ(burrow_mapping_count(v), 1, "guest mapping dropped by vma_drain");
    TEST_EXPECT_EQ(burrow_total_destroyed(), destroyed_before,
        "ring alive on netd's mapping");

    vma_drain(netd);                                     // {h:0, m:0} -> free
    TEST_EXPECT_EQ(burrow_total_destroyed(), destroyed_before + 1,
        "ring frees when the LAST ref (a mapping) drops -- the cross-Proc #847 count");

    drop_proc(netd);
    drop_proc(guest);
}
