// Weft-3 -- the descriptor-ring substrate (kernel/weft.c). The split-ring
// snapshot-and-bounds-validate consumer driven over a burrow_share_into
// cross-Proc shared page (the Weft-2 substrate). Validates specs/weft.tla:
//   weft_ring_basic            -- Consume + the IN-PLACE payload read (no copy).
//   weft_ring_toctou_snapshot  -- DescPinnedToSnapshot: the kernel acts on the
//                                 value-copy, immune to a post-consume mutation.
//   weft_ring_bounds_reject    -- ActedDescValidated: out-of-bounds / malformed /
//                                 reserved-flag / u32-wrap descriptors rejected.
//   weft_ring_multi_split      -- multiple descriptors + the split-ring single-
//                                 writer discipline (the consumer is read-only on
//                                 the producer's prod_tail + descriptor region).
//   weft_should_ring_threshold -- the hybrid <1024 B byte-copy fallback.
//   weft_ring_layout_constraints -- the geometry validation (power-of-two,
//                                 fit, fail-closed).

#include "test.h"

#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>
#include <thylacine/burrow.h>
#include <thylacine/weft.h>

void test_weft_ring_basic(void);
void test_weft_ring_toctou_snapshot(void);
void test_weft_ring_bounds_reject(void);
void test_weft_ring_multi_split(void);
void test_weft_should_ring_threshold(void);
void test_weft_ring_layout_constraints(void);
void test_weft_ready_signal_observe(void);
void test_weft_ready_park_handshake(void);
void test_weft_ready_arm_park_sees_race(void);
void test_weft_notif_terminal_release(void);
void test_weft_notif_premature_blocked(void);
void test_weft_notif_copied_immediate(void);

#define WEFT_TEST_VA   0x20000000ull          // 512 MiB; well inside user-VA
#define WEFT_RING_ENTRIES 8u
// A 4-page ring Burrow: ring_hdr (64) + ready_hdr (128) + desc[8] (128) + a
// payload region spanning multiple pages (exercises the BURROW_TYPE_ANON
// contiguity the share rests on).
#define WEFT_RING_PAGES (4ull * PAGE_SIZE)
#define WEFT_DESC_OFF   192                    // ring_hdr(64) + ready_hdr(128)
#define WEFT_PAYLOAD_OFF (192 + 8 * 16)        // + desc[8] = 320
#define WEFT_READY_OFF  64                     // ring_hdr(64) -> ready_hdr

static struct Proc *weft_make_proc(void) { return proc_alloc(); }

static void weft_drop_proc(struct Proc *p) {
    if (!p) return;
    p->state = 2;                              // PROC_STATE_ZOMBIE
    proc_free(p);
}

// Post a descriptor at the producer's current tail slot and release-bump
// prod_tail -- the guest's single-writer producer step (no payload write, so an
// out-of-bounds descriptor can be posted without overrunning the page).
static void weft_post(struct weft_ring_view *rv, u32 *tail,
                      u32 addr, u32 len, u32 flags) {
    struct weft_ring_hdr *h = (struct weft_ring_hdr *)rv->base;
    struct weft_desc *darr  = (struct weft_desc *)(rv->base + rv->desc_off);
    u32 pos = *tail & (rv->ring_entries - 1u);
    darr[pos].addr  = addr;
    darr[pos].len   = len;
    darr[pos].flags = flags;
    darr[pos]._resv = 0u;
    *tail += 1u;
    __atomic_store_n(&h->prod_tail, *tail, __ATOMIC_RELEASE);
}

// Fill / verify the payload region with a deterministic per-byte pattern (the
// in-place payload the consumer reads at the validated offset).
static void weft_fill(struct weft_ring_view *rv, u32 addr, u32 len) {
    u8 *payload = rv->base + rv->payload_off;
    for (u32 i = 0; i < len; i++) payload[addr + i] = (u8)((addr + i) * 7u + 1u);
}
static bool weft_check(struct weft_ring_view *rv, u32 addr, u32 len) {
    const u8 *payload = rv->base + rv->payload_off;
    for (u32 i = 0; i < len; i++)
        if (payload[addr + i] != (u8)((addr + i) * 7u + 1u)) return false;
    return true;
}

static void weft_teardown(struct Proc *netd, struct Proc *guest, struct Burrow *v) {
    vma_drain(guest);
    vma_drain(netd);
    burrow_unref(v);
    weft_drop_proc(netd);
    weft_drop_proc(guest);
}

