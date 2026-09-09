// P4-Ic5b1b: KObj_DMA lifecycle + Burrow integration + syscall-path tests.
//
// Per <thylacine/dma_handle.h> + specs/handles.tla. The KObj_DMA's
// HwResourceExclusive enforcement comes "for free" from the buddy
// allocator (each alloc_pages call returns a fresh chunk), so the tests
// here focus on:
//
//   1. Basic lifecycle: create returns a valid struct with refcount=1,
//      contiguous PA, page-aligned size.
//   2. Argument validation: zero / overflow / oversize rejected.
//   3. Refcount discipline: ref/unref balanced; last unref frees pages.
//   4. Burrow integration: burrow_create_dma takes a kobj_dma ref;
//      burrow_unref drops it; proc_free tears down VMA → Burrow → KObj.
//   5. Page-content correctness: KP_ZERO at create ⇒ the buffer reads
//      as all-zero from the kernel direct-map alias.

#include "test.h"

#include <thylacine/burrow.h>
#include <thylacine/dma_handle.h>
#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>

#include "../../arch/arm64/uart.h"

void test_dma_handle_create_basic(void);
void test_dma_handle_create_zero_size_rejected(void);
void test_dma_handle_create_oversize_rejected(void);
void test_dma_handle_create_round_up_to_page(void);
void test_dma_handle_distinct_pa(void);
void test_dma_handle_unref_releases_chunk(void);
void test_dma_handle_zero_init(void);
void test_burrow_dma_create_basic(void);
void test_burrow_dma_create_null_rejected(void);
void test_burrow_dma_holds_kobj_ref(void);
void test_burrow_dma_lifecycle_round_trip(void);
void test_dma_map_install_vma(void);
void test_dma_map_proc_free_releases_kobj(void);

// Test sizes — small (fits comfortably in any test universe).
#define TEST_DMA_SIZE_1PAGE  0x1000ull
#define TEST_DMA_SIZE_4PAGE  0x4000ull
#define USER_VA_DMA          0x50000000ull

extern struct Proc *proc_alloc(void);
extern void         proc_free(struct Proc *p);

// =============================================================================
// KObj_DMA layer.
// =============================================================================

void test_dma_handle_create_basic(void) {
    struct KObj_DMA *k = kobj_dma_create(TEST_DMA_SIZE_1PAGE);
    TEST_ASSERT(k != NULL, "kobj_dma_create returned NULL");
    TEST_EXPECT_EQ(k->size, (size_t)TEST_DMA_SIZE_1PAGE, "wrong size");
    TEST_EXPECT_EQ(k->ref, 1, "ref should start at 1");
    // WEAVE-SKEIN: plain DMA is single-block by construction (a virtqueue
    // descriptor table must be contiguous), so nblk == 1 is part of its
    // contract, not an accident of this size.
    TEST_EXPECT_EQ((int)k->nblk, 1, "plain DMA must be single-block");
    TEST_ASSERT(k->blk[0].pages != NULL, "pages should be allocated");
    TEST_ASSERT((k->blk[0].pa & (PAGE_SIZE - 1)) == 0, "pa should be page-aligned");
    TEST_EXPECT_EQ(k->blk[0].pa, page_to_pa(k->blk[0].pages),
                   "pa should match page_to_pa(pages)");
    TEST_EXPECT_EQ(kobj_dma_pa_at(k, 0), k->blk[0].pa,
                   "offset 0 must resolve to block 0's base");
    kobj_dma_unref(k);
}

void test_dma_handle_create_zero_size_rejected(void) {
    struct KObj_DMA *k = kobj_dma_create(0);
    TEST_ASSERT(k == NULL, "size=0 must reject");
}

void test_dma_handle_create_oversize_rejected(void) {
    // KOBJ_DMA_MAX_SIZE + 1 page must reject.
    struct KObj_DMA *k = kobj_dma_create(KOBJ_DMA_MAX_SIZE + PAGE_SIZE);
    TEST_ASSERT(k == NULL, "oversize must reject");
}

