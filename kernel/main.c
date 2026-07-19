// Thylacine kernel main entry — boot_main().
//
// Called from arch/arm64/start.S after BSS clear and stack setup. Parses
// the DTB QEMU loaded for us, updates the UART base from DTB, prints the
// boot banner per TOOLING.md §10 (load-bearing kernel ABI for the agentic
// tooling), then halts.
//
// At P1-B: DTB is parsed; mem field shows discovered RAM size; UART base
// confirmed via DTB. Remaining fields (kaslr offset, full hardening, SMP
// CPU count) are filled in at P1-C / P1-F / P1-H respectively.
//
// The `Thylacine boot OK` line is the agent's boot-success signal per
// TOOLING.md §10. Do not change without a coordinated update to
// `tools/run-vm.sh`, `tools/agent-protocol.md`, `CLAUDE.md`, and
// `TOOLING.md`.

#include "uart.h"
#include "../arch/arm64/alternatives.h"  // apply_alternatives (W1.5 LSE patcher)
#include "../arch/arm64/hwdebug.h"        // 8a-2: hwdebug_init_cpu (OS-Lock unlock)
#include "../arch/arm64/asid.h"
#include "../arch/arm64/exception.h"
#include "../arch/arm64/gic.h"
#include "../arch/arm64/hwfeat.h"
#include "../arch/arm64/kaslr.h"
#include "../arch/arm64/mmu.h"          // mmu_retire_ttbr0_identity (P3-Bda)
#include "../arch/arm64/timer.h"
#include "../arch/arm64/rtc.h"          // rtc_read_epoch_seconds (LS-K wall clock)
#include "../mm/magazines.h"
#include "../mm/phys.h"
#include "../mm/slub.h"
#include "test/test.h"

#include <thylacine/canary.h>
#include <thylacine/context.h>          // fp_enable_this_cpu (P4-Ic5-FP)

#include <stdint.h>
#include <thylacine/dtb.h>
#include <thylacine/extinction.h>
#include <thylacine/handle.h>
#include <thylacine/irqfwd.h>           // irqfwd_init (P4-Ib R9 F142)
#include <thylacine/joey.h>     // joey_run (P3-F)
#include <thylacine/mmio_handle.h>      // kobj_mmio_init (P4-Ib)
#include <thylacine/pci_handle.h>       // kobj_pci_init (pci-1b)
#include <thylacine/dma_handle.h>       // kobj_dma_init (P4-Ic5b1b)
#include <thylacine/page.h>
#include <thylacine/territory.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/smp.h>
#include <thylacine/thread.h>
#include <thylacine/vma.h>      // vma_init (P3-Da)
#include <thylacine/burrow.h>
#include <thylacine/image.h>    // image_cache_init (REVENANT R-3)
#include <thylacine/vdso.h>
#include <thylacine/weft.h>     // vdso_init (the clock vDSO page, #343)
#include <thylacine/cons.h>     // console_mgr_main (A-4c-1)
#include <thylacine/dev.h>
#include <thylacine/dev9p.h>
#include <thylacine/pipe.h>
#include <thylacine/random.h>  // random_seed_from_virtio (Lazarus W3b)
#include <thylacine/types.h>
#include <thylacine/virtio.h>
#include <thylacine/virtio_pci.h>

#include "../arch/arm64/psci.h"

// From arch/arm64/start.S — DTB physical address handed to us by the
// bootloader (in x0 per the Linux ARM64 boot protocol). Populated
// before boot_main() is called.
extern volatile u64 _saved_dtb_ptr;

// From arch/arm64/start.S — 1 if the kernel was entered at EL2 and
// dropped to EL1 by _real_start, 0 if entered at EL1 directly. Used
// for the el-entry banner diagnostic; surfaces a Pi 5 (or other
// EL2-entry firmware) condition that QEMU virt never exhibits.
extern volatile u64 _entered_at_el2;

// From arch/arm64/start.S — CNTPCT_EL0 captured at the very start of
// _real_start. Used to compute the boot-time banner (P1-I).
extern volatile u64 _boot_start_cntpct;

// From arch/arm64/kernel.ld.
extern char _kernel_start[];
extern char _kernel_end[];

// From arch/arm64/kernel.ld — the boot CPU's static 16 KiB stack. The
// kthread (TID 0, kstack_base==NULL) runs the entire in-kernel test suite
// on this stack, including the deep proc.rfork_stress_1000 chain.
extern char _boot_stack_bottom[];
extern char _boot_stack_top[];

// #806 boot-stack high-water probe. Paint the unused boot stack at boot_main
// entry, scan the deepest sentinel-overwrite after the suite -> measure the
// true stack depth. Discriminates a genuine depth overflow (high-water near
// 16 KiB) from a wild-SP fault landing in the guard (shallow high-water).
#define BOOT_STACK_SENTINEL 0xB007B007B007B007ULL

static void boot_stack_paint(void) {
    u64 sp;
    __asm__ __volatile__("mov %0, sp" : "=r"(sp));
    // Paint only BELOW the live SP (with slack for this frame); the live
    // frames above are untouched. The deep test chain runs below here, so
    // the high-water it leaves is captured.
    u64 *lo = (u64 *)(uintptr_t)_boot_stack_bottom;
    u64 *hi = (u64 *)(uintptr_t)(sp - 512);
    for (u64 *p = lo; p < hi; p++) *p = BOOT_STACK_SENTINEL;
}

#ifdef KERNEL_TESTS
// #806 boot-stack high-water probe: only the gated test suite drives the deep
// stack usage this reports, so it is part of the test scaffolding (#61). The
// painter above stays unconditional (cheap; harmless in the production shape).
static void boot_stack_report(void) {
    u64 lo  = (u64)(uintptr_t)_boot_stack_bottom;
    u64 top = (u64)(uintptr_t)_boot_stack_top;
    u64 deepest = top;  // nothing touched
    for (u64 *p = (u64 *)(uintptr_t)lo; p < (u64 *)(uintptr_t)top; p++) {
        if (*p != BOOT_STACK_SENTINEL) { deepest = (u64)(uintptr_t)p; break; }
    }
    u64 used = top - deepest;
    u64 total = top - lo;
    uart_puts("  boot-stack high-water: ");
    uart_putdec(used);
    uart_puts(" / ");
    uart_putdec(total);
    uart_puts(" B used, margin ");
    uart_putdec(total - used);
    uart_puts(" B to guard; deepest SP ");
    uart_puthex64(deepest);
    uart_puts("\n");
}
#endif /* KERNEL_TESTS */