// Stand up a per-flow ring Burrow shared into two Procs (netd owns the handle +
// its own map; guest receives the share), lay the ring out, write the geometry
// mirror. Inlined as a macro so a setup-assertion failure aborts the calling
// test function cleanly (TEST_ASSERT expands to `return;`).
#define WEFT_SETUP(netd, guest, v, rv)                                              \
    struct Proc *netd = weft_make_proc(), *guest = weft_make_proc();                \
    TEST_ASSERT(netd != NULL && guest != NULL, "proc_alloc failed");                \
    struct Burrow *v = burrow_create_anon(WEFT_RING_PAGES);                         \
    TEST_ASSERT(v != NULL, "burrow_create_anon failed");                            \
    TEST_EXPECT_EQ(burrow_map(netd, v, WEFT_TEST_VA, WEFT_RING_PAGES, VMA_PROT_RW), \
                   0, "netd burrow_map");                                           \
    TEST_EXPECT_EQ(burrow_share_into(guest, v, WEFT_TEST_VA, VMA_PROT_RW),          \
                   0, "guest burrow_share_into");                                   \
    struct weft_ring_view rv;                                                       \
    TEST_EXPECT_EQ(weft_ring_layout((u8 *)pa_to_kva(page_to_pa(v->pages)),          \
                                    WEFT_RING_PAGES, WEFT_RING_ENTRIES, &rv),       \
                   0, "weft_ring_layout");                                          \
    weft_ring_init_hdr(&rv)

// ---------------------------------------------------------------------------
// weft.ring_basic -- a large payload rides the ring; the kernel snapshots the
// descriptor (Consume) and reads the payload IN PLACE at the validated offset.
// ---------------------------------------------------------------------------
void test_weft_ring_basic(void) {
    WEFT_SETUP(netd, guest, v, rv);

    // The geometry mirror the guest reads from the shared header.
    struct weft_ring_hdr *h = (struct weft_ring_hdr *)rv.base;
    TEST_EXPECT_EQ((int)h->magic,        (int)WEFT_MAGIC,    "hdr magic mirror");
    TEST_EXPECT_EQ((int)h->ring_entries, WEFT_RING_ENTRIES,  "hdr ring_entries mirror");
    TEST_EXPECT_EQ((int)h->desc_off,     WEFT_DESC_OFF,      "desc_off = sizeof(hdr)");
    TEST_EXPECT_EQ((int)h->payload_off,  WEFT_PAYLOAD_OFF,   "payload_off after desc[8]");
    TEST_EXPECT_EQ((int)rv.payload_size, (int)(WEFT_RING_PAGES - WEFT_PAYLOAD_OFF),
        "payload_size = burrow - geometry");

    // Guest: write a >= threshold payload, post one descriptor.
    u32 tail = 0;
    const u32 ADDR = 32, LEN = 2000;           // LEN >= WEFT_HYBRID_THRESHOLD
    TEST_ASSERT(weft_should_ring(LEN), "2000 B rides the ring");
    weft_fill(&rv, ADDR, LEN);
    weft_post(&rv, &tail, ADDR, LEN, 0);

    // Kernel: drain the descriptor -> the validated snapshot.
    struct weft_desc out[WEFT_RING_ENTRIES];
    int n = weft_ring_drain(&rv, out, WEFT_RING_ENTRIES);
    TEST_EXPECT_EQ(n, 1,                     "one descriptor drained");
    TEST_EXPECT_EQ((int)out[0].addr, ADDR,   "snapshot addr");
    TEST_EXPECT_EQ((int)out[0].len,  LEN,    "snapshot len");
    TEST_EXPECT_EQ((int)rv.cons_head, 1,     "cons_head advanced");
    TEST_EXPECT_EQ((int)h->cons_head, 1,     "cons_head mirror published");

    // The consumer reads the payload IN PLACE at the validated offset -- no copy
    // out of the ring. (rv.base is the shared page both Procs' VMAs back.)
    TEST_ASSERT(weft_check(&rv, out[0].addr, out[0].len),
        "in-place payload read matches the producer's write");

    weft_teardown(netd, guest, v);
}

