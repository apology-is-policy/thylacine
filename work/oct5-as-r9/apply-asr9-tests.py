#!/usr/bin/env python3
"""AS-R9 native regression tests. Abort-before-write, exact anchors."""
import sys, pathlib
R = pathlib.Path(__file__).resolve().parents[2]
edits = []
def ed(p, old, new, label): edits.append((R / p, old, new, label))

TESTS = r'''

// =============================================================================
// AS-R9: the settled drops. The pre-fix sequence was claim the charge record,
// drop a ref, and -- if the drop reported non-final -- restore the record. The
// Burrow's lock protected each of those three operations and none of the gaps
// between them, so another holder could make the FINAL drop inside the window:
// the restore then wrote through a pointer whose last reference was gone (a
// use-after-free write, which burrow_free_internal's magic clobber turns into
// an extinction when the slot is still free, and into a charge planted on an
// unrelated region when SLUB has already reissued it), and the holder that
// actually freed the region found an empty record and refunded nothing.
//
// The repair folds the charge decision into the SAME lock interval that decides
// finality. These tests pin both halves of that: a non-final drop must leave
// the record ALONE, and the drop that frees must take it exactly once.
// =============================================================================

void test_burrow_settled_drop_retains_nonfinal_charge(void) {
    snap_counters();
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    struct Burrow *v = burrow_create_anon(2 * PAGE_SIZE, false);
    TEST_ASSERT(v != NULL, "burrow_create_anon NULL");
    burrow_charge_record(v, p, 2);
    burrow_ref(v);                                  // handle_count = 2
    TEST_EXPECT_EQ(burrow_handle_count(v), 2, "ref -> handle_count=2");

    u32 r1 = 0;
    bool f1 = burrow_unref_settled(v, p, &r1);
    TEST_ASSERT(!f1, "a non-final settled drop must not free");
    TEST_EXPECT_EQ(r1, 0u, "a non-final settled drop must settle nothing");

    // The AS-R9 property. The old sequence cleared the record here and put it
    // back afterwards; a racing final holder in that window found it empty.
    u32 peek = burrow_charge_claim_in(v, p->as);
    TEST_EXPECT_EQ(peek, 2u, "a non-final drop must RETAIN the payer's record");
    burrow_charge_restore_in(v, p->as, peek);

    u32 r2 = 0;
    bool f2 = burrow_unref_settled(v, p, &r2);
    TEST_ASSERT(f2, "the last settled drop frees the region");
    TEST_EXPECT_EQ(r2, 2u, "the freeing drop settles exactly the recorded pages");
    TEST_EXPECT_EQ(destroyed_since_snap(), (u64)1, "freed exactly once");

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

void test_burrow_settled_drop_exact_payer(void) {
    snap_counters();
    struct Proc *payer = proc_alloc();
    struct Proc *other = proc_alloc();
    TEST_ASSERT(payer != NULL && other != NULL, "proc_alloc failed");

    // A NULL payer settles nothing -- how a caller whose policy predicate fails
    // opts out (vma.c passes NULL for a shared-in or non-eager-ANON mapping).
    struct Burrow *a = burrow_create_anon(PAGE_SIZE, false);
    TEST_ASSERT(a != NULL, "burrow_create_anon NULL");
    burrow_charge_record(a, payer, 1);
    u32 ra = 0;
    TEST_ASSERT(burrow_unref_settled_in(a, NULL, &ra), "final drop frees");
    TEST_EXPECT_EQ(ra, 0u, "a NULL payer must settle nothing");

    // A non-payer's FINAL drop frees the region but must refund nothing: the
    // exact-payer rule is what keeps a Weft ring shared in from being refunded
    // to its consumer, and the repair must not have widened it.
    struct Burrow *b = burrow_create_anon(PAGE_SIZE, false);
    TEST_ASSERT(b != NULL, "burrow_create_anon NULL");
    burrow_charge_record(b, payer, 1);
    u32 rb = 0;
    TEST_ASSERT(burrow_unref_settled(b, other, &rb),
        "the last drop frees regardless of who makes it");
    TEST_EXPECT_EQ(rb, 0u, "a non-payer's final drop must refund nothing");

    TEST_EXPECT_EQ(destroyed_since_snap(), (u64)2, "both regions freed, once each");

    payer->state = PROC_STATE_ZOMBIE; proc_free(payer);
    other->state = PROC_STATE_ZOMBIE; proc_free(other);
}

void test_burrow_settled_mapping_drop_defers_free(void) {
    snap_counters();
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    struct Burrow *v = burrow_create_anon(PAGE_SIZE, false);
    TEST_ASSERT(v != NULL, "burrow_create_anon NULL");
    burrow_charge_record(v, p, 1);
    burrow_acquire_mapping(v);                      // handle=1, mapping=1

    // Drop the HANDLE first -- the mixed-holder order. Not final, so the record
    // must survive for the mapping drop that does end the occupancy.
    u32 r1 = 0;
    bool f1 = burrow_unref_settled(v, p, &r1);
    TEST_ASSERT(!f1, "a handle drop with a live mapping must not free");
    TEST_EXPECT_EQ(r1, 0u, "a non-final handle drop settles nothing");
    TEST_EXPECT_EQ(destroyed_since_snap(), (u64)0, "mapping still holds the pages");

    // Now the mapping drop is the last reference. Deferred contract: hand back
    // the dead Burrow rather than free it under the caller's as->lock, and
    // settle in the same interval that decided it.
    u32 r2 = 0;
    struct Burrow *dead = burrow_release_mapping_settled_deferred(v, p->as, &r2);
    TEST_ASSERT(dead == v, "the last mapping drop hands back the dead Burrow");
    TEST_EXPECT_EQ(r2, 1u, "the freeing mapping drop settles the record");
    TEST_EXPECT_EQ(destroyed_since_snap(), (u64)0,
        "deferred: the caller frees after its unlock, not inside the drop");

    burrow_free_deferred(dead);
    TEST_EXPECT_EQ(destroyed_since_snap(), (u64)1, "freed by burrow_free_deferred");

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// SYS_JIT_DESTROY keeps the claim/restore pair, and is sound only because every
// failure return in burrow_unmap_reporting precedes that function's first
// mutation: a refused unmap is NO teardown, not a partial one, so the alias it
// names still holds its mapping ref and the region cannot have been freed. That
// premise is the fragile half of the proof -- a failure return added BELOW the
// mutation point would silently make that site a use-after-free write -- so it
// is pinned here rather than left to a comment.
void test_burrow_unmap_failure_leaves_mapping_attached(void) {
    snap_counters();
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    struct Burrow *v = burrow_create_anon(PAGE_SIZE, false);
    TEST_ASSERT(v != NULL, "burrow_create_anon NULL");
    TEST_EXPECT_EQ(burrow_map(p, v, 0x10000000ull, PAGE_SIZE, VMA_PROT_RW), 0,
        "burrow_map should succeed on a clean Proc");
    int attached = burrow_mapping_count(v);
    TEST_ASSERT(attached >= 1, "the mapping must be attached before the refusals");
    burrow_charge_record(v, p, 1);

    // Every refusal burrow_unmap_reporting can issue, each of which must leave
    // the mapping attached and the charge record intact.
    TEST_ASSERT(burrow_unmap(p, 0x10000000ull, 2 * PAGE_SIZE) != 0,
        "a length that does not match the mapping exactly must be refused");
    TEST_ASSERT(burrow_unmap(p, 0x10000000ull + PAGE_SIZE, PAGE_SIZE) != 0,
        "a vaddr that is not the mapping's start must be refused");
    TEST_ASSERT(burrow_unmap(p, 0x10000000ull, 0) != 0,
        "a zero length must be refused");
    TEST_ASSERT(burrow_unmap(p, 0x10000000ull + 1, PAGE_SIZE) != 0,
        "a misaligned vaddr must be refused");
    TEST_ASSERT(burrow_unmap(p, 0x10000000ull, PAGE_SIZE + 1) != 0,
        "a misaligned length must be refused");

    TEST_EXPECT_EQ(burrow_mapping_count(v), attached,
        "a REFUSED burrow_unmap must leave its mapping attached -- this is the "
        "remaining-reference premise SYS_JIT_DESTROY's charge restore rests on");
    TEST_EXPECT_EQ(destroyed_since_snap(), (u64)0,
        "no refused unmap may free the region");
    u32 held = burrow_charge_claim_in(v, p->as);
    TEST_EXPECT_EQ(held, 1u,
        "a refused burrow_unmap must leave the charge record intact");
    burrow_charge_restore_in(v, p->as, held);

    // The positive control one variable away: the same call with the mapping's
    // exact geometry succeeds. Without it, every assertion above is satisfied by
    // a burrow_unmap that refuses unconditionally.
    TEST_EXPECT_EQ(burrow_unmap(p, 0x10000000ull, PAGE_SIZE), 0,
        "the correctly-shaped unmap must still succeed");
    TEST_EXPECT_EQ(burrow_mapping_count(v), attached - 1,
        "the accepted unmap detaches exactly one mapping");

    u32 settled = 0;
    TEST_ASSERT(burrow_unref_settled(v, p, &settled), "last handle drop frees");
    TEST_EXPECT_EQ(settled, 1u, "and settles the record it left intact");
    TEST_EXPECT_EQ(destroyed_since_snap(), (u64)1, "freed exactly once");

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}
'''

