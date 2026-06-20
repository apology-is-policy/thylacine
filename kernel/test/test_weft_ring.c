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

#define WEFT_TEST_VA   0x20000000ull          // 512 MiB; well inside user-VA
#define WEFT_RING_ENTRIES 8u
// A 4-page ring Burrow: hdr (64) + desc[8] (128) + a payload region spanning
// multiple pages (exercises the BURROW_TYPE_ANON contiguity the share rests on).
#define WEFT_RING_PAGES (4ull * PAGE_SIZE)
#define WEFT_DESC_OFF   64
#define WEFT_PAYLOAD_OFF (64 + 8 * 16)        // hdr + desc[8]

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