// ---------------------------------------------------------------------------
// weft.ring_toctou_snapshot -- the kernel acts on the value-copy taken at
// consume; a guest mutation of the shared slot AFTER consume cannot redirect
// the op (weft.tla DescPinnedToSnapshot).
// ---------------------------------------------------------------------------
void test_weft_ring_toctou_snapshot(void) {
    WEFT_SETUP(netd, guest, v, rv);

    u32 tail = 0;
    const u32 ADDR = 0, LEN = 1500;
    weft_fill(&rv, ADDR, LEN);
    weft_post(&rv, &tail, ADDR, LEN, 0);

    struct weft_desc out[WEFT_RING_ENTRIES];
    int n = weft_ring_drain(&rv, out, WEFT_RING_ENTRIES);
    TEST_EXPECT_EQ(n, 1, "drained the valid descriptor");
    TEST_EXPECT_EQ((int)out[0].addr, ADDR, "snapshot addr before mutation");
    TEST_EXPECT_EQ((int)out[0].len,  LEN,  "snapshot len before mutation");

    // The adversary mutates the SHARED descriptor slot after consume (a guest
    // thread racing the kernel's in-place access). The kernel holds a COPY.
    struct weft_desc *darr = (struct weft_desc *)(rv.base + rv.desc_off);
    darr[0].addr = 0xDEADBEEFu;                // an out-of-bounds value
    darr[0].len  = 0xFFFFFFFFu;

    // The snapshot is unaffected: the kernel acts on out[0] (the in-bounds
    // value), never re-reading the now-poisoned shared slot.
    TEST_EXPECT_EQ((int)out[0].addr, ADDR, "snapshot UNCHANGED by the live mutation");
    TEST_EXPECT_EQ((int)out[0].len,  LEN,  "snapshot len UNCHANGED");
    TEST_ASSERT(darr[0].addr == 0xDEADBEEFu,
        "the live shared slot DID change (the snapshot diverged from it)");
    // The in-place read uses the snapshot offset -> the valid payload, not OOB.
    TEST_ASSERT(weft_check(&rv, out[0].addr, out[0].len),
        "in-place read follows the snapshot, not the poisoned slot");

    weft_teardown(netd, guest, v);
}

// ---------------------------------------------------------------------------
// weft.ring_bounds_reject -- only a validated descriptor reaches out[];
// out-of-bounds / zero-length / reserved-flag / u32-wrap descriptors are
// rejected at the gate (weft.tla ActedDescValidated). One valid + four bad.
// ---------------------------------------------------------------------------
void test_weft_ring_bounds_reject(void) {
    WEFT_SETUP(netd, guest, v, rv);
    struct weft_ring_hdr *h = (struct weft_ring_hdr *)rv.base;
    u32 psz = rv.payload_size;

    u32 tail = 0;
    weft_fill(&rv, 0, 100);
    weft_post(&rv, &tail, 0,          100, 0);   // [0] VALID
    weft_post(&rv, &tail, psz - 50,   100, 0);   // [1] OOB: addr+len > payload_size
    weft_post(&rv, &tail, 0,          0,   0);   // [2] malformed: len 0
    weft_post(&rv, &tail, 0,          100, 1);   // [3] reserved flag set
    weft_post(&rv, &tail, 0xFFFFFFFFu, 2,  0);   // [4] u32-wrap OOB (caught in u64)

    struct weft_desc out[WEFT_RING_ENTRIES];
    int n = weft_ring_drain(&rv, out, WEFT_RING_ENTRIES);
    TEST_EXPECT_EQ(n, 1,                    "only the valid descriptor emitted");
    TEST_EXPECT_EQ((int)out[0].addr, 0,     "the valid one is [0]");
    TEST_EXPECT_EQ((int)out[0].len,  100,   "the valid one's len");
    TEST_EXPECT_EQ((int)h->dropped, 4,      "four descriptors rejected (dropped counter)");
    TEST_EXPECT_EQ((int)rv.cons_head, 5,    "all five slots consumed");
    TEST_ASSERT(weft_check(&rv, 0, 100),    "the valid payload reads in place");

    weft_teardown(netd, guest, v);
}

