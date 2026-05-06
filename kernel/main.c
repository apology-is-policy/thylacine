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
#include "../arch/arm64/exception.h"
#include "../arch/arm64/gic.h"
#include "../arch/arm64/hwfeat.h"
#include "../arch/arm64/kaslr.h"
#include "../arch/arm64/timer.h"
#include "../mm/magazines.h"
#include "../mm/phys.h"
#include "../mm/slub.h"
#include "test/test.h"

#include <thylacine/canary.h>

#include <stdint.h>
#include <thylacine/dtb.h>
#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/pgrp.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/smp.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

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
            uart_set_base((uintptr_t)dtb_uart_base);
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
    uart_puts(" dist=");
    uart_puthex64(gic_dist_base());
    uart_puts(" redist=");
    uart_puthex64(gic_redist_base());
    uart_puts("\n");

    uart_puts("  timer: ");
    uart_putdec(timer_get_freq() / 1000UL);
    uart_puts(" kHz freq, 1000 Hz tick (PPI 14 / INTID 30)\n");

    // Phase 6: process / thread bring-up (P2-A entry).
    //
    // pgrp_init creates the namespace SLUB cache + kproc's empty Pgrp
    // (P2-Eb). Must run BEFORE proc_init so kproc has a pgrp to assign.
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
    pgrp_init();
    proc_init();
    thread_init();
    sched_init(0);                              // boot CPU's per-CPU sched state

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

    uart_puts("  sched:   bands=3 (INTERACTIVE/NORMAL/IDLE) runnable=");
    uart_putdec((u64)sched_runnable_count());
    uart_puts(" (0 expected pre-test; ready() inserts)\n");

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

    uart_puts("Thylacine boot OK\n");

    // boot_main() must not return. start.S has a fallthrough to _hang
    // for safety, but be explicit here too.
    extern void _hang(void) __attribute__((noreturn));
    _hang();
}
