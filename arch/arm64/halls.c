// Halls of Extinction -- Tier-1 crash dump (HX-1). See halls.h + the design
// scripture docs/HALLS-OF-EXTINCTION.md.
//
// The dump runs on the dying CPU at the extinction tail. It must assume the
// machine is in an unknown state: read only what is cheap and bounded, never
// loop, and survive its own faults. Three invariants carry that:
//   HX-I1  a fault DURING the dump trips the per-CPU guard and bails to
//          _torpor (the recursive extinction sees the guard set and returns).
//   HX-I2  the fp-chain walk is depth-capped + sanity-gated; a wild x29
//          cannot spin or read unboundedly (a single bad read still faults,
//          but that is caught by HX-I1).
//   HX-I4  the per-CPU live-frame slot is trusted only when it is a PLAUSIBLE
//          live frame on the current stack (see halls_dump) -- a frame
//          stranded by a since-exited Proc (whose kstack has been freed +
//          recycled) is rejected, so a stale slot never produces a
//          fabricated dump nor suppresses the correct capture-current.
//
// Output ordering is deliberate: the register block (pure ctx-field reads,
// no stack walk) is emitted FIRST so the most-likely-to-survive data lands
// before the backtrace / hexdump touch live (possibly-corrupt) stack.

#include "halls.h"

#include "exception.h"
#include "halls_symtab.h"
#include "kaslr.h"
#include "uart.h"

#include <thylacine/smp.h>
#include <thylacine/types.h>

// Per-CPU live-exception-frame slot + re-entrancy guard. Per-CPU: only the
// local CPU's exception handlers write its slot and only the local CPU's
// halls_dump reads it, so no atomics are needed (handlers do not migrate
// CPUs mid-execution at v1.0).
//
// The slot can be left STALE: a handler that departs via a noreturn path
// (exception_sync_lower_el_impl -> proc_fault_terminate/exits -> sched, when
// an EL0 fault or SYS_EXITS terminates the *Proc* but not the kernel) skips
// its wrapper's halls_leave_frame, stranding the slot pointing at a kstack
// that thread_free later returns to the buddy. halls_dump does NOT trust the
// raw slot for that reason -- HX-I4's plausibility gate rejects a stale slot.
static const struct exception_context *g_halls_frame[DTB_MAX_CPUS];
static u8 g_halls_in_dump[DTB_MAX_CPUS];

// A live exception frame sits just above the current SP (KERNEL_ENTRY built it
// at a higher address; the dump descended a few call frames below it). The
// gap is only the handler -> arch_fault_handle -> extinction -> halls_dump
// chain -- well under a stack. A stale/dangling slot points at a different
// (freed/other-thread) stack, so the gap is huge or negative. 16 KiB is far
// above the real chain depth yet far below an inter-stack distance.
#define HALLS_LIVE_FRAME_SLACK   (16u * 1024u)

// MPIDR.Aff0 can in principle exceed DTB_MAX_CPUS-1 on exotic topologies;
// clamp so a wild index never indexes the per-CPU arrays out of bounds (a
// bug in the crash handler itself would be the worst place to fault). F4:
// the clamp aliases an out-of-range index onto CPU 0; this is dormant --
// QEMU virt + the RPi-400 first board use Aff0 in [0,N), N <= DTB_MAX_CPUS=8
// (the F-C SMP sweep established that bound), so the supported topology never
// produces an out-of-range index. smp.c uses Aff0 as the CPU index kernel-
// wide, so this is consistent with the rest of the kernel.
static unsigned halls_cpu(void) {
    unsigned c = smp_cpu_idx_self();
    return c < DTB_MAX_CPUS ? c : 0u;
}

const struct exception_context *halls_enter_frame(const struct exception_context *ctx) {
    unsigned c = halls_cpu();
    const struct exception_context *prev = g_halls_frame[c];
    g_halls_frame[c] = ctx;
    return prev;
}

void halls_leave_frame(const struct exception_context *prev) {
    g_halls_frame[halls_cpu()] = prev;
}

const struct exception_context *halls_current_frame(void) {
    return g_halls_frame[halls_cpu()];
}

bool halls_fp_is_sane(u64 fp, u64 prev_fp, u64 lo, u64 hi) {
    if (fp & 0xfu)        return false;   // AAPCS64: frames are 16-aligned
    if (fp <= prev_fp)    return false;   // strictly increasing -> no cycles
    if (fp < lo)          return false;
    if (fp >= hi)         return false;
    return true;
}

bool halls_frame_is_live(u64 frame, u64 cur_sp) {
    return frame >= cur_sp && (frame - cur_sp) <= HALLS_LIVE_FRAME_SLACK;
}