// ---------------------------------------------------------------------------
// weft.binding_validate_write -- the data-drive write validator (Weft-6b-2): a
// SYS_WRITE buffer pointing INTO the flow's ring payload region resolves to the
// payload-relative offset; a buffer below the region or a window past
// payload_size is rejected (the byte-copy fall-through). The geometry is read
// from the kernel-private view, never the shared header.
// ---------------------------------------------------------------------------
void test_weft_binding_validate_write(void) {
    WEFT_SETUP(netd, guest, v, rv);

    // The binding the kernel records at SYS_WEFT_MAP: the guest mapped the ring
    // at WEFT_TEST_VA; the view carries the trusted geometry (burrow not needed
    // by the validator -- it reads only guest_va + view).
    struct weft_binding b = {
        .burrow = NULL, .guest_va = WEFT_TEST_VA,
        .ring_size = WEFT_RING_PAGES, .view = rv,
    };
    u64 payload_base = WEFT_TEST_VA + rv.payload_off;
    u32 off = 0xABCDu;

    // In the payload region, in bounds -> the payload-relative offset.
    TEST_EXPECT_EQ(weft_binding_validate_write(&b, payload_base + 64, 2000, &off), 0,
        "in-ring write validated");
    TEST_EXPECT_EQ((int)off, 64, "offset is payload-relative");
    off = 0xABCDu;
    TEST_EXPECT_EQ(weft_binding_validate_write(&b, payload_base, 1500, &off), 0, "base ok");
    TEST_EXPECT_EQ((int)off, 0, "base offset 0");

    // Below the payload region (the descriptor/control area) -> rejected.
    TEST_EXPECT_EQ(weft_binding_validate_write(&b, WEFT_TEST_VA, 2000, &off), -1,
        "below-payload buffer rejected (byte-copy fall-through)");
    TEST_EXPECT_EQ(weft_binding_validate_write(&b, payload_base - 1, 2000, &off), -1,
        "one byte below payload rejected");

    // Window past payload_size -> rejected (an in-place read would overrun).
    TEST_EXPECT_EQ(weft_binding_validate_write(&b, payload_base, rv.payload_size + 1, &off), -1,
        "len past payload_size rejected");
    TEST_EXPECT_EQ(weft_binding_validate_write(&b, payload_base + rv.payload_size - 10, 11, &off),
        -1, "window straddling the payload end rejected");
    TEST_EXPECT_EQ(weft_binding_validate_write(&b, payload_base + rv.payload_size - 1, 1, &off), 0,
        "last byte ok");

    // Zero-length + NULL guards.
    TEST_EXPECT_EQ(weft_binding_validate_write(&b, payload_base, 0, &off), -1, "zero-len rejected");
    TEST_EXPECT_EQ(weft_binding_validate_write(NULL, payload_base, 100, &off), -1, "NULL binding");
    TEST_EXPECT_EQ(weft_binding_validate_write(&b, payload_base, 100, NULL), -1, "NULL out_off");

    weft_teardown(netd, guest, v);
}

// ---------------------------------------------------------------------------
// weft.ring_multi_split -- multiple descriptors drained in order; the consumer
// is READ-ONLY on the producer's regions (prod_tail + the descriptor slots),
// the single-writer split-ring discipline (weft.tla leg (5)).
// ---------------------------------------------------------------------------
void test_weft_ring_multi_split(void) {
    WEFT_SETUP(netd, guest, v, rv);
    struct weft_ring_hdr *h = (struct weft_ring_hdr *)rv.base;
    struct weft_desc *darr  = (struct weft_desc *)(rv.base + rv.desc_off);

    u32 tail = 0;
    weft_fill(&rv, 0,    500);  weft_post(&rv, &tail, 0,    500, 0);
    weft_fill(&rv, 600,  400);  weft_post(&rv, &tail, 600,  400, 0);
    weft_fill(&rv, 2000, 1000); weft_post(&rv, &tail, 2000, 1000, 0);

    // Capture the producer-owned region before the drain.
    u32 prod_tail_before = h->prod_tail;
    struct weft_desc desc_before[3] = { darr[0], darr[1], darr[2] };

    struct weft_desc out[WEFT_RING_ENTRIES];
    int n = weft_ring_drain(&rv, out, WEFT_RING_ENTRIES);
    TEST_EXPECT_EQ(n, 3, "three descriptors drained in order");
    TEST_EXPECT_EQ((int)out[0].addr, 0,    "desc 0 addr");
    TEST_EXPECT_EQ((int)out[1].addr, 600,  "desc 1 addr");
    TEST_EXPECT_EQ((int)out[2].addr, 2000, "desc 2 addr");
    TEST_EXPECT_EQ((int)out[2].len,  1000, "desc 2 len");
    TEST_EXPECT_EQ((int)rv.cons_head, 3,   "cons_head advanced to 3");

    // The consumer wrote ONLY cons_head: prod_tail + the descriptor slots are
    // byte-unchanged (single-writer-per-region).
    TEST_EXPECT_EQ((int)h->prod_tail, (int)prod_tail_before, "consumer did not touch prod_tail");
    for (int i = 0; i < 3; i++) {
        TEST_EXPECT_EQ((int)darr[i].addr, (int)desc_before[i].addr, "desc slot addr unchanged");
        TEST_EXPECT_EQ((int)darr[i].len,  (int)desc_before[i].len,  "desc slot len unchanged");
    }
    for (int i = 0; i < 3; i++)
        TEST_ASSERT(weft_check(&rv, out[i].addr, out[i].len), "each payload reads in place");

    weft_teardown(netd, guest, v);
}

