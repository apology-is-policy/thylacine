// vdso.c — the monotonic-clock vDSO page (the read-only timekeeping page
// native userspace reads instead of trapping into SYS_CLOCK_GETTIME).
//
// Binding design: docs/VDSO-DESIGN.md. ABI: <thylacine/vdso.h>.
//
// One kernel-owned BURROW_TYPE_ANON page, populated once at boot and mapped
// READ-ONLY into every Proc at exec (the same physical page for all — the Linux
// shared-vvar model; there is no per-Proc timekeeping state). The Burrow is held
// forever (handle_count stays 1), so the page is never freed even as Procs
// map/unmap it; a Proc's mapping_count rises at exec and falls at teardown via
// the ordinary VMA lifecycle.
//
// Userspace reads CNTVCT_EL0 directly (already EL0-enabled,
// CNTKCTL_EL1.EL0VCTEN) plus this page's `freq` and `wall_offset_ns`, and
// replicates timer_now_ns()'s split arithmetic — no syscall, no seqlock. The
// page is best-effort: if vdso_init OOMs, vdso_clock_burrow() returns NULL,
// exec omits the AT_VDSO_CLOCK auxv entry, and every reader falls back to the
// syscall.

#include <thylacine/vdso.h>
#include <thylacine/burrow.h>
#include <thylacine/page.h>      // PAGE_SIZE, page_to_pa, pa_to_kva
#include <thylacine/types.h>

#include "../arch/arm64/timer.h" // timer_get_freq, timer_wallclock_offset_ns_now

// The kernel-owned Burrow wrapping the shared page (handle_count==1 forever) +
// the kernel VA of the page (the vdso_publish_wall mirror target). Both NULL
// until a successful vdso_init; NULL forever if vdso_init OOMs.
static struct Burrow     *g_vdso_burrow;
static struct vdso_clock *g_vdso_page;

void vdso_init(void) {
    if (g_vdso_burrow) return;                  // idempotent

    struct Burrow *v = burrow_create_anon(PAGE_SIZE);
    if (!v) return;                             // best-effort: readers fall back

    // burrow_create_anon allocates the page KP_ZERO, so reserved[] is already 0
    // and the struct sits at the head of the page (offset 0).
    struct vdso_clock *pg = (struct vdso_clock *)pa_to_kva(page_to_pa(v->pages));

    pg->freq           = (u64)timer_get_freq();          // CNTFRQ Hz (write-once)
    pg->wall_offset_ns = timer_wallclock_offset_ns_now(); // the boot anchor's offset
    pg->version        = VDSO_CLOCK_VERSION;
    // Publish magic LAST with a release barrier: a reader that observes the
    // valid magic is guaranteed to see the fully-populated fields above. (Boot
    // is single-CPU, but the discipline is free and documents the contract.)
    __atomic_store_n(&pg->magic, VDSO_CLOCK_MAGIC, __ATOMIC_RELEASE);

    g_vdso_page   = pg;
    g_vdso_burrow = v;
}

void vdso_publish_wall(u64 wall_offset_ns) {
    struct vdso_clock *pg = g_vdso_page;
    if (!pg) return;                            // null-safe (pre-init / OOM)
    __atomic_store_n(&pg->wall_offset_ns, wall_offset_ns, __ATOMIC_RELAXED);
}

struct Burrow *vdso_clock_burrow(void) {
    return g_vdso_burrow;
}