ed('kernel/test/test_burrow.c',
"""    TEST_ASSERT(burrow_create_anon((size_t)-1, false) == NULL,
        "and the creator refuses the same size (the guards agree)");
}
""",
"""    TEST_ASSERT(burrow_create_anon((size_t)-1, false) == NULL,
        "and the creator refuses the same size (the guards agree)");
}
""" + TESTS,
'test_burrow.c: append the AS-R9 tests')

ed('kernel/test/test_burrow.c',
"""#include <thylacine/types.h>
#include <thylacine/burrow.h>
""",
"""#include <thylacine/types.h>
#include <thylacine/burrow.h>
#include <thylacine/vma.h>         // AS-R9: VMA_PROT_RW for the unmap-refusal witness
""",
'test_burrow.c: include vma.h')

ed('kernel/test/test.c',
"""void test_vmo_size_overflow_rejected(void);
""",
"""void test_vmo_size_overflow_rejected(void);
// AS-R9: settled drops + the JIT remaining-reference premise.
void test_burrow_settled_drop_retains_nonfinal_charge(void);
void test_burrow_settled_drop_exact_payer(void);
void test_burrow_settled_mapping_drop_defers_free(void);
void test_burrow_unmap_failure_leaves_mapping_attached(void);
""",
'test.c: declare the AS-R9 tests')