// ---------------------------------------------------------------------------
// weft.should_ring_threshold -- the hybrid <1024 B byte-copy fallback; the
// shared page is for large payloads only (NET-THROUGHPUT.md section 4.8).
// ---------------------------------------------------------------------------
void test_weft_should_ring_threshold(void) {
    TEST_ASSERT(!weft_should_ring(0),    "0 B stays on the byte-copy ring");
    TEST_ASSERT(!weft_should_ring(1),    "1 B stays on the byte-copy ring");
    TEST_ASSERT(!weft_should_ring(1023), "1023 B stays on the byte-copy ring");
    TEST_ASSERT(weft_should_ring(1024),  "1024 B rides the shared page");
    TEST_ASSERT(weft_should_ring(65536), "64 KiB rides the shared page");
}

// ---------------------------------------------------------------------------
// weft.ring_layout_constraints -- the geometry validation: power-of-two
// entries, the regions must fit, NULL + degenerate inputs fail closed.
// ---------------------------------------------------------------------------
void test_weft_ring_layout_constraints(void) {
    struct Burrow *v = burrow_create_anon(WEFT_RING_PAGES);
    TEST_ASSERT(v != NULL, "burrow_create_anon failed");
    u8 *base = (u8 *)pa_to_kva(page_to_pa(v->pages));
    struct weft_ring_view rv;

    TEST_EXPECT_EQ(weft_ring_layout(NULL, WEFT_RING_PAGES, 8, &rv), -1, "NULL base rejected");
    TEST_EXPECT_EQ(weft_ring_layout(base, WEFT_RING_PAGES, 8, NULL), -1, "NULL view rejected");
    TEST_EXPECT_EQ(weft_ring_layout(base, WEFT_RING_PAGES, 0, &rv), -1, "0 entries rejected");
    TEST_EXPECT_EQ(weft_ring_layout(base, WEFT_RING_PAGES, 3, &rv), -1, "non-power-of-two rejected");
    TEST_EXPECT_EQ(weft_ring_layout(base, WEFT_RING_PAGES, WEFT_MAX_ENTRIES * 2u, &rv), -1,
        "entries over the cap rejected");
    // A Burrow with room for only the header + descriptors, no payload region.
    TEST_EXPECT_EQ(weft_ring_layout(base, WEFT_PAYLOAD_OFF, 8, &rv), -1,
        "no payload room rejected");

    // A valid layout yields a sane, contiguous geometry.
    TEST_EXPECT_EQ(weft_ring_layout(base, WEFT_RING_PAGES, 8, &rv), 0, "valid layout");
    TEST_EXPECT_EQ((int)rv.ring_entries, 8,              "ring_entries");
    TEST_EXPECT_EQ((int)rv.desc_off, WEFT_DESC_OFF,      "desc_off");
    TEST_EXPECT_EQ((int)rv.payload_off, WEFT_PAYLOAD_OFF, "payload_off");
    TEST_EXPECT_EQ((int)rv.payload_size, (int)(WEFT_RING_PAGES - WEFT_PAYLOAD_OFF), "payload_size");
    TEST_EXPECT_EQ((int)rv.cons_head, 0,                 "cons_head starts at 0");

    burrow_unref(v);
}

// ---------------------------------------------------------------------------
// Readiness ring (Weft-4) -- the netd<->guest single-cache-line poke. These
// tests drive the producer (weft_ready_signal) + consumer (weft_ready_observe /
// arm_park) decision primitives over the shared page. The store-buffer no-lost-
// wake ordering itself is the spec's (weft_readiness.tla) + the seq-cst code's
// proof; a single-threaded test cannot reorder hardware, so it instead asserts
// the primitives' contracts -- the poke's cross-page visibility, the producer's
// wake decision (armed consumer -> wake), and the consumer's observe re-check
// (an edge already posted -> don't park).
// ---------------------------------------------------------------------------