// Sub-page request gets rounded up to a full page.
void test_dma_handle_create_round_up_to_page(void) {
    struct KObj_DMA *k = kobj_dma_create(1);
    TEST_ASSERT(k != NULL, "kobj_dma_create(1) failed (page-up rounding)");
    TEST_EXPECT_EQ(k->size, (size_t)PAGE_SIZE, "size should round up to PAGE_SIZE");
    kobj_dma_unref(k);
}

// Two creates must yield distinct PAs (buddy allocator's per-alloc
// partitioning is the HwResourceExclusive enforcement for DMA).
void test_dma_handle_distinct_pa(void) {
    struct KObj_DMA *k1 = kobj_dma_create(TEST_DMA_SIZE_1PAGE);
    TEST_ASSERT(k1 != NULL, "k1 create failed");
    struct KObj_DMA *k2 = kobj_dma_create(TEST_DMA_SIZE_1PAGE);
    TEST_ASSERT(k2 != NULL, "k2 create failed");
    TEST_EXPECT_NE(k1->blk[0].pa, k2->blk[0].pa, "two creates must yield distinct PAs");
    kobj_dma_unref(k1);
    kobj_dma_unref(k2);
}

void test_dma_handle_unref_releases_chunk(void) {
    u64 live_before = kobj_dma_live_count();
    struct KObj_DMA *k = kobj_dma_create(TEST_DMA_SIZE_4PAGE);
    TEST_ASSERT(k != NULL, "create failed");
    TEST_EXPECT_EQ(kobj_dma_live_count(), live_before + 1, "live should bump");

    kobj_dma_unref(k);
    TEST_EXPECT_EQ(kobj_dma_live_count(), live_before, "live should drop on final unref");
}

// KP_ZERO at alloc means the buffer reads as all-zero immediately.
// Verify by reading through the kernel direct map (pa_to_kva).
void test_dma_handle_zero_init(void) {
    struct KObj_DMA *k = kobj_dma_create(TEST_DMA_SIZE_1PAGE);
    TEST_ASSERT(k != NULL, "create failed");

    volatile u8 *p = (volatile u8 *)pa_to_kva(kobj_dma_pa_at(k, 0));
    for (size_t i = 0; i < k->size; i++) {
        TEST_EXPECT_EQ(p[i], (u8)0, "DMA buffer not zero-initialized");
    }
    kobj_dma_unref(k);
}

// =============================================================================
// burrow_create_dma layer.
// =============================================================================

void test_burrow_dma_create_basic(void) {
    struct KObj_DMA *kd = kobj_dma_create(TEST_DMA_SIZE_1PAGE);
    TEST_ASSERT(kd != NULL, "kobj_dma_create failed");

    struct Burrow *b = burrow_create_dma(kd);
    TEST_ASSERT(b != NULL, "burrow_create_dma failed");
    TEST_EXPECT_EQ((int)b->type, (int)BURROW_TYPE_DMA, "wrong type");
    TEST_EXPECT_EQ(b->size, (size_t)TEST_DMA_SIZE_1PAGE, "wrong size");
    // WEAVE-SKEIN: a DMA Burrow carries NO base PA -- the backing is a skein
    // resolved per page through kobj_dma_pa_at. 0 here is the contract, not
    // an unset field: a plausible base is what a future reader would add an
    // offset to and address another object's pages once a weave scatters.
    TEST_EXPECT_EQ(b->pa, (u64)0, "DMA Burrow must carry no base pa");
    TEST_ASSERT(b->pages == NULL, "DMA Burrow should have pages=NULL (chunk on kobj)");
    TEST_EXPECT_EQ(b->handle_count, 1, "construction ref should be 1");
    TEST_EXPECT_EQ(b->mapping_count, 0, "mapping_count starts at 0");
    TEST_ASSERT(b->kobj_dma == kd, "kobj_dma field not set correctly");

    burrow_unref(b);    // frees b + drops Burrow's kobj_dma ref
    kobj_dma_unref(kd);  // drops caller's ref → KObj_DMA freed + pages released
}

void test_burrow_dma_create_null_rejected(void) {
    struct Burrow *b = burrow_create_dma(NULL);
    TEST_ASSERT(b == NULL, "burrow_create_dma(NULL) should return NULL");
}