ed('kernel/test/test.c',
"""    { "burrow.size_overflow_rejected",    test_vmo_size_overflow_rejected,    false, NULL },
""",
"""    { "burrow.size_overflow_rejected",    test_vmo_size_overflow_rejected,    false, NULL },
    // AS-R9: the charge decision inside the drop's lock interval.
    { "burrow.settled_drop_retains_nonfinal_charge",
      test_burrow_settled_drop_retains_nonfinal_charge, false, NULL },
    { "burrow.settled_drop_exact_payer",
      test_burrow_settled_drop_exact_payer,             false, NULL },
    { "burrow.settled_mapping_drop_defers_free",
      test_burrow_settled_mapping_drop_defers_free,     false, NULL },
    { "burrow.unmap_failure_leaves_mapping_attached",
      test_burrow_unmap_failure_leaves_mapping_attached, false, NULL },
""",
'test.c: register the AS-R9 tests')

texts, fail = {}, False
for path, old, new, label in edits:
    t = texts.get(path)
    if t is None: t = texts[path] = path.read_text()
    n = t.count(old)
    if n != 1:
        print(f'ABORT [{label}]: anchor occurs {n} times in {path.name}, expected 1'); fail = True
    else:
        texts[path] = t.replace(old, new, 1); print(f'  ok  [{label}]')
if fail:
    print('NOTHING WRITTEN'); sys.exit(1)
for path, t in texts.items():
    path.write_text(t); print(f'wrote {path.relative_to(R)}')
