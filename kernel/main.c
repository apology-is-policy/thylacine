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
#include "../arch/arm64/kaslr.h"
#include "../mm/phys.h"
#include "../mm/magazines.h"

#include <stdint.h>
#include <thylacine/dtb.h>
#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/types.h>

// From arch/arm64/start.S — DTB physical address handed to us by the
// bootloader (in x0 per the Linux ARM64 boot protocol). Populated
// before boot_main() is called.
extern volatile u64 _saved_dtb_ptr;

// From arch/arm64/start.S — 1 if the kernel was entered at EL2 and
// dropped to EL1 by _real_start, 0 if entered at EL1 directly. Used
// for the el-entry banner diagnostic; surfaces a Pi 5 (or other
// EL2-entry firmware) condition that QEMU virt never exhibits.
extern volatile u64 _entered_at_el2;

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

    uart_puts("  cpus: 1 (P1-C-extras; SMP at P1-F)\n");

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

    uart_puts("  hardening: MMU+W^X+extinction+KASLR (P1-C-extras; PAC/MTE/CFI at P1-H)\n");

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

    // Smoke test: alloc/free 256 single pages plus a 2 MiB block;
    // verify free count returns to baseline. A real test harness
    // lands at P1-I; this is a sanity check that exercises the
    // magazine fast path (orders 0 and 9), the magazine refill /
    // drain, AND a non-magazine order (>=10 falls through to buddy
    // direct).
    {
        u64 baseline = phys_free_pages();
        #define SMOKE_N 256
        struct page *pages[SMOKE_N];
        bool ok = true;
        for (int i = 0; i < SMOKE_N; i++) {
            pages[i] = alloc_pages(0, KP_ZERO);
            if (!pages[i]) { ok = false; break; }
        }
        for (int i = 0; i < SMOKE_N; i++) {
            if (pages[i]) free_pages(pages[i], 0);
        }

        // Order-9 (2 MiB) round-trip — exercises the second magazine slot.
        struct page *big2 = alloc_pages(9, KP_ZERO);
        if (!big2) ok = false;
        if (big2) free_pages(big2, 9);

        // Order-10 (4 MiB) round-trip — bypasses magazines, hits buddy
        // direct + tests split/merge of larger blocks.
        struct page *big10 = alloc_pages(10, 0);
        if (!big10) ok = false;
        if (big10) free_pages(big10, 10);

        // Drain magazines back to buddy so the accounting is exact.
        // (Without this, the per-CPU magazine retains pages refilled
        // during the test — pages that are "allocated" from the buddy's
        // perspective even though the test logically returned them.)
        magazines_drain_all();

        u64 after = phys_free_pages();
        uart_puts("  alloc smoke: ");
        if (ok && after == baseline) {
            uart_puts("PASS (256 x 4 KiB + 2 MiB + 4 MiB alloc+free; free count restored)\n");
        } else {
            uart_puts("FAIL (");
            if (!ok) uart_puts("alloc returned NULL; ");
            if (after != baseline) {
                uart_puts("free count drift baseline=");
                uart_putdec(baseline);
                uart_puts(" after=");
                uart_putdec(after);
            }
            uart_puts(")\n");
            extinction("phys_init smoke test failed");
        }
    }

    uart_puts("  phase: " THYLACINE_PHASE_STRING "\n");

    uart_puts("Thylacine boot OK\n");

    // boot_main() must not return. start.S has a fallthrough to _hang
    // for safety, but be explicit here too.
    extern void _hang(void) __attribute__((noreturn));
    _hang();
}