// Burrow's ref keeps the underlying KObj_DMA alive past the caller's
// own unref. Verified via kobj_dma_live_count.
void test_burrow_dma_holds_kobj_ref(void) {
    u64 live_before = kobj_dma_live_count();

    struct KObj_DMA *kd = kobj_dma_create(TEST_DMA_SIZE_1PAGE);
    TEST_ASSERT(kd != NULL, "create failed");
    TEST_EXPECT_EQ(kobj_dma_live_count(), live_before + 1, "create should bump live");

    struct Burrow *b = burrow_create_dma(kd);
    TEST_ASSERT(b != NULL, "burrow_create_dma failed");
    // Burrow create doesn't make a new KObj_DMA.
    TEST_EXPECT_EQ(kobj_dma_live_count(), live_before + 1,
                   "burrow_create_dma should not bump live");

    // Drop caller's ref. Burrow's ref keeps it alive.
    kobj_dma_unref(kd);
    TEST_EXPECT_EQ(kobj_dma_live_count(), live_before + 1,
                   "kobj should stay alive (Burrow holds ref)");

    // Drop Burrow → last ref drops → kobj freed.
    burrow_unref(b);
    TEST_EXPECT_EQ(kobj_dma_live_count(), live_before,
                   "kobj should be freed after Burrow unref");
}

// Symmetric: Burrow first, kobj second.
void test_burrow_dma_lifecycle_round_trip(void) {
    u64 live_before = kobj_dma_live_count();

    struct KObj_DMA *kd = kobj_dma_create(TEST_DMA_SIZE_1PAGE);
    TEST_ASSERT(kd != NULL, "create failed");
    struct Burrow *b = burrow_create_dma(kd);
    TEST_ASSERT(b != NULL, "burrow create failed");

    burrow_unref(b);
    TEST_EXPECT_EQ(kobj_dma_live_count(), live_before + 1,
                   "kobj alive while caller holds ref");

    kobj_dma_unref(kd);
    TEST_EXPECT_EQ(kobj_dma_live_count(), live_before,
                   "kobj freed when both refs gone");
}

// =============================================================================
// burrow_map + VMA integration.
// =============================================================================

// burrow_create_dma + burrow_map installs a VMA reachable via vma_lookup.
// proc_free tears down: VMA → burrow_release_mapping → burrow_free_internal
// → kobj_dma_unref → free_pages.
void test_dma_map_install_vma(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    struct KObj_DMA *kd = kobj_dma_create(TEST_DMA_SIZE_4PAGE);
    TEST_ASSERT(kd != NULL, "kobj_dma_create failed");

    struct Burrow *b = burrow_create_dma(kd);
    TEST_ASSERT(b != NULL, "burrow_create_dma failed");

    int rc = burrow_map(p, b, USER_VA_DMA, TEST_DMA_SIZE_4PAGE, VMA_PROT_RW);
    TEST_EXPECT_EQ(rc, 0, "burrow_map failed");
    burrow_unref(b);    // transfer ref to VMA

    struct Vma *vma = vma_lookup(p, USER_VA_DMA);
    TEST_ASSERT(vma != NULL, "vma_lookup didn't find the new VMA");
    TEST_EXPECT_EQ(vma->vaddr_start, (u64)USER_VA_DMA, "wrong vaddr_start");
    TEST_EXPECT_EQ(vma->vaddr_end,   (u64)(USER_VA_DMA + TEST_DMA_SIZE_4PAGE),
                   "wrong vaddr_end");
    TEST_ASSERT(vma->burrow != NULL, "VMA's burrow is NULL");
    TEST_EXPECT_EQ((int)vma->burrow->type, (int)BURROW_TYPE_DMA,
                   "VMA's burrow has wrong type");
    TEST_EXPECT_EQ(vma->burrow->pa, (u64)0, "DMA Burrow must carry no base pa");
    TEST_EXPECT_EQ(kobj_dma_pa_at(vma->burrow->kobj_dma, 0), kd->blk[0].pa,
                   "the VMA's burrow must resolve to the same backing");

    // Clean up.
    p->state = 2;     // PROC_STATE_ZOMBIE
    proc_free(p);
    kobj_dma_unref(kd);
}