u64 halls_link_addr(u64 addr, u64 kaslr_offset) {
    // A slid kernel code address is >= the runtime base (KASLR_LINK_VA +
    // offset); subtracting the offset recovers the link-time VA. A value
    // below the offset is not a slid code address (stack datum, small
    // constant) -- leave it untouched so the operator is not misled.
    if (addr < kaslr_offset) return addr;
    return addr - kaslr_offset;
}

// Return addresses spilled to the kernel stack are PAC-signed (FEAT_PAuth;
// the kernel ships hardening=PAC, signing with paciasp on entry). The raw
// stack word carries the pointer-auth code in the unused high VA bits; xpaci
// strips it, restoring the canonical VA so halls_link_addr + addr2line
// resolve the frame. xpaci is in the hint encoding space -- a NOP on a CPU
// without FEAT_PAuth -- and only strips (never authenticates), so it is safe
// on any value including an unsigned canonical address (idempotent).
static u64 halls_strip_pac(u64 addr) {
    __asm__ ("xpaci %0" : "+r"(addr));
    return addr;
}

// Bounded reads of (possibly bad) kernel memory. A fault here is caught by
// HX-I1. volatile so the access is not elided or reordered by the optimizer.
//
// F3 (Lazarus-gated): on real hardware a read to a Device-nGnRnE MMIO VA can
// stall the interconnect (never reaching _torpor) rather than fault. This is
// dormant on QEMU virt (RAM-backed) and largely unreachable post-HX-I4: only
// a PLAUSIBLE live frame is dumped, whose ctx->regs[29] is a real stack fp,
// and the fp-walk stays within [fp, fp+512KiB) of that stack. The residual --
// a live frame whose x29 is itself corrupt-into-device-space -- is revisited
// when the bare-metal (Lazarus) arc lands; a normal-RAM-VA predicate on the
// peek targets is the fix there.
static u64 halls_peek(u64 addr) {
    return *(volatile u64 *)(uintptr_t)addr;
}

// HX-2: minimal-width hex (no leading zeros) for the symbol offset --
// "boot_main+0x1a4" reads cleaner than uart_puthex64's fixed 16-digit field.
static void halls_puthex(u64 v) {
    uart_puts("0x");
    if (v == 0) { uart_putc('0'); return; }
    char buf[16];
    unsigned n = 0;
    while (v && n < sizeof(buf)) {
        u64 d = v & 0xfu;
        buf[n++] = (char)(d < 10u ? ('0' + d) : ('a' + (d - 10u)));
        v >>= 4;
    }
    while (n) uart_putc(buf[--n]);
}

// HX-2: pure binary search over an EXPLICIT (offset, name) table (the global
// wrapper below feeds it the linked-in table; test_halls feeds a synthetic
// one). Returns the greatest entry with `off <= (link_va - base)`. Reads only
// the .rodata table (no faulting stack reads) and is bounded by log2(count),
// so it is safe to call from the dying-machine dump path.
const char *halls_symbolize_table(const struct halls_sym *tab, u32 count,
                                  const char *names, u64 base,
                                  u64 link_va, u64 *out_off) {
    if (count == 0u || link_va < base) return (const char *)0;
    u64 q64 = link_va - base;
    if (q64 > 0xFFFFFFFFu) return (const char *)0;   // outside the u32 window
    u32 q = (u32)q64;
    if (q < tab[0].off) return (const char *)0;       // below the first symbol
    // Half-open [lo, hi); invariant tab[lo].off <= q holds after the guard.
    u32 lo = 0u, hi = count;
    while (hi - lo > 1u) {
        u32 mid = lo + (hi - lo) / 2u;
        if (tab[mid].off <= q) lo = mid;
        else                   hi = mid;
    }
    if (out_off) *out_off = (u64)(q - tab[lo].off);
    return &names[tab[lo].name_off];
}

const char *halls_symbolize(u64 link_va, u64 *out_off) {
    return halls_symbolize_table(halls_symtab, halls_symtab_count,
                                 halls_symtab_names, halls_symtab_link_base,
                                 link_va, out_off);
}

// One "raw  (link 0x...)" code-address line: strip the PAC, print the
// canonical address, then -- unless it is a userspace VA (skip_link; F5) --
// its KASLR link-time form for addr2line, and (HX-2) its live `name+0xN`
// symbolization. A user VA minus the kernel KASLR offset is nonsense, so both
// the link and the symbol are suppressed for EL0-source frames.
static void halls_emit_code_addr(const char *label, u64 addr, u64 koff, bool skip_link) {
    u64 a = halls_strip_pac(addr);
    uart_puts(label);
    uart_puthex64(a);
    if (!skip_link) {
        u64 link = halls_link_addr(a, koff);
        uart_puts("  link ");
        uart_puthex64(link);
        // HX-2: live symbolization. NULL (a stub/count-0 table, or an address
        // outside .text) falls back to the raw+link form -- the HX-1 behaviour.
        u64 off = 0;
        const char *name = halls_symbolize(link, &off);
        if (name) {
            uart_puts("  ");
            uart_puts(name);
            uart_putc('+');
            halls_puthex(off);
        }
    }
    uart_puts("\n");
}