void boot_main(void);

// A-5a: the "Thylacine boot OK" banner (TOOLING.md section 10 ABI) is printed
// HERE, exactly once, on the first call -- not after joey exits. The session
// shape is "joey persists as init" (IDENTITY-DESIGN.md section 9.9): joey runs
// its boot-test asserts, then calls SYS_BOOT_COMPLETE -> this function, then
// getty-loops /sbin/login (never exiting). joey can no longer ride the
// post-reap print, so the kernel emits the banner on init's explicit signal.
// ONE-SHOT via an atomic exchange (a 2nd SYS_BOOT_COMPLETE is a no-op); the
// SYS_BOOT_COMPLETE handler additionally gates on the caller being
// console-attached so no spawned child can emit a premature banner (-> a false
// test PASS). Returns true iff this call printed the banner.
// File-scope so boot_is_complete() can read it (8a-2c F2: the hwverify verb is a
// boot-only diagnostic -- vestigial once the 8a-2b per-Proc HW install is the real
// mechanism -- so devproc.c refuses it post-boot; that keeps an unprivileged Proc
// from arming the global verify slot and swallowing another Proc's real breakpoint).
static bool g_boot_complete_done;   // BSS false

// boot_is_complete: true once SYS_BOOT_COMPLETE has fired (the boot->session
// boundary; the "Thylacine boot OK" banner). Boot-window-only gates read this.
bool boot_is_complete(void) {
    return __atomic_load_n(&g_boot_complete_done, __ATOMIC_ACQUIRE);
}

bool boot_mark_complete(void) {
    if (__atomic_exchange_n(&g_boot_complete_done, true, __ATOMIC_SEQ_CST))
        return false;
    // TI-4b boot-duration gate: timer_now_ns() is CLOCK_MONOTONIC (ns since the
    // CNTVCT reset == since boot), so at this one-shot SYS_BOOT_COMPLETE point it
    // IS the boot duration. A greppable in-guest number (tools/ci-smp-gate.sh
    // thresholds it) -- the throughput sentinel TI-3 lacked. On HVF CNTVCT tracks
    // wall-clock, so boot-ms ~= the wall-clock seconds the #299 / TI-4 regression
    // was measured in; on TCG it is the virtual-clock elapsed, comparable
    // run-to-run. Printed BEFORE the banner so the "Thylacine boot OK" tooling
    // ABI line (TOOLING.md section 10) stays a clean standalone line.
    uart_puts("boot-ms: ");
    uart_putdec(timer_now_ns() / 1000000ull);
    uart_puts("\n");
    // TI-4d work-conservation summary for the boot itself: how much idle time
    // was spent parked WHILE work was queued elsewhere (a steal/handoff gap).
    // This is THE diagnostic for the tickless boot regression -- a large
    // starved_ms / max_ms means queued-but-unstolen work (rebalance is the
    // lever); ~0 means the boot is genuinely sequential (the cost is per-park
    // overhead, not work-conservation). The /ctl/sched `wc:` line carries the
    // live counters; this is the boot-window snapshot the ci-gate can grep.
    struct sched_wc_stats wc;
    sched_wc_stats(&wc);
    uart_puts("boot-wc: parks=");
    uart_putdec(wc.park_events);
    uart_puts(" idle_ms=");
    uart_putdec(wc.idle_ns / 1000000ull);
    uart_puts(" starved=");
    uart_putdec(wc.starved_events);
    uart_puts(" starved_ms=");
    uart_putdec(wc.starved_ns / 1000000ull);
    uart_puts(" max_ms=");
    uart_putdec(wc.max_starved_ns / 1000000ull);
    // The tickless subset is the regression signal -- starved parks here can run
    // to the backstop (the periodic remainder ends at the next <=1ms tick).
    uart_puts(" | tickless: parks=");
    uart_putdec(wc.tickless_parks);
    uart_puts(" starved=");
    uart_putdec(wc.tickless_starved_events);
    uart_puts(" starved_ms=");
    uart_putdec(wc.tickless_starved_ns / 1000000ull);
    uart_puts(" max_ms=");
    uart_putdec(wc.tickless_max_starved_ns / 1000000ull);
    uart_puts(" wake-ipi=");
    uart_putdec(wc.tickless_ipi_wakes);
    uart_puts(" wake-oneshot=");
    uart_putdec(wc.tickless_oneshot_wakes);
    uart_puts("\n");
    uart_puts("Thylacine boot OK\n");
    return true;
}