// proc_free path correctly tears down the entire chain even when only
// the VMA holds the Burrow ref (caller already dropped kd). Verifies
// the cross-subsystem refcount handoff matches the MMIO pattern.
void test_dma_map_proc_free_releases_kobj(void) {
    u64 live_before = kobj_dma_live_count();

    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    struct KObj_DMA *kd = kobj_dma_create(TEST_DMA_SIZE_1PAGE);
    TEST_ASSERT(kd != NULL, "create failed");
    TEST_EXPECT_EQ(kobj_dma_live_count(), live_before + 1, "live +1");

    struct Burrow *b = burrow_create_dma(kd);
    TEST_ASSERT(b != NULL, "burrow create failed");
    int rc = burrow_map(p, b, USER_VA_DMA, TEST_DMA_SIZE_1PAGE, VMA_PROT_RW);
    TEST_EXPECT_EQ(rc, 0, "burrow_map failed");
    burrow_unref(b);   // transfer to VMA

    // Drop caller's kd ref BEFORE proc_free. VMA's Burrow still holds.
    kobj_dma_unref(kd);
    TEST_EXPECT_EQ(kobj_dma_live_count(), live_before + 1,
                   "kobj stays alive while VMA's Burrow holds ref");

    p->state = 2;     // PROC_STATE_ZOMBIE
    proc_free(p);
    TEST_EXPECT_EQ(kobj_dma_live_count(), live_before,
                   "kobj freed after proc_free walks VMAs + Burrows");
}

// =============================================================================
// WEAVE-SKEIN: the scattered-backing layer.
// =============================================================================
//
// These test the property the whole change exists for: a weave larger than one
// SKEIN_BLOCK is backed by N contiguous runs, and every buffer byte resolves to
// exactly one page of exactly one of them. A single-span 48.8 MiB weave demands
// a 64 MiB naturally-aligned buddy block and fails with 1889 MiB free
// (measured); the skein asks for 2 MiB at a time.
//
// The size below deliberately does NOT divide evenly by SKEIN_BLOCK, so the
// tail block is exercised on every run -- a size that divided evenly would
// leave the tail arm (the one place block length differs from the stride)
// permanently unconstructed.
#define TEST_SKEIN_SIZE  (5ull * 1024 * 1024 + 3ull * 4096)   // 5 MiB + 12 KiB

// The blocks a skein reports must together cover the buffer exactly once:
// contiguous in buffer order, no gap, no overlap, summing to size.
void test_skein_blocks_tile_the_buffer(void) {
    struct KObj_DMA *k = kobj_dma_create_weave(TEST_SKEIN_SIZE);
    TEST_ASSERT(k != NULL, "weave create failed");

    // 5 MiB + 12 KiB over 2 MiB blocks = 3 blocks (2 full + a 1 MiB+12 KiB
    // tail). Asserted rather than derived so a SKEIN_BLOCK change is a visible
    // test failure rather than a silently-retuned expectation.
    TEST_EXPECT_EQ((int)k->nblk, 3, "5 MiB + 12 KiB must split into 3 blocks");

    u64 sum = 0;
    for (u32 i = 0; i < k->nblk; i++) {
        u64 len = kobj_dma_block_len(k, i);
        TEST_ASSERT(len != 0, "every block must report a non-zero length");
        TEST_ASSERT((k->blk[i].pa & (PAGE_SIZE - 1)) == 0,
                    "every block base must be page-aligned");
        TEST_ASSERT((len & (PAGE_SIZE - 1)) == 0,
                    "every block length must be a page multiple");
        if (i + 1 < k->nblk) {
            TEST_EXPECT_EQ(len, (u64)SKEIN_BLOCK,
                           "every block but the last spans a full SKEIN_BLOCK");
        }
        sum += len;
    }
    TEST_EXPECT_EQ(sum, (u64)k->size, "block lengths must sum to the size");

    // Pairwise disjoint. THE I-45 obligation: no other object's page may
    // appear in the list a device is handed, and two entries naming the same
    // page would also mean the buffer aliases itself.
    for (u32 i = 0; i < k->nblk; i++) {
        for (u32 j = i + 1; j < k->nblk; j++) {
            u64 ai = k->blk[i].pa, bi = ai + kobj_dma_block_len(k, i);
            u64 aj = k->blk[j].pa, bj = aj + kobj_dma_block_len(k, j);
            TEST_ASSERT(bi <= aj || bj <= ai, "skein blocks must not overlap");
        }
    }
    kobj_dma_unref(k);
}