#define HALLS_BT_MAX            32u
#define HALLS_STACK_SPAN        (512u * 1024u)   // fp-walk ceiling above the start
#define HALLS_STACK_DUMP_BYTES  256u

// Frame-pointer backtrace: #0 is the faulting/interrupted PC; subsequent
// frames walk the x29 chain reading the saved (PAC-signed) LR at [fp+8].
// F5: for an EL0-source frame the x29 is USERSPACE -- walking it would read
// user memory from EL1 (via TTBR0) and let a faulting Proc plant fake
// "kernel" backtrace entries, so the walk is skipped and the link addrs
// suppressed (the register block + ESR/FAR remain, which carry the signal).
static void halls_backtrace(const struct exception_context *ctx, u64 koff, bool el0_source) {
    uart_puts("HALLS: backtrace (fp-chain; link addrs for addr2line):\n");
    halls_emit_code_addr("HALLS:   #0  ", ctx->elr, koff, el0_source);
    if (el0_source) {
        uart_puts("HALLS:   (EL0 source -- userspace fp not walked from EL1)\n");
        return;
    }

    u64 fp   = ctx->regs[29];
    u64 lo   = fp;
    u64 hi   = fp + HALLS_STACK_SPAN;
    u64 prev = 0;
    for (unsigned i = 0; i < HALLS_BT_MAX; i++) {
        if (!halls_fp_is_sane(fp, prev, lo, hi)) break;
        u64 ra = halls_peek(fp + 8);
        uart_puts("HALLS:   #");
        uart_putdec((u64)(i + 1));
        uart_puts(i + 1 < 10 ? "  " : " ");
        halls_emit_code_addr("", ra, koff, false);
        prev = fp;
        fp = halls_peek(fp);   // next frame
    }
}

static void halls_stack_hexdump(u64 ksp) {
    uart_puts("HALLS: stack @");
    uart_puthex64(ksp);
    uart_puts(" (");
    uart_putdec((u64)HALLS_STACK_DUMP_BYTES);
    uart_puts(" bytes, ascending):\n");
    for (u64 a = ksp; a < ksp + HALLS_STACK_DUMP_BYTES; a += 16) {
        uart_puts("HALLS:   ");
        uart_puthex64(a);
        uart_puts(": ");
        uart_puthex64(halls_peek(a));
        uart_puts(" ");
        uart_puthex64(halls_peek(a + 8));
        uart_puts("\n");
    }
}

// Common dump body. `ctx` is a real or synthetic frame; `ksp` anchors the
// stack hexdump; `have_gp_regs` gates the x0..x28 dump (false for a bare
// extinction, where only fp/lr/sp survive the call into the dumper).
static void halls_emit(const struct exception_context *ctx, u64 ksp,
                       bool have_gp_regs, bool from_exception) {
    u64 koff  = kaslr_get_offset();
    unsigned el = (unsigned)((ctx->spsr >> 2) & 0x3u);   // SPSR M[3:2]: 0=EL0,1=EL1
    bool el0_source = from_exception && (el == 0);

    uart_puts("HALLS: --- Halls of Extinction (crash dump) ---\n");
    uart_puts("HALLS: cpu ");
    uart_putdec((u64)halls_cpu());
    uart_puts(from_exception ? (el0_source ? "  source EL0 (userspace fault)\n"
                                           : "  source EL1 (kernel fault)\n")
                             : "  source: bare extinction (no exception frame)\n");

    halls_emit_code_addr(from_exception ? "HALLS: ELR  " : "HALLS: ELR  (stale) ",
                         ctx->elr, koff, el0_source);

    uart_puts("HALLS: ESR  ");
    uart_puthex64(ctx->esr);
    uart_puts("  FAR  ");
    uart_puthex64(ctx->far);
    uart_puts("  SPSR ");
    uart_puthex64(ctx->spsr);
    uart_puts("\n");

    uart_puts("HALLS: SP   ");
    uart_puthex64(ksp);
    uart_puts("\n");
    halls_emit_code_addr("HALLS: LR   ", ctx->regs[30], koff, el0_source);
    uart_puts("HALLS: FP   ");
    uart_puthex64(ctx->regs[29]);
    uart_puts("\n");

    if (have_gp_regs) {
        for (unsigned i = 0; i < 29u; i += 2) {
            uart_puts("HALLS:  x");
            uart_putdec((u64)i);
            uart_puts(i < 10 ? "  = " : " = ");
            uart_puthex64(ctx->regs[i]);
            uart_puts("  x");
            uart_putdec((u64)(i + 1));
            uart_puts((i + 1) < 10 ? "  = " : " = ");
            uart_puthex64(ctx->regs[i + 1]);
            uart_puts("\n");
        }
    } else {
        uart_puts("HALLS:  x0..x28 not captured (bare extinction; fp/lr/sp above)\n");
    }

    uart_puts("HALLS: KASLR offset ");
    uart_puthex64(koff);
    uart_puts("  kbase ");
    uart_puthex64(kaslr_kernel_high_base());
    uart_puts("\n");

    halls_backtrace(ctx, koff, el0_source);
    halls_stack_hexdump(ksp);
    uart_puts("HALLS: --- end ---\n");
}