void boot_main(void) {
    // #806 probe: paint the unused boot stack first thing, before any deep
    // call uses it. boot_stack_report() (post-suite) reads the high-water.
    boot_stack_paint();

    // P4-Ic5-FP: enable CPACR_EL1.FPEN = 0b11 on the boot CPU before
    // any context switch (and any STP/LDP Q-reg the kernel emits in
    // cpu_switch_context). Secondaries call this from per_cpu_main.
    // Idempotent and < 10 cycles; safe to call first thing.
    fp_enable_this_cpu();

    // P4-Ic-latency: enable EL0 reads of CNTPCT_EL0 via CNTKCTL_EL1.
    // Required for the IRQ-to-userspace latency benchmark + future
    // vDSO clock_gettime. Per-CPU; secondaries set it in per_cpu_main.
    timer_enable_el0_counter_access();

    // Phase 1: parse the DTB (early prints use the fallback PL011 base
    // 0x09000000 from uart.c; if the DTB places PL011 elsewhere,
    // uart_set_base() below will update it before the banner prints).
    bool dtb_ok = dtb_init((paddr_t)_saved_dtb_ptr);

    u64 mem_base = 0, mem_size = 0;
    bool mem_ok = false;
    u64 dtb_uart_base = 0, dtb_uart_size = 0;
    bool uart_ok = false;

    if (dtb_ok) {
        mem_ok  = dtb_get_memory(&mem_base, &mem_size);
        uart_ok = dtb_get_compat_reg("arm,pl011", &dtb_uart_base, &dtb_uart_size);
        if (uart_ok) {
            // P3-Bca: remap PL011 into the kernel vmalloc range. After
            // this, all UART writes go through the vmalloc KVA rather
            // than PA-as-VA via TTBR0 identity. The pre-remap fallback
            // (pl011_base = 0x09000000UL set in uart.c) covered the
            // brief window before this call; once it returns, the
            // active base is the vmalloc KVA returned by mmu_map_mmio.
            uart_remap_to_vmalloc((uintptr_t)dtb_uart_base, (size_t)dtb_uart_size);
        }
    }

    // Banner. Format is kernel ABI per TOOLING.md §10; do not change.
    uart_puts("Thylacine v" THYLACINE_VERSION_STRING "-dev booting...\n");

    uart_puts("  arch: arm64\n");

    uart_puts("  el-entry: ");
    if (_entered_at_el2) {
        uart_puts("EL2 -> EL1 (dropped)\n");
    } else {
        uart_puts("EL1 (direct)\n");
    }

    {
        // CPU count printed pre-bringup; smp_init() (later in boot) brings
        // up the secondaries and reports the online count in the smp: line.
        u32 dtb_cpus = dtb_cpu_count();
        uart_puts("  cpus: ");
        uart_putdec(dtb_cpus ? (u64)dtb_cpus : (u64)1);
        uart_puts(" (DTB-reported; secondaries bring up at smp_init)\n");
    }

    uart_puts("  mem:  ");
    if (mem_ok) {
        uart_putdec(mem_size / (1024UL * 1024UL));
        uart_puts(" MiB at ");
        uart_puthex64(mem_base);
    } else {
        uart_puts("unknown (DTB parse failed or /memory absent)");
    }
    uart_puts("\n");

    uart_puts("  dtb:  ");
    uart_puthex64(_saved_dtb_ptr);
    if (!dtb_ok) {
        uart_puts(" (parse FAILED — fallback UART, no memory info)");
    } else {
        uart_puts(" (parsed)");
    }
    uart_puts("\n");

    uart_puts("  uart: ");
    uart_puthex64((u64)uart_get_base());
    if (uart_ok) {
        uart_puts(" (DTB-driven)");
    } else if (dtb_ok) {
        uart_puts(" (fallback; arm,pl011 absent in DTB)");
    } else {
        uart_puts(" (fallback; DTB unavailable)");
    }
    uart_puts("\n");

    // P1-H + Lazarus W1: detect hardware hardening features. The
    // unconditional set (MMU, W^X, KASLR, vectors, IRQ, canaries,
    // extinction) holds on every target. PAC + BTI are runtime-conditional
    // (HINT-space markers, active only where start.S enabled them); LSE is
    // the LL/SC floor, patched to single-instruction LSE at boot by W1.5
    // where FEAT_LSE is present. The `features:` line below reports what the
    // running CPU actually implements. Set up `g_hw_features` early.
    hw_features_detect();

    // Go IDE Stage 8a-2: per-PE debug bring-up on the boot CPU (secondaries do
    // it in per_cpu_main). Clears the OS Lock (LOCKED at reset — it suppresses
    // debug exceptions) so a guest-programmed EL0 hardware breakpoint can
    // deliver; hw_features_detect already enumerated DFR0's bp/wp counts.
    hwdebug_init_cpu();

    uart_puts("  hardening: MMU+W^X+extinction+KASLR+vectors+IRQ+canaries (unconditional); PAC/BTI/LSE conditional (P1-H; Lazarus W1)\n");

    // Per-feature live-status banner line. Reports what the CPU
    // *implements* (different from the static "we compiled with X"
    // line above). On QEMU virt with -cpu max we expect: PAC,BTI,LSE
    // (MTE is host-emulator-controlled; not always live).
    {
        char buf[64];
        unsigned n = hw_features_describe(buf, sizeof(buf));
        (void)n;
        uart_puts("  features: ");
        uart_puts(buf);
        uart_puts(" (CPU-implemented)\n");
    }

    // Print a 16-bit XOR-fold of the cookie as a presence indicator that
    // varies across boots without leaking the cookie itself. A serial-
    // console attacker who reads the full cookie can forge any canary
    // check (P1-H audit F14); the fold is one-way and non-recoverable
    // — sufficient diagnostic that the cookie was randomized, but
    // insufficient to defeat the protection.
    {
        u64 c = canary_get_cookie();
        u64 fold = (c ^ (c >> 32));
        fold = (fold ^ (fold >> 16)) & 0xFFFFu;
        uart_puts("  canary: initialized (fold ");
        uart_puthex64(fold);
        uart_puts(")\n");
    }


    uart_puts("  kernel base: ");
    uart_puthex64(kaslr_kernel_high_base());
    uart_puts(" (KASLR offset ");
    uart_puthex64(kaslr_get_offset());
    uart_puts(", seed: ");
    uart_puts(kaslr_seed_source_str(kaslr_get_seed_source()));
    uart_puts(")\n");

    // Phase 2: bring up the physical allocator. Reads RAM range from
    // DTB, reserves [low firmware, kernel image, struct page array,
    // DTB blob], pushes the rest onto the buddy.
    if (!phys_init()) {
        extinction("phys_init failed");
    }

    u64 total_pages    = phys_total_pages();
    u64 free_pages_now = phys_free_pages();
    u64 reserved       = phys_reserved_pages();

    uart_puts("  ram: ");
    uart_putdec((total_pages * PAGE_SIZE) / (1024UL * 1024UL));
    uart_puts(" MiB total, ");
    uart_putdec((free_pages_now * PAGE_SIZE) / (1024UL * 1024UL));
    uart_puts(" MiB free, ");
    uart_putdec((reserved * PAGE_SIZE) / 1024UL);
    uart_puts(" KiB reserved (kernel + struct_page + DTB)\n");

    // #808: the buddy direct map is page-mapped (L3) from boot, so no runtime
    // mmu_set_no_access_range demote does a block->table break-before-make.
    uart_puts("  directmap: page-mapped to L3 (");
    uart_putdec((phys_directmap_table_pages() * PAGE_SIZE) / 1024UL);
    uart_puts(" KiB tables; no runtime BBM -- #808)\n");

    // P3-Bda: relocate the DTB blob from its original PA (where QEMU
    // placed it; reachable only via TTBR0 identity) to a buddy-allocated
    // buffer in the kernel direct map. After this call, all DTB walks
    // go through the buffer's direct-map KVA. Done HERE because:
    //   - phys_init has just run; buddy is ready.
    //   - Subsequent code (gic_init at minimum) calls dtb_get_compat_reg,
    //     which must NOT touch the original PA after we retire TTBR0
    //     identity.
    if (!dtb_relocate_to_buffer()) {
        extinction("dtb_relocate_to_buffer failed (DTB > free buddy memory?)");
    }

    // Phase 3: SLUB on top of phys. Standard kmalloc-* caches plus
    // a meta cache for kmem_cache_create. Public API: kmalloc /
    // kfree / kmem_cache_*.
    slub_init();

    // Phase 4: arm the exception vector table (P1-F). After this,
    // synchronous faults route through arch/arm64/exception.c —
    // boot-stack guard region accesses → extinction("kernel stack
    // overflow"); kernel-image permission faults → extinction(
    // "PTE violates W^X"); other sync faults → extinction with
    // ESR/FAR/ELR diagnostic. The IRQ slot is wired by P1-G below.
    // FIQ / SError + lower-EL group remain unexpected (Phase 2
    // wires up the lower-EL group when userspace lands).
    exception_init();

    // Phase 5: bring up the GIC, ARM generic timer, and route the
    // timer IRQ (PPI 11 → INTID 27, the virtual timer) through. After this the kernel
    // receives 1000 Hz ticks; tick observation below confirms the
    // interrupt path is live.
    //
    // gic_init autodetects v2-vs-v3 from DTB; v3 is QEMU virt's
    // default with run-vm.sh's gic-version=3. v2 detection extincts
    // cleanly with a deferred-to-future-chunk diagnostic.
    if (!gic_init()) {
        extinction("gic_init returned false (no extinction caught — bug?)");
    }
    if (!timer_init(1000)) {
        extinction("timer_init failed (CNTFRQ_EL0 = 0 or hz out of range)");
    }
    // LS-K: read the PL031 RTC once + anchor CLOCK_REALTIME to the monotonic
    // counter. Runs after timer_init (g_freq set, timer_now_ns live) and after
    // the MMU/vmalloc are up (gic_init mapped MMIO above), strictly before
    // smp_init (the write-once-before-SMP anchor discipline). Fails soft to a 0
    // epoch (no extinction) -> realtime reads 1970 + uptime if no RTC.
    timer_set_wallclock_anchor(rtc_read_epoch_seconds());
    if (!gic_attach(TIMER_INTID_EL1_VIRT, timer_irq_handler, NULL)) {
        extinction("gic_attach(timer) failed");
    }
    if (!gic_enable_irq(TIMER_INTID_EL1_VIRT)) {
        extinction("gic_enable_irq(timer) failed");
    }
    // #868: make cpu0 a full IPI_RESCHED peer (attach the handler + enable SGI 0
    // on cpu0's redistributor) so a peer's sched_notify_idle_peer can wake cpu0's
    // idle immediately, not only via cpu0's <=1 ms timer. cpu0's GIC redist + CPU
    // interface are already up (gic_init above); attach while IRQs are still
    // masked, unmask below -- the same order as the timer + the secondaries'
    // smp_cpu_ipi_init.
    smp_boot_cpu_ipi_init();
    // Unmask IRQs at PSTATE. The DAIF.I bit gates IRQ delivery;
    // clearing it via daifclr opens the gate. FIQ / SError stay
    // masked at v1.0.
    __asm__ __volatile__("msr daifclr, #2" ::: "memory");

    uart_puts("  gic:  v");
    uart_putdec((u64)gic_version());
    uart_puts(" dist PA=");
    uart_puthex64(gic_dist_pa());
    uart_puts(" KVA=");
    uart_puthex64(gic_dist_base());
    if (gic_version() == GIC_VERSION_V2) {
        // v2 has no redistributor; report the GICC CPU-interface frame.
        uart_puts(" cpuif PA=");
        uart_puthex64(gic_cpu_iface_pa());
        uart_puts(" KVA=");
        uart_puthex64(gic_cpu_iface_base());
    } else {
        uart_puts(" redist PA=");
        uart_puthex64(gic_redist_pa());
        uart_puts(" KVA=");
        uart_puthex64(gic_redist_base());
    }
    uart_puts("\n");

    uart_puts("  timer: ");
    uart_putdec(timer_get_freq() / 1000UL);
    uart_puts(" kHz freq, 1000 Hz tick (virtual timer, PPI 11 / INTID 27)\n");

    // Phase 6: process / thread bring-up (P2-A entry).
    //
    // territory_init creates the territory SLUB cache + kproc's empty Territory
    // (P2-Eb). Must run BEFORE proc_init so kproc has a territory to assign.
    //
    // proc_init creates kproc (PID 0) — the kernel's own process. It
    // owns the kernel address space, will own the kernel handle table
    // (Phase 2), and parents the kernel threads. thread_init creates
    // kthread (TID 0) and parks it in TPIDR_EL1 as the boot CPU's
    // current thread.
    //
    // After this point current_thread() is valid; cpu_switch_context
    // can save/load thread state; thread_create + thread_switch work.
    // The actual scheduler (EEVDF) lands at P2-B.
    // P2-Fc: handle_init creates the handle-table SLUB cache. Must run
    // BEFORE proc_init since proc_init allocates a HandleTable for kproc.
    // P2-Fd: burrow_init creates the BURROW SLUB cache. Order doesn't matter
    // wrt proc_init (no kproc-BURROW ownership at v1.0); placed near
    // handle_init for grouping.
    // P3-Ba / RW-1 B-F1: asid_init initializes the rolling-ASID allocator
    // (generation + bitmap + per-CPU active/reserved/flush_pending; reads the
    // ASID width). No SLUB dependency (state lives in BSS). Runs after the MMU
    // is up (asid_hw_bits reads ID_AA64MMFR0_EL1) and before any context switch
    // calls asid_resolve.
    territory_init();
    handle_init();
    burrow_init();
    // vDSO clock page: one kernel-owned BURROW_TYPE_ANON page, mapped RO into
    // every exec'd Proc so native userspace reads CLOCK_MONOTONIC/_REALTIME
    // without a SYS_CLOCK_GETTIME trap (docs/VDSO-DESIGN.md, #343). Runs AFTER
    // burrow_init (it burrow_create_anons the page), timer_init (freq known,
    // line 398), and the boot wall anchor (offset set, line 406) so the page
    // seeds correct values. Best-effort: on OOM the page is absent + readers
    // fall back to the syscall.
    vdso_init();
    image_cache_init();    // REVENANT R-3: the qid-keyed shared-text Image cache (BSS-backed; after burrow_init)
    vma_init();
    asid_init();
    // P4-Ib: kobj_mmio_init sets up the MMIO claim-tracking table.
    // Independent of proc/thread/sched bring-up; placed here for
    // grouping with handle_init.
    kobj_mmio_init();
    // P4-Ic5b1b: kobj_dma_init sets up the DMA-handle subsystem.
    // No state to allocate (buddy is the claim layer); the call is
    // a one-time init guard + boot-banner diagnostic. Must run AFTER
    // phys_init (alloc_pages must be live by the time the first
    // kobj_dma_create fires).
    kobj_dma_init();
    proc_init();
    thread_init();
    sched_init(0);                              // boot CPU's per-CPU sched state

    // P4-A: bring up the device subsystem. spoor_init allocates the
    // Spoor SLUB cache; dev_register(&devnone) seeds the bestiary; the
    // bestiary walk calls each ->init() once (no-op for devnone at v1.0;
    // P4-B's cons / null / zero / random will hook here). Order: after
    // sched_init so that any future driver-side dev->init that allocates
    // a Spoor + walks has a working scheduler context.
    dev_init();

    // P5-attach-dev: register the dev9p proxy Dev. After dev_init so
    // dev_register's bestiary array is initialized. The 9P stack itself
    // (codec / session / transport / client) needs no init — each
    // p9_client is caller-allocated and initialized per-mount.
    dev9p_init();
    // net-6b-2b: the dev9p.poll readiness registry + lock + kthread rendez. Just
    // initializes state here; the poll-pump kthread is spawned later (after
    // sched + kproc are up, alongside console_mgr).
    dev9p_poll_init();

    // P5-pipe: register devpipe + allocate SLUB caches for the pipe
    // ring + endpoint structs. After dev9p_init for grouping with the
    // 9P-adjacent registrations; both are post-dev_init since they
    // need spoor_init to have run.
    pipe_init();

    // P4-F: VirtIO MMIO transport probe. Walks DTB for "virtio,mmio"
    // nodes; maps each MMIO range; reads transport identity. Order:
    // after dev_init since virtio_init prints to UART (cons must be
    // up) and after slub_init since virtqueue_create uses kmalloc.
    virtio_init();

    // Lazarus W3b: pull strong entropy from the kernel virtio-rng device
    // now that the MMIO transport is probed. Upgrades the CSPRNG from its
    // boot-time DTB/cntpct seed (devrandom_init, during dev_init) to real
    // host entropy. Best-effort: if no RNG device is attached, the chacha
    // pool keeps the DTB seed and this prints "unavailable".
    {
        size_t rng_bytes = random_seed_from_virtio();
        u64 rng_spin = 0;
        const char *rng_reason = kern_random_pull_diag(&rng_spin);
        uart_puts("  random: virtio-rng reseed ");
        if (rng_bytes) {
            uart_puts("OK (");
            uart_putdec((u64)rng_bytes);
            uart_puts(" bytes mixed, polled ");
            uart_putdec(rng_spin);
            uart_puts(" iters)\n");
        } else {
            // #188: report WHICH site failed -- the historic "no RNG device"
            // text was misleading (the device is usually present; a transient
            // poll-timeout is the real mode). The chacha pool keeps the boot
            // DTB/cntpct seed; on a target with no RNDR this means readiness
            // (kern_random_seeded) stays false -- the fail-closed cascade.
            uart_puts("FAILED: ");
            uart_puts(rng_reason);
            uart_puts(" (polled ");
            uart_putdec(rng_spin);
            uart_puts(" iters; chacha keeps the boot seed)\n");
        }
    }

    // P4-Ib R9 F142: reserve kernel-owned INTIDs in g_intid_claimed
    // so that subsequent kobj_irq_create (via SYS_IRQ_CREATE syscall
    // from a userspace driver) can't accidentally clobber the timer
    // or IPI_RESCHED handler slots. Runs AFTER timer_init (the virtual-timer
    // PPI is attached) but BEFORE smp_init (RW-7 R1-F4: irqfwd_init pre-claims
    // SGI 0 = IPI_RESCHED here; smp_init attaches its handler later via
    // gic_attach, which bypasses the claim layer -- the reservation is
    // order-independent) and BEFORE any user-driven kobj_irq_create.
    irqfwd_init();

    // P4-Ic2 R10 F154: reserve kernel-owned MMIO PA ranges in
    // g_mmio_claims so that subsequent kobj_mmio_create (via
    // SYS_MMIO_CREATE / SYS_MMIO_MAP from a userspace driver) can't
    // claim PA ranges that the kernel uses directly (GIC, PL011,
    // ECAM, VirtIO MMIO). Mirrors irqfwd_init's pattern. Must run
    // AFTER kobj_mmio_init + dtb_init + after the kernel has mapped
    // its own MMIO (so we know the full claim set).
    kobj_mmio_reserve_kernel_ranges();

    // P4-H: VirtIO PCIe enumeration. Probes the DTB-described PCIe
    // ECAM (pci-host-ecam-generic), maps bus 0's config space via
    // mmu_map_mmio, walks (dev, fn) on bus 0, and records every
    // VirtIO PCI device (vendor 0x1AF4 + device IDs 0x1000..0x107F).
    // Silent skip when no PCIe root is present in DTB.
    virtio_pci_init();

    // pci-1b: the KObj_PCI claim subsystem. Must follow virtio_pci_init (it
    // claims from the enumerated g_virtio_pci_devs[]). No DTB/HW access of its
    // own -- BARs are assigned lazily from dtb_pci_mem_window on the first claim.
    kobj_pci_init();

    // W1.5: boot-time LSE alternatives-patching. Rewrites the LL/SC atomic
    // sites (the spinlock test-and-set, the Spoor/SrvConn refcounts, the
    // scheduler steal-rotate) to single-instruction LSE on FEAT_LSE cores.
    // Runs HERE -- after hw_features_detect + the MMU/allocator are up, and
    // strictly BEFORE smp_init -- so it executes single-CPU (no peer runs a
    // site mid-patch; secondaries start later with cold I-caches and fetch
    // already-patched bytes). No-op on a non-LSE core (A72). It self-modifies
    // .text through a transient RW-not-X alias, so W^X / I-12 holds
    // throughout (PORTABILITY.md 4.5; audit-trigger surface, ARCH 25.4).
    apply_alternatives();

    // SMP secondary bring-up (P2-Ca). Reads /psci/method, brings up
    // each /cpus/cpu@N (N>0) via PSCI_CPU_ON pointing at the asm
    // trampoline secondary_entry. Each secondary flips its online
    // flag + parks at WFI; primary waits with timeout. Failures are
    // logged but do not abort the boot.
    bool psci_ok = psci_init();
    unsigned secondaries_online = psci_ok ? smp_init() : 0;
    if (!psci_ok && dtb_cpu_count() > 1) {
        // Multi-CPU DTB but no PSCI — UP fallback. smp_init still
        // marks boot online + caches the count.
        smp_init();
    }

    // P3-Bda: retire the TTBR0 identity map. By now, all secondaries
    // have completed their secondary_entry trampoline (which uses
    // TTBR0 identity for stack until SP_EL0 is re-anchored to high
    // VA post-mmu_program_this_cpu). After this call, any low-VA
    // dereference (PA-as-VA-via-TTBR0 idiom) faults at the L2 invalid
    // entry. All known callers were migrated:
    //   - UART → vmalloc (P3-Bca uart_remap_to_vmalloc).
    //   - GIC → vmalloc (P3-Bca gic_init via mmu_map_mmio).
    //   - kstack → direct-map (P3-Bca kernel/thread.c).
    //   - struct_pages → direct-map (P3-Bda mm/phys.c::phys_init).
    //   - DTB → buffer in direct map (P3-Bda dtb_relocate_to_buffer).
    //   - boot stack SP_EL0 → high VA (P3-Bda start.S step 8.6).
    //   - secondary boot stack SP_EL0 → high VA (P3-Bda start.S
    //     secondary_entry post-mmu_program_this_cpu).
    //
    // Boot-CPU TTBR0_EL1 still points at l0_ttbr0 (now with empty L2s);
    // P3-Bdb will swap it to per-Proc page-tables on context switch.
    mmu_retire_ttbr0_identity();

    uart_puts("  smp:  ");
    uart_putdec((u64)smp_cpu_online_count());
    uart_puts("/");
    uart_putdec((u64)smp_cpu_count());
    uart_puts(" cpus online (boot + ");
    uart_putdec((u64)secondaries_online);
    uart_puts(" secondaries via PSCI ");
    {
        dtb_psci_method_t m = dtb_psci_method();
        uart_puts(m == DTB_PSCI_HVC ? "HVC" : m == DTB_PSCI_SMC ? "SMC" : "NONE");
    }
    uart_puts(")\n");

    uart_puts("  exception: uniform EL1h (SPSel=1; exception frames on the per-thread kernel stack)\n");

    uart_puts(gic_version() == GIC_VERSION_V2
              ? "  ipi:  GICv2 SGIs live (IPI_RESCHED=SGI 0 via GICD_SGIR; banked SGI/PPI + IRQ unmask on secondaries)\n"
              : "  ipi:  GICv3 SGIs live (IPI_RESCHED=SGI 0; per-CPU GIC redist + IRQ unmask on secondaries)\n");

    uart_puts("  kproc:   pid=");
    uart_putdec((u64)kproc()->pid);
    uart_puts(" threads=");
    uart_putdec((u64)kproc()->thread_count);
    uart_puts("\n");

    uart_puts("  kthread: tid=");
    uart_putdec((u64)kthread()->tid);
    uart_puts(" state=RUNNING (current_thread = kthread)\n");

    // SMP redesign (ARCH §8.4.2): allocate cpu0's idle thread -- distinct from
    // kthread, an ordinary CPU-pinned in-tree BAND_IDLE thread on a dedicated BSS
    // stack -- and ready() it into cpu0's run_tree[IDLE] via
    // sched_install_bootcpu_idle. Ordinary pick_next dispatches it whenever
    // kthread (or any cpu0 thread) blocks with no other runnable work: idle WFI
    // loops until an IRQ makes some thread RUNNABLE, then its own sched() picks
    // that thread up. This retires the old off-tree real-kstack g_bootcpu_idle +
    // its deadlock-path dispatch (the #860 root cause: a real kstack made it
    // stealable if it ever entered a tree).
    //
    // Must be installed BEFORE test_run_all() since rendez tests cause kthread to
    // sleep; before the idle is in cpu0's tree, such a sleep with no runnable
    // peer would hit the (now-defensive, structurally-unreachable) deadlock
    // extinction. By construction once installed, cpu0's idle is always either
    // current or in run_tree[IDLE], so pick_next always finds it.
    {
        struct Thread *bootcpu_idle =
            thread_create_bootcpu_idle(bootcpu_idle_main, smp_bootcpu_idle_stack_top());
        if (!bootcpu_idle) extinction("boot_main: bootcpu_idle alloc failed");
        sched_install_bootcpu_idle(bootcpu_idle);
    }

    // HMP foundation (#864, ARCH §8.4.4): parse + normalize per-CPU capacity
    // from the DTB's capacity-dmips-mhz (composes with I-15). Once, on cpu0,
    // after smp_init so dtb_cpu_count is final and every CpuSched slot exists.
    // Uniform on QEMU virt / RPi (no capacity-dmips-mhz declared) -> every CPU
    // SCHED_CAPACITY_SCALE, hetero=0, and the capacity-aware placement policy
    // is inert (select_target_cpu returns the prev/waking CPU, so ready() keeps
    // the pre-#864 behavior exactly). A declared-heterogeneous DTB activates
    // the capacity bias. The placement LOGIC is unit-tested against a synthetic
    // asymmetric DTB; the empirical EAS tuning is deferred to real hetero HW.
    sched_capacity_init();
    uart_puts("  sched:   topology ");
    uart_puts(sched_topology_hetero() ? "HETEROGENEOUS caps=[" : "homogeneous caps=[");
    {
        unsigned ncpu = smp_cpu_count();
        if (ncpu > DTB_MAX_CPUS) ncpu = DTB_MAX_CPUS;
        for (unsigned i = 0; i < ncpu; i++) {
            if (i) uart_puts(" ");
            uart_putdec((u64)sched_cpu_capacity(i));
        }
    }
    uart_puts("] (scale=1024; HMP placement inert when homogeneous)\n");

    // A-4c-1: kernel UART console RX + the console_mgr kthread. Unmask the
    // PL011 RX IRQ, route it to the console RX handler (fills the cons input
    // ring; Ctrl-C -> a deferred `interrupt` note to the console owner), and
    // spawn the console_mgr kthread that services deferred console actions in
    // process context (the RX IRQ handler is wakeup-only, since notes_post is
    // not IRQ-safe). On the kernel UART Dev (dc='c'); the userspace
    // virtio-input path is separate (ARCH §17.1). The UART SPI is reserved
    // against userspace SYS_IRQ_CREATE claim in irqfwd_init (above). Spawned
    // after sched_init + kproc + bootcpu_idle so the kthread can block + be
    // scheduled; RX bytes arriving before it runs simply sit in the ring.
    uart_rx_init();
    if (!gic_attach(UART_INTID_PL011, uart_rx_handler, NULL))
        extinction("boot_main: gic_attach(uart-rx) failed");
    if (!gic_enable_irq(UART_INTID_PL011))
        extinction("boot_main: gic_enable_irq(uart-rx) failed");
    {
        struct Thread *console_mgr = thread_create(kproc(), console_mgr_main);
        if (!console_mgr) extinction("boot_main: console_mgr alloc failed");
        ready(console_mgr);
    }
    // net-6b-2b: the global dev9p.poll-pump kthread. Drives the 9P elected reader
    // for outstanding readiness probes (a poll() caller parks, so nothing else
    // pumps the reader) + walks the poll hook lists in process context. Parked
    // until the first /net poll submits a probe; no cost when nothing polls.
    {
        struct Thread *poll_pump = thread_create(kproc(), dev9p_poll_pump_main);
        if (!poll_pump) extinction("boot_main: dev9p_poll_pump alloc failed");
        ready(poll_pump);
    }

    // G-3 (R2-F3): the orphaned-weave reaper -- force-reclaims a dead
    // compositor's stale client weave mappings after a bounded grace
    // (TAPESTRY.md section 18.12; the tapestry_present.tla ServerDeath leg's
    // kernel half). Parks indefinitely while no weave binding is registered.
    weft_reap_init();
    {
        struct Thread *weft_reaper = thread_create(kproc(), weft_reaper_main);
        if (!weft_reaper) extinction("boot_main: weft_reaper alloc failed");
        ready(weft_reaper);
    }
    uart_puts("  cons:  UART RX live (INTID ");
    uart_putdec((u64)UART_INTID_PL011);
    uart_puts("); console_mgr kthread up\n");

    uart_puts("  sched:   bands=3 (INTERACTIVE/NORMAL/IDLE) runnable=");
    uart_putdec((u64)sched_runnable_count());
    uart_puts(" (counts console_mgr work; per-CPU idles are BAND_IDLE, excluded)\n");

    // In-kernel test harness. Runs every test in g_tests[] (kaslr
    // mix64 avalanche, DTB chosen seed presence, refactored phys
    // alloc smoke, refactored slub kmem smoke). Tests cover stable
    // leaf APIs only — internal data-structure invariants are
    // tested implicitly via the smoke flows so we don't pin
    // ourselves to evolving subsystem layouts. Future host-side
    // sanitizer matrix lands at P1-I.
#ifdef KERNEL_TESTS
    // #58: the spawn tests resolve the binary through the caller's namespace
    // (exec_resolve_from_namespace -> stalk), so the test Proc (kproc) needs a
    // root_spoor. Root it at devramfs BEFORE the suite; joey_run idempotently
    // re-roots it later for the boot chain. NOTE (#58 audit F5): kproc's root is
    // now devramfs for the WHOLE suite -- no test may assume an unrooted kproc or
    // FROM_ROOT == -1 (pre-#58 kproc was already devramfs-rooted for joey, so
    // there is no behavior delta; this just moves the root earlier).
    joey_root_kproc_at_devramfs();
    uart_puts("  tests:\n");
    test_run_all();
    uart_puts("  tests: ");
    uart_putdec(test_passed());
    uart_puts("/");
    uart_putdec(test_total());
    if (test_all_passed()) {
        uart_puts(" PASS");
        if (test_soft_warns() > 0) {
            // Host-fragility budgets (e.g. the irq-bench QEMU latency
            // ceiling) tripped without failing the suite -- surfaced so a
            // throttled CI host is visible, not silently swallowed.
            uart_puts(" (");
            uart_putdec(test_soft_warns());
            uart_puts(" soft-warn)");
        }
        uart_puts("\n");
    } else {
        uart_puts(" FAIL\n");
        extinction("kernel test suite failed");
    }

    // #806 probe: report the boot-stack high-water left by the suite.
    boot_stack_report();
#else
    // Production shape (#61, KERNEL_TESTS=OFF): the in-kernel suite is not
    // compiled. Emit a one-line marker so a production boot is self-identifying.
    // The "tests:" PASS line is informational -- only "Thylacine boot OK" + the
    // "EXTINCTION:" prefix are the binding tooling ABI (TOOLING.md §10).
    uart_puts("  tests: DISABLED (KERNEL_TESTS=OFF production build)\n");
#endif

    // Wait for ticks to confirm IRQ delivery, then print the count.
    // 5 ticks at 1000 Hz = 5 ms — enough to demonstrate the path is
    // live without elongating the boot. WFI inside the loop puts the
    // CPU to sleep until the next IRQ arrives.
    timer_busy_wait_ticks(5);
    uart_puts("  ticks: ");
    uart_putdec(timer_get_ticks());
    uart_puts(" (kernel breathing)\n");

    // #788: count of thread_free calls that had to wait out an in-flight
    // cpu_switch_context away from the victim (SLEEPING/EXITING but still
    // on_cpu). Silent at 0; a non-zero value flags that the #788 UAF window
    // was hit on this boot (host-stalled secondary; common only under E-core
    // contention) AND the on_cpu gate handled it instead of corrupting.
    {
        extern u64 thread_free_oncpu_waits(void);
        u64 waits = thread_free_oncpu_waits();
        if (waits > 0) {
            uart_puts("  thread_free on_cpu-waits: ");
            uart_putdec(waits);
            uart_puts(" (#788 gate engaged -- race window hit + handled)\n");
        }
    }

    // Boot-time banner (P1-I; ROADMAP §4.2 < 500 ms exit criterion).
    // Read CNTPCT now and subtract the start-of-_real_start capture to
    // get elapsed counter ticks; convert to microseconds via CNTFRQ.
    // Includes the 5-tick busy-wait above (~5 ms), which is part of
    // the boot path's normal cost.
    {
        u64 now;
        __asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(now));
        u64 elapsed_ticks = now - _boot_start_cntpct;
        u64 freq = (u64)timer_get_freq();         // CNTFRQ_EL0
        u64 us = (elapsed_ticks * 1000000UL) / freq;
        uart_puts("  boot-time: ");
        uart_putdec(us / 1000);
        uart_puts(".");
        uart_putdec((us % 1000) / 100);
        uart_puts(" ms (target < 500 ms per VISION §4)\n");
    }

    uart_puts("  phase: " THYLACINE_PHASE_STRING "\n");

    // P1-I deliberate-fault test (no-op in production builds). When
    // THYLACINE_FAULT_TEST is set, this triggers exactly one
    // hardening protection (canary smash / W^X / BTI); the resulting
    // EXTINCTION: line is what tools/test-fault.sh checks for.
    extern void fault_test_run(void);
    fault_test_run();

    // P3-G: enable cross-CPU work-stealing IPIs from ready()/wakeup().
    // Off during in-kernel tests so they keep their UP-like single-CPU
    // assumptions; on for /init and beyond so secondaries actually
    // pick up runnable work instead of sitting in WFI indefinitely.
    // Closes R5-H F78. See sched.c::sched_notify_idle_peer.
    sched_set_notify_enabled(true);

    // #810: enable preemptive scheduling on secondary CPUs (each arms its own
    // per-CPU generic timer). Like the notify gate above, this is OFF during
    // the UP-like in-kernel tests -- a secondary self-waking on a timer tick
    // and stealing a test thread surfaced as `thread_free of RUNNING thread`
    // in scheduler.preemption_smoke. From here on, every CPU gets the
    // preemptive tick, so a CPU-bound thread on a secondary can no longer
    // monopolize it (the exitgroup boot hang) -- invariants I-8 / I-17.
    smp_enable_secondary_preemption();

    // P3-F: /init is the first userspace process. v1.0 embedded blob —
    // prints "hello\n" via SYS_PUTS and exits via SYS_EXITS(0). Validates
    // the kernel→exec→userspace→syscall→kernel chain end-to-end in the
    // production boot path (no test-harness scaffolding). Trip-hazard
    // #157 (second-userspace-exec hang) means we run /init exactly once
    // per boot and don't follow it with another userspace exec; the
    // prior `userspace.exec_exits_ok` test was retired in favor of this.
    // Failure (rfork OOM / exec_setup error / non-zero exit before the
    // boot-complete signal) extincts inside joey_run.
    //
    // A-5a: joey is now the long-running session supervisor. On SUCCESS it
    // calls SYS_BOOT_COMPLETE (-> boot_mark_complete prints the banner) and
    // then NEVER exits (it getty-loops /sbin/login), so joey_run's wait_pid
    // blocks forever and this call does not return. On FAILURE joey exits
    // non-zero before the banner -> joey_run extincts. Either way the banner
    // is no longer printed here -- it rides SYS_BOOT_COMPLETE while joey is
    // alive (IDENTITY-DESIGN.md section 9.9).
    joey_run();

    // Reached only if joey exits cleanly WITHOUT persisting (not a v1.0 path;
    // the banner already printed via SYS_BOOT_COMPLETE if joey got that far).
    // boot_main() must not return; start.S falls through to _torpor, but be
    // explicit.
    extern void _torpor(void) __attribute__((noreturn));
    _torpor();
}