// weft.ready_signal_observe -- the producer bumps the shared edge counter +
// publishes the mask; the consumer observes the new seq + mask over the same
// shared page (the Shenango cache-line read). A not-armed consumer is no wake.
void test_weft_ready_signal_observe(void) {
    WEFT_SETUP(netd, guest, v, rv);
    weft_ready_init_hdr(&rv);

    struct weft_ring_hdr *h = (struct weft_ring_hdr *)rv.base;
    TEST_EXPECT_EQ((int)h->ready_off, WEFT_READY_OFF, "ready_off mirror = sizeof(ring_hdr)");

    // Initial state: no edge posted.
    u32 mask = 0xFFu;
    TEST_EXPECT_EQ((int)weft_ready_observe(&rv, &mask), 0, "initial ready_seq = 0");
    TEST_EXPECT_EQ((int)mask, 0,                           "initial mask = 0");

    // Producer posts an RX edge; the consumer is not parked, so no wake needed.
    TEST_ASSERT(!weft_ready_signal(&rv, WEFT_READY_RX),
        "signal with no parked consumer -> no wake");
    TEST_EXPECT_EQ((int)weft_ready_observe(&rv, &mask), 1, "ready_seq advanced to 1");
    TEST_EXPECT_EQ((int)mask, (int)WEFT_READY_RX,          "mask = RX over the shared page");

    // A second edge (TX) advances the counter + republishes the mask.
    TEST_ASSERT(!weft_ready_signal(&rv, WEFT_READY_TX), "second signal -> no wake (not parked)");
    TEST_EXPECT_EQ((int)weft_ready_observe(&rv, &mask), 2, "ready_seq advanced to 2");
    TEST_EXPECT_EQ((int)mask, (int)WEFT_READY_TX,          "mask = TX");

    weft_teardown(netd, guest, v);
}

// weft.ready_park_handshake -- the consumer (caught up) arms a park; a producer
// edge then finds it armed and reports a wake is needed (the no-lost-wake: the
// poke wakes the parked consumer). The consumer un-parks + observes the edge.
void test_weft_ready_park_handshake(void) {
    WEFT_SETUP(netd, guest, v, rv);
    weft_ready_init_hdr(&rv);
    struct weft_ready_hdr *rh = (struct weft_ready_hdr *)(rv.base + rv.ready_off);

    // Consumer caught up (last_seen = current seq = 0); arming finds no edge in
    // the window -> safe to park, and the park-intent is published.
    u32 last_seen = weft_ready_observe(&rv, NULL);
    TEST_EXPECT_EQ((int)last_seen, 0, "consumer caught up at seq 0");
    TEST_ASSERT(weft_ready_arm_park(&rv, last_seen), "arm with no edge -> safe to park");
    TEST_EXPECT_EQ((int)rh->wait_active, 1, "park-intent published");
    TEST_EXPECT_EQ((int)rh->wait_seq, 0,    "registered at last_seen");

    // Producer posts an edge: the consumer is armed at a seq this edge passed,
    // so a wake IS needed (the parked consumer would be woken -- no lost wake).
    TEST_ASSERT(weft_ready_signal(&rv, WEFT_READY_RX),
        "signal with a parked consumer -> wake needed");

    // The consumer (woken) clears its park-intent and observes the new edge.
    weft_ready_unpark(&rv);
    TEST_EXPECT_EQ((int)rh->wait_active, 0, "park-intent cleared on resume");
    u32 mask = 0;
    TEST_EXPECT_EQ((int)weft_ready_observe(&rv, &mask), 1, "observes the posted edge");
    TEST_EXPECT_EQ((int)mask, (int)WEFT_READY_RX,          "observes the edge mask");

    weft_teardown(netd, guest, v);
}