void halls_dump(const struct exception_context *ctx) {
    unsigned c = halls_cpu();

    // HX-I1: a fault during the dump re-enters here via the extinction path;
    // the set guard makes that re-entry a no-op so the caller reaches
    // _torpor instead of looping. Set BEFORE any potentially-faulting read.
    // F2: emit a visible marker on re-entry so a suppressed dump is never
    // silent. CONTRACT: every halls_dump caller (extinction / extinction_
    // with_addr) MUST _torpor() afterward -- so a faulting dump that leaves
    // this guard set is followed by a halt, and the set guard never wedges a
    // survivable caller. If a survivable caller is ever added, this guard
    // must become save/restore (like the frame slot).
    if (g_halls_in_dump[c]) {
        uart_puts("HALLS: (dump re-entered after a fault; suppressed)\n");
        return;
    }
    g_halls_in_dump[c] = 1;

    u64 cur_sp;
    __asm__ __volatile__("mov %0, sp" : "=r"(cur_sp));

    if (!ctx) ctx = g_halls_frame[c];

    // HX-I4: trust the per-CPU slot only if it is a PLAUSIBLE live frame on
    // the current stack -- just above cur_sp, within one stack of it. A stale
    // slot stranded by a since-exited Proc points at a freed/other-thread
    // stack (gap huge or negative) and is rejected, falling through to
    // capture-current. The fault->extinct path (incl. #806) always passes:
    // its frame is a few hundred bytes above cur_sp.
    if (ctx && !halls_frame_is_live((u64)(uintptr_t)ctx, cur_sp))
        ctx = (void *)0;   // stale/dangling slot -> capture-current instead

    if (ctx) {
        // Real, live exception frame. KERNEL_ENTRY did `sub sp,
        // #EXCEPTION_CTX_SIZE` then `mov x0, sp`, so the interrupted kernel SP
        // is ctx + the frame size. (For an EL0 source, ctx->sp holds SP_EL0;
        // the kernel SP at entry is still ctx + size -- the thread's kstack.)
        u64 ksp = (u64)(uintptr_t)ctx + (u64)EXCEPTION_CTX_SIZE;
        halls_emit(ctx, ksp, /*have_gp_regs=*/true, /*from_exception=*/true);
    } else {
        // Bare extinction (or a rejected stale slot): no live exception frame.
        // Capture what survives the call into the dumper -- current sp/fp/lr --
        // and read the EL1 syndrome regs best-effort (stale at an assert,
        // labelled as such). The x29 chain from here walks halls_dump ->
        // extinction -> the assert site (kernel context -> backtrace valid).
        struct exception_context synth;
        for (unsigned i = 0; i < 31u; i++) synth.regs[i] = 0;
        synth.regs[29] = (u64)(uintptr_t)__builtin_frame_address(0);
        synth.regs[30] = (u64)(uintptr_t)__builtin_return_address(0);
        synth.sp = cur_sp;
        __asm__ __volatile__("mrs %0, elr_el1"  : "=r"(synth.elr));
        __asm__ __volatile__("mrs %0, spsr_el1" : "=r"(synth.spsr));
        __asm__ __volatile__("mrs %0, esr_el1"  : "=r"(synth.esr));
        __asm__ __volatile__("mrs %0, far_el1"  : "=r"(synth.far));
        halls_emit(&synth, cur_sp, /*have_gp_regs=*/false, /*from_exception=*/false);
    }

    // Reached on a clean (non-faulting) dump. The guard stays set on the
    // faulting path (we never get here then); clearing here keeps the slot
    // honest if a future caller ever dumps without immediately torpor-ing.
    g_halls_in_dump[c] = 0;
}
