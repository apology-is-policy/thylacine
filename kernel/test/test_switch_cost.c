// What a context switch is made of, timed on the machine it runs on, so a fast path
// is chosen from numbers. It reports and claims nothing about the cost: the figures
// mean something under HVF and KVM and very little under TCG.
//
// Each figure is the best of SC_ROUNDS rounds, which discards a round a tick landed
// in. `call` is the loop's own cost per piece, included in every piece's figure.

#include "test.h"

#include "../../arch/arm64/timer.h"
#include "../../arch/arm64/uart.h"

#include <thylacine/context.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/smp.h>
#include <thylacine/spinlock.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#define SC_ROUNDS   5u
#define SC_PIECES   4096u
#define SC_SWITCHES 2048u

static struct Thread *g_sc_main;
static volatile u32   g_sc_stop;
static u8             g_sc_area[520] __attribute__((aligned(16)));

static void sc_direct_peer(void) {
    for (;;) thread_switch(g_sc_main);
}

static void sc_sched_peer(void) {
    while (!g_sc_stop) sched();
    (void)spin_lock_irqsave(NULL);   // thread_kthread_retire's contract: masked to sched()
    thread_kthread_retire();
    sched();
}

static void sc_piece_call(void) {
    __asm__ __volatile__("" ::: "memory");
}

static void sc_piece_fp(void) {
    fp_save_area(g_sc_area);
    fp_restore_area(g_sc_area);
}

static void sc_piece_ttbr0(void) {
    u64 v;
    __asm__ __volatile__("mrs %0, ttbr0_el1\n\t"
                         "msr ttbr0_el1, %0\n\t"
                         "isb"
                         : "=&r"(v) : : "memory");
}

static void sc_piece_isb(void) {
    __asm__ __volatile__("isb" ::: "memory");
}

static void sc_piece_dsb(void) {
    __asm__ __volatile__("dsb ish" ::: "memory");
}

static u64 sc_best_ns(void (*piece)(void)) {
    u64 best = ~0ull;
    for (u32 r = 0; r < SC_ROUNDS; r++) {
        irq_state_t s = spin_lock_irqsave(NULL);
        u64 t0 = timer_now_ns();
        for (u32 i = 0; i < SC_PIECES; i++)
            piece();
        u64 dt = timer_now_ns() - t0;
        spin_unlock_irqrestore(NULL, s);
        if (dt < best) best = dt;
    }
    return best / SC_PIECES;
}

static void sc_report(const char *name, u64 ns) {
    uart_puts(name);
    uart_putc(' ');
    uart_putdec(ns);
    uart_puts(" ns");
}

void test_context_switch_cost(void) {
    g_sc_main = current_thread();
    TEST_ASSERT(g_sc_main == kthread(), "runs on the test kthread");

    // The primitive and its wrapper: no pick, no run tree, no time stamps.
    struct Thread *d = thread_create(kproc(), sc_direct_peer);
    TEST_ASSERT(d != NULL, "thread_create failed");
    u64 direct = ~0ull;
    for (u32 r = 0; r < SC_ROUNDS; r++) {
        u64 t0 = timer_now_ns();
        for (u32 i = 0; i < SC_SWITCHES; i++)
            thread_switch(d);
        u64 dt = timer_now_ns() - t0;
        if (dt < direct) direct = dt;
    }
    thread_free(d);

    // The whole path, between two threads pinned to this CPU. A sched() that keeps
    // the caller costs less than a switch, so the figure divides by switches taken.
    g_sc_stop = 0;
    struct Thread *p = thread_create(kproc(), sc_sched_peer);
    TEST_ASSERT(p != NULL, "thread_create failed");
    p->cpu_pinned = true;
    u64 before = g_sc_main->nsched;
    ready_on(smp_cpu_idx_self(), p);
    u64 t0 = timer_now_ns();
    for (u32 i = 0; i < SC_SWITCHES; i++)
        sched();
    u64 whole = timer_now_ns() - t0;
    u64 taken = (g_sc_main->nsched - before) + p->nsched;
    g_sc_stop = 1;
    TEST_YIELD_UNTIL(p->state == THREAD_EXITING);
    thread_free(p);
    TEST_ASSERT(taken != 0, "the two threads never switched, so there is no figure");

    uart_puts("    switch-cost: ");
    sc_report("direct", direct / (2u * SC_SWITCHES));
    uart_puts(", ");
    sc_report("sched", whole / taken);
    uart_puts(" (n=");
    uart_putdec(taken);
    uart_puts("), ");
    sc_report("fp-pair", sc_best_ns(sc_piece_fp));
    uart_puts(", ");
    sc_report("ttbr0+isb", sc_best_ns(sc_piece_ttbr0));
    uart_puts(", ");
    sc_report("isb", sc_best_ns(sc_piece_isb));
    uart_puts(", ");
    sc_report("dsb-ish", sc_best_ns(sc_piece_dsb));
    uart_puts(", ");
    sc_report("call", sc_best_ns(sc_piece_call));
    uart_puts("\n");
}