// Every offset resolves into the block that owns it, at the right page, and
// out-of-range offsets refuse. This is the property the demand-fault arm
// depends on: get it wrong and a client's mapping addresses pages the object
// does not own (I-45) or pages that were freed (I-7).
void test_skein_offset_resolution(void) {
    struct KObj_DMA *k = kobj_dma_create_weave(TEST_SKEIN_SIZE);
    TEST_ASSERT(k != NULL, "weave create failed");

    // Walk EVERY page of the buffer, not a sample: the interesting offsets are
    // the block boundaries, and a sample chosen by stride can step over them.
    u64 covered = 0;
    for (u32 i = 0; i < k->nblk; i++) {
        u64 len = kobj_dma_block_len(k, i);
        for (u64 off = 0; off < len; off += PAGE_SIZE) {
            u64 got = kobj_dma_pa_at(k, covered + off);
            TEST_EXPECT_EQ(got, k->blk[i].pa + off,
                           "offset must resolve inside its own block");
        }
        covered += len;
    }
    TEST_EXPECT_EQ(covered, (u64)k->size, "the walk must cover the buffer");

    // In-page offsets ride along: the resolver is byte-granular, and the fault
    // arm's page-masking is its caller's business, not a precondition.
    TEST_EXPECT_EQ(kobj_dma_pa_at(k, 0x37), k->blk[0].pa + 0x37,
                   "a sub-page offset must carry into the returned PA");

    // Out of range refuses. 0 is never a valid backing PA (page 0 is not
    // buddy-allocated), which is what lets every caller test with one compare.
    TEST_EXPECT_EQ(kobj_dma_pa_at(k, k->size), (u64)0,
                   "the first byte past the buffer must refuse");
    TEST_EXPECT_EQ(kobj_dma_pa_at(k, ~(u64)0), (u64)0,
                   "a wild offset must refuse");
    TEST_EXPECT_EQ(kobj_dma_pa_at(NULL, 0), (u64)0, "NULL must refuse");

    kobj_dma_unref(k);
}

// A weave that fits in ONE block must not be padded up to a full SKEIN_BLOCK:
// the kernel suite and the small-surface path mint tiny weaves, and rounding
// an 8 KiB weave to 2 MiB would be a 256x waste for no gain.
void test_skein_small_weave_stays_one_block(void) {
    struct KObj_DMA *k = kobj_dma_create_weave(2 * PAGE_SIZE);
    TEST_ASSERT(k != NULL, "small weave create failed");
    TEST_EXPECT_EQ((int)k->nblk, 1, "a weave under SKEIN_BLOCK stays one block");
    TEST_EXPECT_EQ(kobj_dma_block_len(k, 0), (u64)(2 * PAGE_SIZE),
                   "the single block must report exactly the buffer size");
    TEST_EXPECT_EQ((int)k->blk[0].order, 1, "2 pages must not over-allocate");
    kobj_dma_unref(k);
}

// Plain DMA and GPU BOs stay single-block DELIBERATELY (a virtqueue descriptor
// table must be contiguous -- the device walks it by address with no length
// list). This is the guard on the ratified weave-only scope: if a later change
// scatters them, virtio-net and virtio-blk break in a way no GPU test sees.
void test_skein_scope_is_weave_only(void) {
    struct KObj_DMA *plain = kobj_dma_create(KOBJ_DMA_MAX_SIZE);
    TEST_ASSERT(plain != NULL, "plain create at the envelope failed");
    TEST_EXPECT_EQ((int)plain->nblk, 1, "plain DMA must never scatter");
    kobj_dma_unref(plain);

    struct KObj_DMA *bo = kobj_dma_create_gpu_bo(4 * SKEIN_BLOCK);
    TEST_ASSERT(bo != NULL, "gpu_bo create failed");
    TEST_EXPECT_EQ((int)bo->nblk, 1, "GPU BOs stay single-block at this scope");
    kobj_dma_unref(bo);
}

