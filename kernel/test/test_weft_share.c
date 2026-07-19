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
#include <thylacine/caps.h>
#include <thylacine/dma_handle.h>   // G-2: the weave mint + kobj live count
#include <thylacine/proc.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>
#include <thylacine/weft.h>

void test_weft_share_register_claim(void);
void test_weft_share_full(void);
void test_weft_share_owner_gc(void);
void test_weft_syscall_share(void);
void test_weft_map_binding_lifetime(void);
void test_weft_share_cap_gate(void);
void test_weft_share_rejects_plain_dma(void);
void test_weft_weave_share_and_claim(void);
void test_weft_unshare_disarm(void);
void test_weft_shared_map_budget_cap(void);

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
    netd->caps = CAP_HW_CREATE;   // Weft-7 F1: SYS_WEFT_SHARE is driver-tier gated.

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

    // ring_entries = 8: [hdr 64][ready 128][desc 8*16=128][payload 3776] fits one page.
    struct weft_binding *b = weft_binding_alloc(v, WEFT_TEST_VA, (u32)PAGE_SIZE, 8);
    TEST_ASSERT(b != NULL, "binding alloc + view");

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

// Weft-7 F1: SYS_WEFT_SHARE is gated to the NIC-owning driver tier
// (CAP_HW_CREATE). Ungated, any EL0 Proc could squat the fixed WEFT_MAX_SHARES
// registry (its minted share_id is never claimable -- only netd's Rweft hands
// the kernel an id to claim) and starve netd's weft_ensure -> every flow
// system-wide degrades to byte-copy (a cross-Proc availability DoS). A Proc
// WITHOUT the cap is refused before it touches a registry slot; the cap lets it
// through. Fails on pre-fix code (the cap-less share returned a positive id).
void test_weft_share_cap_gate(void) {
    struct Proc *p = make_proc();           // a fresh Proc has caps == 0
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    s64 va = sys_burrow_attach_for_proc(p, PAGE_SIZE);
    TEST_ASSERT(va > 0, "burrow_attach a ring");
    struct Vma *vma = vma_lookup(p, (u64)va);
    TEST_ASSERT(vma != NULL && vma->burrow != NULL, "ring VMA present");
    struct Burrow *v = vma->burrow;
    int h_before = burrow_handle_count(v);

    // No CAP_HW_CREATE: the share is refused (-1) and touches NO registry slot
    // (the pin count is the witness -- the gate fires before weft_share_register).
    TEST_ASSERT((p->caps & CAP_HW_CREATE) == 0, "test Proc starts cap-less");
    TEST_EXPECT_EQ(sys_weft_share_for_proc(p, (u64)va, PAGE_SIZE), -1,
        "SYS_WEFT_SHARE without CAP_HW_CREATE is refused");
    TEST_EXPECT_EQ(burrow_handle_count(v), h_before,
        "a cap-refused share takes no registration pin");

    // Grant the driver-tier cap: the share now passes the gate + mints an id.
    p->caps = CAP_HW_CREATE;
    s64 id = sys_weft_share_for_proc(p, (u64)va, PAGE_SIZE);
    TEST_ASSERT(id > 0, "SYS_WEFT_SHARE with CAP_HW_CREATE succeeds");
    TEST_EXPECT_EQ(burrow_handle_count(v), h_before + 1,
        "the granted share holds the registration pin");

    // Tidy up: claim the id (transfers the pin), drop it + the mapping -> free.
    struct Burrow *claimed = weft_share_claim((u64)id);
    TEST_EXPECT_EQ(claimed, v, "claim returns the ring");
    u64 destroyed_before = burrow_total_destroyed();
    burrow_unref(v);                 // the claimed pin
    vma_drain(p);                    // the ring mapping
    TEST_EXPECT_EQ(burrow_total_destroyed(), destroyed_before + 1,
        "ring frees once the pin + mapping drop");

    drop_proc(p);
}

// =============================================================================
// G-2: the DMA-weave share admission + the disarm + the shared-in budget
// (TAPESTRY.md §18.1 + §18.11 F3/F10 + §18.12 R2-F1/R2-F3/R2-F5).
// =============================================================================