// weft.ready_arm_park_sees_race -- the register-then-observe re-check: an edge
// posted BEFORE the consumer arms is caught by the post-register re-read, so the
// consumer does NOT park (it re-processes), and the park-intent is left clear.
void test_weft_ready_arm_park_sees_race(void) {
    WEFT_SETUP(netd, guest, v, rv);
    weft_ready_init_hdr(&rv);
    struct weft_ready_hdr *rh = (struct weft_ready_hdr *)(rv.base + rv.ready_off);

    // An edge arrives (ready_seq -> 1) while the consumer still thinks last_seen
    // = 0 (it has not observed it yet -- the in-window race).
    TEST_ASSERT(!weft_ready_signal(&rv, WEFT_READY_RX), "edge posted, consumer not yet armed");

    // The consumer arms a park at its STALE last_seen = 0. The observe re-check
    // sees ready_seq = 1 != 0 -> DON'T park; un-arm and re-process.
    TEST_ASSERT(!weft_ready_arm_park(&rv, 0), "arm sees the raced edge -> don't park");
    TEST_EXPECT_EQ((int)rh->wait_active, 0, "park-intent left clear (un-armed)");

    // The consumer re-processes, catches up to seq 1, then a fresh arm is safe.
    u32 last_seen = weft_ready_observe(&rv, NULL);
    TEST_EXPECT_EQ((int)last_seen, 1, "re-processed to the current edge");
    TEST_ASSERT(weft_ready_arm_park(&rv, last_seen), "arm caught-up -> safe to park");
    TEST_EXPECT_EQ((int)rh->wait_active, 1, "park-intent now published");

    weft_teardown(netd, guest, v);
}

// ---------------------------------------------------------------------------
// F_NOTIF zero-copy-send completion contract (Weft-5) -- the multi-holder
// buffer-pin release. The weft_notif tracker is kernel-private per-send
// completion state (NOT a shared page), so these are pure struct-level tests of
// the holder lifecycle (specs/weft.tla Holders / HolderRelease / ReleaseClean).
// ---------------------------------------------------------------------------

// weft.notif_terminal_release -- arm the {netd,nic,ack} in-flight set; the pin
// stays held (inflight) through each holder clear, and the LAST clear (any order)
// yields WEFT_NOTIF_RELEASE EXACTLY ONCE (the notification-terminal). A stray
// late event after the terminal is a HELD no-op (no double-release).
void test_weft_notif_terminal_release(void) {
    struct weft_notif n = {0};

    weft_notif_arm(&n, WEFT_HOLDERS_ALL);
    TEST_ASSERT(weft_notif_inflight(&n),               "armed -> in flight (pin held)");
    TEST_EXPECT_EQ((int)weft_notif_result_flags(&n), 0, "zero-copied -> no COPIED flag");

    // Clear in a non-trivial order (nic, ack, netd); each non-last clear keeps
    // the page in flight (a holder still pending -> the pin stays held).
    TEST_EXPECT_EQ((int)weft_notif_clear(&n, WEFT_HOLDER_NIC),  WEFT_NOTIF_HELD,
        "NIC done -> still held (netd/ack pending)");
    TEST_ASSERT(weft_notif_inflight(&n),               "still in flight after NIC");
    TEST_EXPECT_EQ((int)weft_notif_clear(&n, WEFT_HOLDER_ACK),  WEFT_NOTIF_HELD,
        "peer ACK -> still held (netd pending)");
    TEST_ASSERT(weft_notif_inflight(&n),               "still in flight after ACK");

    // The LAST holder clears -> RELEASE (the notification-terminal: drop the pin).
    TEST_EXPECT_EQ((int)weft_notif_clear(&n, WEFT_HOLDER_NETD), WEFT_NOTIF_RELEASE,
        "last holder (netd) -> RELEASE (notification-terminal)");
    TEST_ASSERT(!weft_notif_inflight(&n),              "no longer in flight (reusable)");

    // Exactly-once: a stray/duplicate completion after the terminal is a no-op,
    // NEVER a second RELEASE (no double pin-drop).
    TEST_EXPECT_EQ((int)weft_notif_clear(&n, WEFT_HOLDER_NETD), WEFT_NOTIF_HELD,
        "stray late netd event -> HELD (no second RELEASE)");
    TEST_EXPECT_EQ((int)weft_notif_clear(&n, WEFT_HOLDER_NIC),  WEFT_NOTIF_HELD,
        "stray late nic event -> HELD");
    TEST_ASSERT(!weft_notif_inflight(&n),              "still reusable (no re-arm)");
}