// The whole point, at the size that provoked the change: a weave whose single
// span would be an order-14 (64 MiB) allocation. On a fragmented long-uptime
// heap the single-span form is what fails; here it must succeed, and every
// page must still resolve.
void test_skein_full_envelope_weave(void) {
    struct KObj_DMA *k = kobj_dma_create_weave(KOBJ_DMA_WEAVE_MAX_SIZE);
    TEST_ASSERT(k != NULL, "a full-envelope weave must allocate");
    TEST_EXPECT_EQ((int)k->nblk, (int)KOBJ_DMA_MAX_BLOCKS,
                   "the full envelope must use every block slot");

    u64 sum = 0;
    for (u32 i = 0; i < k->nblk; i++) sum += kobj_dma_block_len(k, i);
    TEST_EXPECT_EQ(sum, (u64)KOBJ_DMA_WEAVE_MAX_SIZE,
                   "the blocks must cover the whole envelope");

    // The last byte is the one a boundary error loses.
    TEST_ASSERT(kobj_dma_pa_at(k, KOBJ_DMA_WEAVE_MAX_SIZE - 1) != 0,
                "the final byte must resolve");
    TEST_EXPECT_EQ(kobj_dma_pa_at(k, KOBJ_DMA_WEAVE_MAX_SIZE), (u64)0,
                   "one byte past the envelope must refuse");
    kobj_dma_unref(k);
}

// Zeroing survives scattering. KP_ZERO is per-block, so a missed block would
// leak a prior occupant's bytes into a client's first map -- and for a weave
// those bytes are another surface's PIXELS.
void test_skein_zero_init_across_blocks(void) {
    struct KObj_DMA *k = kobj_dma_create_weave(TEST_SKEIN_SIZE);
    TEST_ASSERT(k != NULL, "weave create failed");

    for (u64 off = 0; off < k->size; off += PAGE_SIZE) {
        u64 pa = kobj_dma_pa_at(k, off);
        TEST_ASSERT(pa != 0, "every page must resolve");
        volatile u64 *w = (volatile u64 *)pa_to_kva(pa);
        // One word per page: the buddy zeroes whole pages, so a per-page
        // sample discriminates a missed BLOCK, which is the failure this
        // guards. A byte-by-byte sweep of 5 MiB would cost the suite far more
        // than it adds.
        TEST_EXPECT_EQ(w[0], (u64)0, "a skein page was not zeroed");
    }
    kobj_dma_unref(k);
}

// A SINGLE-BLOCK object LARGER than SKEIN_BLOCK must resolve throughout.
//
// This is the case scope_is_weave_only constructs and never queries: a GPU BO
// is single-block by design but its envelope is KOBJ_DMA_GPU_BO_MAX_SIZE
// (64 MiB), so `nblk == 1` and `size > SKEIN_BLOCK` is an ORDINARY state, not
// an exotic one -- tapestryd's WARP_CTX_BACKING_MAX is 64 MiB and its own
// comment names a client's 32 MiB texture heap. Resolving such an object by
// dividing the offset by SKEIN_BLOCK yields a block index past nblk and
// refuses every byte after the first block, so the client faults on most of
// its own buffer.
//
// Asserting nblk == 1 was never enough: the bound has to be exercised on the
// object that has it, not merely on an object that could.
void test_skein_single_block_larger_than_a_block(void) {
    struct KObj_DMA *k = kobj_dma_create_gpu_bo(8 * SKEIN_BLOCK);
    TEST_ASSERT(k != NULL, "large gpu_bo create failed");
    TEST_EXPECT_EQ((int)k->nblk, 1, "a GPU BO is single-block by design");

    // The whole buffer resolves contiguously off the one block -- including
    // the bytes past the first SKEIN_BLOCK, which is the point.
    for (u64 off = 0; off < k->size; off += PAGE_SIZE) {
        TEST_EXPECT_EQ(kobj_dma_pa_at(k, off), k->blk[0].pa + off,
                       "a single-block object must resolve across its whole size");
    }
    TEST_EXPECT_EQ(kobj_dma_block_len(k, 0), (u64)k->size,
                   "the single block must report the whole buffer");
    TEST_EXPECT_EQ(kobj_dma_pa_at(k, k->size), (u64)0,
                   "one byte past the end must still refuse");
    kobj_dma_unref(k);
}
