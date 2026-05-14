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
#include "../arch/arm64/asid.h"
#include "../arch/arm64/exception.h"
#include "../arch/arm64/gic.h"
#include "../arch/arm64/hwfeat.h"
#include "../arch/arm64/kaslr.h"
#include "../arch/arm64/mmu.h"          // mmu_retire_ttbr0_identity (P3-Bda)
#include "../arch/arm64/timer.h"
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
#include <thylacine/dma_handle.h>       // kobj_dma_init (P4-Ic5b1b)
#include <thylacine/page.h>
#include <thylacine/territory.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/smp.h>
#include <thylacine/thread.h>
#include <thylacine/vma.h>      // vma_init (P3-Da)
#include <thylacine/burrow.h>
#include <thylacine/dev.h>
#include <thylacine/dev9p.h>
#include <thylacine/pipe.h>
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

void boot_main(void);

void boot_main(void) {
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

    // P1-H: detect hardware-supported hardening features. The compile-
    // time flags (canaries, PAC, BTI, LSE) are always emitted; this
    // detection tells the operator which of those will actually be
    // enforced by the running CPU. Set up `g_hw_features` early so the
    // banner can report it accurately.
    hw_features_detect();

    uart_puts("  hardening: MMU+W^X+extinction+KASLR+vectors+IRQ+canaries+PAC+BTI+LSE (P1-H)\n");

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
    // timer IRQ (PPI 14 → INTID 30) through. After this the kernel
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
    if (!gic_attach(TIMER_INTID_EL1_PHYS_NS, timer_irq_handler, NULL)) {
        extinction("gic_attach(timer) failed");
    }
    if (!gic_enable_irq(TIMER_INTID_EL1_PHYS_NS)) {
        extinction("gic_enable_irq(timer) failed");
    }
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
    uart_puts(" redist PA=");
    uart_puthex64(gic_redist_pa());
    uart_puts(" KVA=");
    uart_puthex64(gic_redist_base());
    uart_puts("\n");

    uart_puts("  timer: ");
    uart_putdec(timer_get_freq() / 1000UL);
    uart_puts(" kHz freq, 1000 Hz tick (PPI 14 / INTID 30)\n");

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
    // P3-Ba: asid_init initializes the ASID allocator. No SLUB dependency
    // (state lives in BSS). Must run BEFORE any future code that calls
    // asid_alloc; placed early so the order is unambiguous as Phase 3+
    // sub-chunks add per-Proc TTBR0 alloc paths.
    territory_init();
    handle_init();
    burrow_init();
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

    // P4-Ib R9 F142: reserve kernel-owned INTIDs in g_intid_claimed
    // so that subsequent kobj_irq_create (via SYS_IRQ_CREATE syscall
    // from a userspace driver) can't accidentally clobber the timer
    // or IPI_RESCHED handler slots. Must run AFTER timer_init +
    // smp_init (their gic_attach calls bypass the claim layer) and
    // BEFORE any user-driven kobj_irq_create.
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

    uart_puts("  exception: per-CPU SP_EL1 (");
    uart_putdec((u64)EXCEPTION_STACK_SIZE);
    uart_puts(" B/CPU; SPSel=0 kernel mode)\n");

    uart_puts("  ipi:  GICv3 SGIs live (IPI_RESCHED=SGI 0; per-CPU GIC redist + IRQ unmask on secondaries)\n");

    uart_puts("  kproc:   pid=");
    uart_putdec((u64)kproc()->pid);
    uart_puts(" threads=");
    uart_putdec((u64)kproc()->thread_count);
    uart_puts("\n");

    uart_puts("  kthread: tid=");
    uart_putdec((u64)kthread()->tid);
    uart_puts(" state=RUNNING (current_thread = kthread)\n");

    // P4-Ic6-impl (R12-sched): allocate the boot CPU's dedicated idle
    // thread, distinct from kthread, AND register it as the boot CPU's
    // sched-deadlock-path fallback via sched_set_bootcpu_idle. NOT
    // ready()'d into the run tree — preempt's pick_next would otherwise
    // switch to it at SpSel=1 (preempt is hardware-set to SpSel=1 on
    // entry), and cpu_switch_context's `mov sp` would clobber SP_EL1
    // (= exception stack) with bootcpu_idle's kstack. Instead, sched()
    // explicitly switches to it ONLY in the deadlock path — which fires
    // exclusively from VOLUNTARY sched() callers (sleep, exits) at
    // SpSel=0, where cpu_switch_context naturally writes SP_EL0. SP_EL1
    // stays as the per-CPU exception stack throughout.
    //
    // Scenarios where the deadlock path now resolves cleanly (was ELE
    // pre-fix):
    //   - kthread calls wait_pid on a hardware-IRQ-blocked child driver
    //     (e.g., test_virtio_blk_probe); kthread → SLEEPING, child →
    //     SLEEPING; sched() picks bootcpu_idle as the fallback; idle
    //     WFI loops; IRQ wakes child; eventually wait_pid wakes kthread.
    //   - Any voluntary sleep where no peer is runnable system-wide.
    //
    // Must be initialized BEFORE test_run_all() since rendez tests
    // cause kthread to sleep, and an unlucky pre-bootcpu_idle sleep
    // would hit the now-narrowed deadlock extinction.
    {
        struct Thread *bootcpu_idle = thread_create(kproc(), bootcpu_idle_main);
        if (!bootcpu_idle) extinction("boot_main: bootcpu_idle alloc failed");
        bootcpu_idle->band = SCHED_BAND_IDLE;
        sched_set_bootcpu_idle(bootcpu_idle);
    }

    uart_puts("  sched:   bands=3 (INTERACTIVE/NORMAL/IDLE) runnable=");
    uart_putdec((u64)sched_runnable_count());
    uart_puts(" (0 expected pre-test; bootcpu_idle reserved off-tree)\n");

    // In-kernel test harness. Runs every test in g_tests[] (kaslr
    // mix64 avalanche, DTB chosen seed presence, refactored phys
    // alloc smoke, refactored slub kmem smoke). Tests cover stable
    // leaf APIs only — internal data-structure invariants are
    // tested implicitly via the smoke flows so we don't pin
    // ourselves to evolving subsystem layouts. Future host-side
    // sanitizer matrix lands at P1-I.
    uart_puts("  tests:\n");
    test_run_all();
    uart_puts("  tests: ");
    uart_putdec(test_passed());
    uart_puts("/");
    uart_putdec(test_total());
    if (test_all_passed()) {
        uart_puts(" PASS\n");
    } else {
        uart_puts(" FAIL\n");
        extinction("kernel test suite failed");
    }

    // Wait for ticks to confirm IRQ delivery, then print the count.
    // 5 ticks at 1000 Hz = 5 ms — enough to demonstrate the path is
    // live without elongating the boot. WFI inside the loop puts the
    // CPU to sleep until the next IRQ arrives.
    timer_busy_wait_ticks(5);
    uart_puts("  ticks: ");
    uart_putdec(timer_get_ticks());
    uart_puts(" (kernel breathing)\n");

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

    // P3-F: /init is the first userspace process. v1.0 embedded blob —
    // prints "hello\n" via SYS_PUTS and exits via SYS_EXITS(0). Validates
    // the kernel→exec→userspace→syscall→kernel chain end-to-end in the
    // production boot path (no test-harness scaffolding). Trip-hazard
    // #157 (second-userspace-exec hang) means we run /init exactly once
    // per boot and don't follow it with another userspace exec; the
    // prior `userspace.exec_exits_ok` test was retired in favor of this.
    // Failure (rfork OOM / exec error / non-zero exit_status) extincts.
    joey_run();

    uart_puts("Thylacine boot OK\n");

    // boot_main() must not return. start.S has a fallthrough to _torpor
    // for safety, but be explicit here too.
    extern void _torpor(void) __attribute__((noreturn));
    _torpor();
}