// The R2-F1 structural gate: a PLAIN DMA region (the device-command class --
// virtqueue / descriptor table, minted by kobj_dma_create WITHOUT the weave
// bit) is as unshareable as MMIO. Both the register-side gate
// (sys_weft_share_for_proc) and the claim-side gate (burrow_share_into)
// refuse it. Fails on pre-fix code only if someone widens either gate to
// "any DMA".
void test_weft_share_rejects_plain_dma(void) {
    struct Proc *server = make_proc();
    struct Proc *client = make_proc();
    TEST_ASSERT(server != NULL && client != NULL, "proc_alloc failed");
    server->caps = CAP_HW_CREATE;

    struct KObj_DMA *k = kobj_dma_create(PAGE_SIZE);      // plain: weave == false
    TEST_ASSERT(k != NULL, "kobj_dma_create");
    TEST_ASSERT(!k->weave, "plain DMA carries no weave bit");
    struct Burrow *v = burrow_create_dma(k);              // {h:1, m:0}; kobj ref +1
    TEST_ASSERT(v != NULL, "burrow_create_dma");
    kobj_dma_unref(k);   // drop the construction ref: the burrow's ref is now
                         // the sole holder (the real flow parks this ref in the
                         // handle table; the test has none)

    // Map it into the server (the SYS_DMA_MAP shape), construction ref kept as
    // the test's own handle.
    spin_lock(&server->vma_lock);
    TEST_EXPECT_EQ(burrow_map(server, v, WEFT_TEST_VA, PAGE_SIZE, VMA_PROT_RW), 0,
        "server maps its plain DMA region");
    spin_unlock(&server->vma_lock);

    // Register-side: refused before any registry slot is touched.
    int h_before = burrow_handle_count(v);
    TEST_EXPECT_EQ(sys_weft_share_for_proc(server, WEFT_TEST_VA, PAGE_SIZE), -1,
        "SYS_WEFT_SHARE refuses a plain (non-weave) DMA region");
    TEST_EXPECT_EQ(burrow_handle_count(v), h_before,
        "the refused share takes no registration pin");

    // Claim-side (defense-in-depth): burrow_share_into refuses it directly.
    spin_lock(&client->vma_lock);
    TEST_EXPECT_EQ(burrow_share_into(client, v, WEFT_TEST_VA, VMA_PROT_RW), -1,
        "burrow_share_into refuses a plain DMA Burrow");
    spin_unlock(&client->vma_lock);
    TEST_EXPECT_EQ(client->shared_map_pages, 0u,
        "a refused share charges nothing");

    // weft_claimed_kind: inadmissible regardless of the declared geometry.
    TEST_EXPECT_EQ(weft_claimed_kind(v, 0), -1, "plain DMA never claims as weave");
    TEST_EXPECT_EQ(weft_claimed_kind(v, 8), -1, "plain DMA never claims as ring");

    // Teardown: unmap + drop the construction handle -> burrow free -> kobj free.
    u64 live_before = kobj_dma_live_count();
    vma_drain(server);
    burrow_unref(v);
    TEST_EXPECT_EQ(kobj_dma_live_count(), live_before - 1,
        "kobj chunk freed with its burrow");
    drop_proc(server);
    drop_proc(client);
}