// weft.notif_premature_blocked -- the io_uring ubuf_info UAF, structurally
// prevented (weft.tla PinHeldWhileInFlight + NoInFlightReuse). After netd's stack
// is done (op-terminal) the page is STILL in flight (the NIC may DMA / the peer
// has not ACKed), so a reuse-path gating on weft_notif_inflight() blocks the
// reuse. The ONLY path to "reuse safe" is !inflight(), reached only once ALL
// holders clear (notification-terminal). A duplicate clear cannot fake it.
void test_weft_notif_premature_blocked(void) {
    struct weft_notif n = {0};
    weft_notif_arm(&n, WEFT_HOLDERS_ALL);

    // OP-terminal: netd's stack finished (the result CQE point). If the pin were
    // dropped HERE the page would be reused while the NIC/ACK still hold it = the
    // UAF. The tracker keeps it in flight.
    TEST_EXPECT_EQ((int)weft_notif_clear(&n, WEFT_HOLDER_NETD), WEFT_NOTIF_HELD,
        "netd done (op-terminal) -> NOT yet terminal");
    TEST_ASSERT(weft_notif_inflight(&n),
        "page STILL in flight after op-terminal -> reuse BLOCKED (no premature drop)");

    // A duplicate netd-done cannot empty the set (NIC/ACK still pending) -- it
    // can't manufacture a premature RELEASE.
    TEST_EXPECT_EQ((int)weft_notif_clear(&n, WEFT_HOLDER_NETD), WEFT_NOTIF_HELD,
        "duplicate netd-done -> HELD (NIC/ACK still hold the page)");
    TEST_ASSERT(weft_notif_inflight(&n), "duplicate cannot fake the terminal");

    TEST_EXPECT_EQ((int)weft_notif_clear(&n, WEFT_HOLDER_NIC),  WEFT_NOTIF_HELD,
        "NIC DMA done -> still held (ACK pending)");
    TEST_ASSERT(weft_notif_inflight(&n), "still in flight: peer ACK outstanding");

    // The peer ACKs -> the genuine notification-terminal; reuse is now safe.
    TEST_EXPECT_EQ((int)weft_notif_clear(&n, WEFT_HOLDER_ACK),  WEFT_NOTIF_RELEASE,
        "peer ACK (the true last holder) -> RELEASE");
    TEST_ASSERT(!weft_notif_inflight(&n), "reuse now safe (notification-terminal reached)");
}

// weft.notif_copied_immediate -- the fallback-copied path (IORING_SEND_ZC_REPORT_
// USAGE): a copied send is free IMMEDIATELY (no in-flight window); the result CQE
// carries WEFT_NOTIF_COPIED so netd is never silently-wrong. Plus the arm-domain
// masking (an over-wide holders argument cannot manufacture a phantom holder; an
// empty arm is not-in-flight).
void test_weft_notif_copied_immediate(void) {
    struct weft_notif n = {0};

    weft_notif_arm_copied(&n);
    TEST_ASSERT(!weft_notif_inflight(&n),
        "copied -> NOT in flight (buffer free immediately)");
    TEST_EXPECT_EQ((int)weft_notif_result_flags(&n), (int)WEFT_NOTIF_COPIED,
        "result CQE carries the COPIED indicator");
    // A clear on a copied (already-free) tracker is a harmless no-op.
    TEST_EXPECT_EQ((int)weft_notif_clear(&n, WEFT_HOLDER_NETD), WEFT_NOTIF_HELD,
        "clear on a copied send -> HELD no-op");

    // Arm-domain masking: an over-wide argument is masked to the valid holders;
    // only the three real holders end up pending.
    struct weft_notif m = {0};
    weft_notif_arm(&m, 0xFFFFFFFFu);
    TEST_ASSERT(weft_notif_inflight(&m), "over-wide arm masks to the valid holders");
    TEST_EXPECT_EQ((int)weft_notif_clear(&m, WEFT_HOLDER_NETD), WEFT_NOTIF_HELD, "netd");
    TEST_EXPECT_EQ((int)weft_notif_clear(&m, WEFT_HOLDER_NIC),  WEFT_NOTIF_HELD, "nic");
    TEST_EXPECT_EQ((int)weft_notif_clear(&m, WEFT_HOLDER_ACK),  WEFT_NOTIF_RELEASE,
        "exactly the three masked holders empty the set -> RELEASE");
    TEST_ASSERT(!weft_notif_inflight(&m), "no phantom holder remained");

    // An empty arm is a no-op send -- not in flight.
    struct weft_notif z = {0};
    weft_notif_arm(&z, 0u);
    TEST_ASSERT(!weft_notif_inflight(&z), "empty arm -> not in flight");
}
