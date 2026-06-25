// test_vdso.c — the monotonic-clock vDSO page (docs/VDSO-DESIGN.md, #343).
//
// Five tests:
//   vdso.page_populated     — vdso_init seeded magic/version/freq/wall_offset_ns
//                             to the live timer values.
//   vdso.publish_updates    — vdso_publish_wall mirrors a fresh offset into the
//                             page (and restores).
//   vdso.mono_matches_timer — the userspace computation (CNTVCT + page->freq,
//                             timer_now_ns's split form) brackets timer_now_ns()
//                             -> a reader gets a CLOCK_MONOTONIC consistent with
//                             the syscall path.
//   vdso.maps_ro_read       — the shared page burrow_maps RO into a Proc; a READ
//                             fault installs a PTE pointing at the ONE shared
//                             physical page (the vvar model).
//   vdso.maps_ro_write_faults— a WRITE fault on the RO mapping is rejected
//                             (FAULT_UNHANDLED_USER) -> I-13: a user write can
//                             never corrupt the shared timebase.

#include "test.h"

#include "../../arch/arm64/fault.h"   // userland_demand_page, fault_info, FAULT_*
#include "../../arch/arm64/timer.h"   // timer_get_freq / timer_now_ns / offset_now

#include <thylacine/vdso.h>
#include <thylacine/burrow.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>

void test_vdso_page_populated(void);
void test_vdso_publish_updates(void);
void test_vdso_mono_matches_timer(void);
void test_vdso_maps_ro_read(void);
void test_vdso_maps_ro_write_faults(void);

#define TEST_VA   0x20000000ull        // 512 MiB; well within the user half

// The kernel VA of the shared clock page (NULL if vdso_init OOM'd at boot).
static struct vdso_clock *vdso_page(void) {
    struct Burrow *v = vdso_clock_burrow();
    if (!v || !v->pages) return NULL;
    return (struct vdso_clock *)pa_to_kva(page_to_pa(v->pages));
}

static inline u64 read_cntvct(void) {
    u64 c;
    __asm__ __volatile__("isb\n\tmrs %0, cntvct_el0" : "=r"(c));
    return c;
}

// Replicates timer_now_ns()'s split form exactly (the arithmetic a userspace
// reader runs over the page's freq).
static u64 mono_from_cnt(u64 cnt, u64 freq) {
    if (freq == 0) return 0;
    return (cnt / freq) * 1000000000ull
         + (cnt % freq) * 1000000000ull / freq;
}

void test_vdso_page_populated(void) {
    struct vdso_clock *pg = vdso_page();
    TEST_ASSERT(pg != NULL, "vdso page present (vdso_init must have run)");
    TEST_EXPECT_EQ(pg->magic, VDSO_CLOCK_MAGIC, "magic == VDSO_CLOCK_MAGIC");
    TEST_EXPECT_EQ(pg->version, (u64)VDSO_CLOCK_VERSION, "version");
    TEST_EXPECT_EQ(pg->freq, (u64)timer_get_freq(), "freq == CNTFRQ");
    TEST_ASSERT(pg->freq != 0, "freq must be non-zero on a real timer");
    TEST_EXPECT_EQ(pg->wall_offset_ns, timer_wallclock_offset_ns_now(),
        "wall_offset_ns seeded from the boot anchor");
}

void test_vdso_publish_updates(void) {
    struct vdso_clock *pg = vdso_page();
    TEST_ASSERT(pg != NULL, "vdso page present");

    u64 saved = timer_wallclock_offset_ns_now();   // the live offset (== page)
    const u64 sentinel = 0x0123456789ABCDEFull;

    vdso_publish_wall(sentinel);
    TEST_EXPECT_EQ(pg->wall_offset_ns, sentinel,
        "publish mirrors the offset into the page");

    vdso_publish_wall(saved);                        // restore consistency
    TEST_EXPECT_EQ(pg->wall_offset_ns, saved, "offset restored");
}