// The full weave arc: mint (kernel-set immutable bit) -> register (the gate
// ADMITS the weave) -> claim-kind decision (type-derived + declaration
// cross-check) -> share_into (budget charged) -> the weave binding (no ring
// view; every Tweftio consumer closed by the validate_rw kind gate) ->
// teardown (uncharge + the #847 dual count across the KObj_DMA second domain).
void test_weft_weave_share_and_claim(void) {
    struct Proc *server = make_proc();    // tapestryd's role
    struct Proc *client = make_proc();    // the surface client's role
    TEST_ASSERT(server != NULL && client != NULL, "proc_alloc failed");
    server->caps = CAP_HW_CREATE;

    struct KObj_DMA *k = kobj_dma_create_weave(2u * PAGE_SIZE);
    TEST_ASSERT(k != NULL, "kobj_dma_create_weave");
    TEST_ASSERT(k->weave, "the weave bit is kernel-minted at create");
    struct Burrow *v = burrow_create_dma(k);              // {h:1, m:0}
    TEST_ASSERT(v != NULL, "burrow_create_dma over the weave kobj");
    kobj_dma_unref(k);   // drop the construction ref (handle-table stand-in);
                         // the burrow's kobj ref is now the sole holder

    spin_lock(&server->vma_lock);
    TEST_EXPECT_EQ(burrow_map(server, v, WEFT_TEST_VA, 2u * PAGE_SIZE, VMA_PROT_RW), 0,
        "server maps its weave");
    spin_unlock(&server->vma_lock);
    burrow_unref(v);                       // the SYS_DMA_MAP shape: {h:0, m:1}

    // Register: the share gate ADMITS the kernel-minted weave subtype.
    s64 id = sys_weft_share_for_proc(server, WEFT_TEST_VA, 2u * PAGE_SIZE);
    TEST_ASSERT(id > 0, "SYS_WEFT_SHARE admits the weave");
    TEST_EXPECT_EQ(burrow_handle_count(v), 1, "the registration pin held");

    // Claim + the kind decision: the kernel-minted type is the authority; the
    // server's declaration must AGREE (weave => ring_entries == 0).
    struct Burrow *claimed = weft_share_claim((u64)id);
    TEST_EXPECT_EQ(claimed, v, "claim returns the weave");
    TEST_EXPECT_EQ(weft_claimed_kind(v, 0), (int)WEFT_BIND_WEAVE,
        "weave + entries==0 -> the weave kind");
    TEST_EXPECT_EQ(weft_claimed_kind(v, 8), -1,
        "weave + a declared ring geometry -> mismatch, fail closed");

    // Share into the client: the budget charges npages (2) exactly.
    TEST_EXPECT_EQ(client->shared_map_pages, 0u, "client starts uncharged");
    spin_lock(&client->vma_lock);
    TEST_EXPECT_EQ(burrow_share_into(client, v, WEFT_TEST_VA, VMA_PROT_RW), 0,
        "share the weave into the client");
    spin_unlock(&client->vma_lock);
    TEST_EXPECT_EQ(client->shared_map_pages, 2u, "the shared-in budget charged");
    TEST_EXPECT_EQ(burrow_mapping_count(v), 2, "server + client mappings");
    struct Vma *cvma = vma_lookup(client, WEFT_TEST_VA);
    TEST_ASSERT(cvma != NULL && (cvma->flags & VMA_FLAG_SHARED_IN),
        "the client VMA carries the SHARED_IN flag");

    // The weave binding: no ring view; the validate_rw kind gate closes every
    // Tweftio fast path at one chokepoint. The RING allocator refuses the
    // weave outright (pages == NULL + non-ANON).
    struct weft_binding *b =
        weft_binding_alloc_weave(v, WEFT_TEST_VA, 2u * PAGE_SIZE, client->pid);
    TEST_ASSERT(b != NULL, "weave binding alloc");
    u32 off = 0;
    TEST_EXPECT_EQ(weft_binding_validate_rw(b, WEFT_TEST_VA + 64, 128, &off), -1,
        "a WEAVE binding never validates a Tweftio drive (the kind gate)");
    TEST_ASSERT(weft_binding_alloc(v, WEFT_TEST_VA, 2u * PAGE_SIZE, 8) == NULL,
        "the RING allocator refuses a weave Burrow");

    // Teardown: the binding drops the pin; the client unmap uncharges; the
    // server drain drops the last mapping -> burrow free -> the KObj_DMA
    // second-refcount domain frees the pixel chunk (R2-F1's fold).
    u64 destroyed_before = burrow_total_destroyed();
    u64 live_before = kobj_dma_live_count();
    weft_binding_release(b);                              // {h:0, m:2}
    spin_lock(&client->vma_lock);
    TEST_EXPECT_EQ(burrow_unmap(client, WEFT_TEST_VA, 2u * PAGE_SIZE), 0,
        "client unmaps the weave");
    spin_unlock(&client->vma_lock);
    TEST_EXPECT_EQ(client->shared_map_pages, 0u,
        "the unmap uncharges the shared-in budget");
    TEST_EXPECT_EQ(burrow_total_destroyed(), destroyed_before,
        "weave alive on the server mapping");
    vma_drain(server);                                    // {h:0, m:0} -> free
    TEST_EXPECT_EQ(burrow_total_destroyed(), destroyed_before + 1,
        "weave frees when the last ref drops");
    TEST_EXPECT_EQ(kobj_dma_live_count(), live_before - 1,
        "the KObj_DMA pixel chunk freed with its burrow (the second domain)");

    drop_proc(server);
    drop_proc(client);
}