void test_vdso_mono_matches_timer(void) {
    struct vdso_clock *pg = vdso_page();
    TEST_ASSERT(pg != NULL, "vdso page present");

    // A userspace reader: sample CNTVCT once, divide by the page's freq. Bracket
    // it between two timer_now_ns() reads -- since CNTVCT is monotonic and the
    // formula is identical, the page-computed mono must land in [t0, t1].
    u64 t0  = timer_now_ns();
    u64 cnt = read_cntvct();
    u64 t1  = timer_now_ns();
    u64 page_mono = mono_from_cnt(cnt, pg->freq);

    TEST_ASSERT(page_mono >= t0, "page-computed mono >= timer before");
    TEST_ASSERT(page_mono <= t1, "page-computed mono <= timer after");
}

static void make_read_fi(struct fault_info *fi, u64 vaddr, bool is_write) {
    fi->vaddr          = vaddr;
    fi->elr            = 0;
    fi->esr            = 0;
    fi->ec             = 0x24;          // EC_DATA_ABORT_LOWER
    fi->fsc            = 0x07;          // FSC_TRANS_FAULT_L3
    fi->fault_level    = 3;
    fi->from_user      = true;
    fi->is_instruction = false;
    fi->is_write       = is_write;
    fi->is_translation = true;
    fi->is_permission  = false;
    fi->is_access_flag = false;
}

void test_vdso_maps_ro_read(void) {
    struct Burrow *v = vdso_clock_burrow();
    TEST_ASSERT(v != NULL, "vdso burrow present");

    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc");

    // RO-map the SHARED vDSO burrow (do NOT unref it -- it is kernel-owned and
    // held forever; the mapping ref balances at proc_free's vma_drain).
    int rc = burrow_map(p, v, TEST_VA, PAGE_SIZE, VMA_PROT_READ);
    TEST_EXPECT_EQ(rc, 0, "burrow_map RO");

    struct fault_info fi;
    make_read_fi(&fi, TEST_VA + 0x40, /*is_write=*/false);
    enum fault_result r = userland_demand_page(p, &fi);
    TEST_EXPECT_EQ(r, FAULT_HANDLED, "read fault resolves the RO vDSO page");

    // The installed leaf PTE must point at the ONE shared physical page (the
    // vvar model: every Proc's mapping resolves to page_to_pa(v->pages)).
    u64 *l0 = (u64 *)pa_to_kva(p->pgtable_root);
    u64 i0 = (TEST_VA >> 39) & 0x1FF, i1 = (TEST_VA >> 30) & 0x1FF;
    u64 i2 = (TEST_VA >> 21) & 0x1FF, i3 = (TEST_VA >> 12) & 0x1FF;
    TEST_ASSERT(l0[i0] & 1, "L0 valid");
    u64 *l1 = (u64 *)pa_to_kva(l0[i0] & 0x0000FFFFFFFFF000ull);
    TEST_ASSERT(l1[i1] & 1, "L1 valid");
    u64 *l2 = (u64 *)pa_to_kva(l1[i1] & 0x0000FFFFFFFFF000ull);
    TEST_ASSERT(l2[i2] & 1, "L2 valid");
    u64 *l3 = (u64 *)pa_to_kva(l2[i2] & 0x0000FFFFFFFFF000ull);
    u64 pte = l3[i3];
    TEST_ASSERT(pte & 1, "L3 leaf valid");
    TEST_EXPECT_EQ(pte & 0x0000FFFFFFFFF000ull, page_to_pa(v->pages),
        "PTE PA == the shared vDSO page PA");

    p->state = 2;      // PROC_STATE_ZOMBIE
    proc_free(p);      // vma_drain -> burrow_release_mapping (mapping_count--)
}

void test_vdso_maps_ro_write_faults(void) {
    struct Burrow *v = vdso_clock_burrow();
    TEST_ASSERT(v != NULL, "vdso burrow present");

    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc");

    int rc = burrow_map(p, v, TEST_VA, PAGE_SIZE, VMA_PROT_READ);
    TEST_EXPECT_EQ(rc, 0, "burrow_map RO");

    // A user WRITE to the RO page is rejected -- a hostile/buggy Proc can never
    // corrupt the shared timebase (I-13).
    struct fault_info fi;
    make_read_fi(&fi, TEST_VA, /*is_write=*/true);
    enum fault_result r = userland_demand_page(p, &fi);
    TEST_EXPECT_EQ(r, FAULT_UNHANDLED_USER,
        "write fault on the RO vDSO page is rejected");

    p->state = 2;      // PROC_STATE_ZOMBIE
    proc_free(p);
}