// The F3/R2-F5 disarm: weft_share_unregister removes an UN-claimed entry
// (owner-gated) + drops the pin, so a late claim fails closed -- the kernel
// half of the retire-vs-claim NoStaleMap gate (registry-removal-before-free +
// the claim's live-registry lookup). Fails on pre-fix code (no unregister
// existed: a stale share stayed claimable until owner death).
void test_weft_unshare_disarm(void) {
    struct Proc *a = make_proc();
    struct Proc *b = make_proc();
    TEST_ASSERT(a != NULL && b != NULL, "proc_alloc failed");

    struct Burrow *v = burrow_create_anon(PAGE_SIZE);
    TEST_ASSERT(v != NULL, "burrow_create_anon");
    int h0 = burrow_handle_count(v);

    u64 id = weft_share_register(a, v);
    TEST_ASSERT(id != 0, "register");
    TEST_EXPECT_EQ(burrow_handle_count(v), h0 + 1, "pin held");

    // The owner gate: a stranger cannot disarm A's share; the entry survives.
    TEST_EXPECT_EQ(weft_share_unregister(b, id), -1,
        "a non-owner's unregister is refused");
    TEST_EXPECT_EQ(burrow_handle_count(v), h0 + 1, "the entry + pin survive");

    // The owner disarms: entry removed, pin dropped, the late claim fails
    // closed (the NoStaleMap realization).
    TEST_EXPECT_EQ(weft_share_unregister(a, id), 0, "the owner disarms");
    TEST_EXPECT_EQ(burrow_handle_count(v), h0, "the disarm drops the pin");
    TEST_ASSERT(weft_share_claim(id) == NULL,
        "a claim after the disarm fails closed");

    // Idempotence-adjacent: a second unregister of the same id is -1 (gone);
    // an unregister of a CLAIMED id is -1 (the client mapped legitimately --
    // retire proceeds by quiesce, not by yanking).
    TEST_EXPECT_EQ(weft_share_unregister(a, id), -1, "double-disarm is -1");
    u64 id2 = weft_share_register(a, v);
    TEST_ASSERT(id2 != 0, "re-register");
    struct Burrow *c = weft_share_claim(id2);
    TEST_EXPECT_EQ(c, v, "claim consumes");
    TEST_EXPECT_EQ(weft_share_unregister(a, id2), -1,
        "an unregister after the claim is -1 (already consumed)");
    burrow_unref(v);                       // the claimed pin
    TEST_EXPECT_EQ(burrow_handle_count(v), h0, "pin balance restored");

    burrow_unref(v);                       // the construction handle -> free
    drop_proc(a);
    drop_proc(b);
}

// The R2-F3 budget cap: a non-exempt client at the shared-in cap cannot take
// another share (fails clean: -1, no VMA, no ref, no charge); the SYSTEM TCB
// is exempt. Fails on pre-fix code (no budget existed: the share succeeded).
void test_weft_shared_map_budget_cap(void) {
    struct Proc *client = make_proc();     // principal 0 -> NON-exempt
    TEST_ASSERT(client != NULL, "proc_alloc failed");
    TEST_ASSERT(!proc_resource_exempt(client), "test Proc is non-exempt");

    struct Burrow *v = burrow_create_anon(PAGE_SIZE);
    TEST_ASSERT(v != NULL, "burrow_create_anon");
    int m0 = burrow_mapping_count(v);

    // Pre-load the counter to the cap: the next share must fail CLEAN.
    client->shared_map_pages = PROC_SHARED_MAP_MAX_PAGES;
    spin_lock(&client->vma_lock);
    TEST_EXPECT_EQ(burrow_share_into(client, v, WEFT_TEST_VA, VMA_PROT_RW), -1,
        "an over-budget share is refused");
    spin_unlock(&client->vma_lock);
    TEST_EXPECT_EQ(burrow_mapping_count(v), m0, "no mapping ref taken");
    TEST_ASSERT(vma_lookup(client, WEFT_TEST_VA) == NULL, "no VMA installed");
    TEST_EXPECT_EQ(client->shared_map_pages, PROC_SHARED_MAP_MAX_PAGES,
        "the refused share charged nothing");

    // Back under the cap: the share succeeds + charges; the drain uncharges.
    client->shared_map_pages = 0;
    spin_lock(&client->vma_lock);
    TEST_EXPECT_EQ(burrow_share_into(client, v, WEFT_TEST_VA, VMA_PROT_RW), 0,
        "an under-budget share succeeds");
    spin_unlock(&client->vma_lock);
    TEST_EXPECT_EQ(client->shared_map_pages, 1u, "charged one page");
    vma_drain(client);
    TEST_EXPECT_EQ(client->shared_map_pages, 0u, "the drain uncharges");

    burrow_unref(v);                       // construction handle -> free
    drop_proc(client);
}
