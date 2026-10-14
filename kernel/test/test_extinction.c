// The extinction console claim -- the serializer that keeps the "EXTINCTION: "
// ABI line intact when more than one CPU dies at once.
//
// WHY THE CLAIM EXISTS. The re-entrancy guard is deliberately per-CPU, so two
// CPUs extincting concurrently both proceed, and each emits the banner as four
// separate unlocked uart_puts calls. Interleaved, the prefix can stop starting
// a line -- and every gate matches it anchored (`grep -q "^EXTINCTION:"` in
// tools/test-fault.sh, and bare-token matchers elsewhere), so a torn banner is
// not a cosmetic problem: it is a real extinction the harness cannot see.
// Fail-open on the one channel the whole test discipline trusts.
//
//   extinction.claim_word_exactly_one_winner
//     the exactly-one-winner bookkeeping, on a LOCAL word.
//   extinction.console_unclaimed_on_clean_boot
//     the live console is still unclaimed at suite time.
//
// WHAT THE FIRST TEST DOES NOT COVER, stated because the gap is easy to miss.
// It is SEQUENTIAL, and the property under test is a RACE. A non-atomic
// `if (*w) return 0; *w = 1; return 1;` passes it identically -- so it pins the
// bookkeeping (a second caller loses, and keeps losing) and NOT the atomicity.
// The atomicity comes from __atomic_exchange_n, a compiler intrinsic rather
// than our code. The regression this leaves uncovered is a concurrent one, and
// covering it honestly needs a multi-CPU fault-injection arm with a FORCED
// interleaving: without forcing it, the pre-fix build garbles only sometimes,
// and a discriminator that fails only sometimes is not a regression test. That
// arm is tracked rather than skipped quietly.
//
// THE SECOND TEST IS THE ONE THAT GUARDS THE NEW HAZARD. The claim introduces a
// failure mode that did not exist before it: whoever holds the console never
// releases it (every path through extinction() ends in _torpor()), so anything
// that claims it SPURIOUSLY silences every later extinction banner in the boot
// -- the same fail-open arriving from the other side. Hence the deliberate
// split in the interface: the claim core is exported to be exercised on a
// caller-supplied word, and nothing exports a way to claim the live one. A test
// that took the real console would disable extinction reporting for every test
// that ran after it, and would do so silently.

#include "test.h"

#include <thylacine/extinction.h>
#include <thylacine/sched.h>
#include <thylacine/smp.h>
#include <thylacine/spinlock.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

void test_extinction_claim_word_exactly_one_winner(void);
void test_extinction_console_unclaimed_on_clean_boot(void);

void test_extinction_claim_word_exactly_one_winner(void) {
    uint32_t word = 0;

    int first  = extinction_claim_word(&word);
    int second = extinction_claim_word(&word);
    int third  = extinction_claim_word(&word);

    TEST_ASSERT(first == 1,
        "the first claimant of a free word must win");
    TEST_ASSERT(second == 0,
        "a second claimant must lose -- two winners means two CPUs both print");
    TEST_ASSERT(third == 0,
        "losing must be durable: the winner never releases, so every later "
        "claimant loses too");

    // A fresh word wins again -- the loss is a property of the WORD, not a
    // latch in the function. Without this leg the test passes just as well
    // against a claim that always returns 0 after its first call ever.
    uint32_t fresh = 0;
    TEST_ASSERT(extinction_claim_word(&fresh) == 1,
        "an untouched word must still be claimable -- the state belongs to the "
        "word, not to the claim function");
}

void test_extinction_console_unclaimed_on_clean_boot(void) {
    // If this fires, extinction reporting is ALREADY disabled for the rest of
    // this boot: the banner is emitted only by the CPU holding the console, and
    // nothing ever releases it. Any later extinction -- including one a later
    // test is trying to provoke -- would park silently, and the harness would
    // read a hang or a clean run instead of a crash.
    TEST_ASSERT(extinction_console_claimed() == 0,
        "the extinction console must be unclaimed on a healthy boot; a claim "
        "here means every later extinction banner is silently suppressed");
}

// The per-byte console gate (the third tearing source: a peer's direct
// uart_putc). Its cores run on the test's own words, for the reason above.
//
//   extinction.tx_gate_admits_only_the_owner_once_claimed
//   extinction.tx_gate_counts_nested_bytes
//   extinction.tx_enter_holds_the_thread_on_its_cpu
//   extinction.tx_exit_takes_a_pending_preempt
//   extinction.tx_wait_waits_out_a_peer_byte
//
// Like the claim test these are sequential: they pin each side's decision, not
// the Dekker race between them. The race is tools/test-fault.sh's
// extinction_peer_chatter variant, which tears the banner without the gate.

void test_extinction_tx_gate_admits_only_the_owner_once_claimed(void);
void test_extinction_tx_gate_counts_nested_bytes(void);
void test_extinction_tx_enter_holds_the_thread_on_its_cpu(void);
void test_extinction_tx_exit_takes_a_pending_preempt(void);
void test_extinction_tx_wait_waits_out_a_peer_byte(void);

void test_extinction_tx_gate_admits_only_the_owner_once_claimed(void) {
    volatile uint8_t busy = 0;
    uint32_t word = 0;

    int open = extinction_tx_gate_core(&busy, &word, 0);
    uint8_t open_busy = busy;

    busy = 0;
    word = 1;
    int peer = extinction_tx_gate_core(&busy, &word, 0);
    uint8_t peer_busy = busy;

    busy = 0;
    int owner = extinction_tx_gate_core(&busy, &word, 1);
    uint8_t owner_busy = busy;

    TEST_ASSERT(open == 1, "an unclaimed console admits every cpu's byte");
    TEST_ASSERT(open_busy == 1,
        "an admitted byte is published as in flight, so a winner can wait it out");
    TEST_ASSERT(peer == 0,
        "once a peer owns the console this cpu's byte is dropped -- it would land "
        "inside the banner");
    TEST_ASSERT(peer_busy == 0,
        "a dropped byte must not stay published: the winner would wait on it for "
        "its whole bound");
    TEST_ASSERT(owner == 1 && owner_busy == 1,
        "the owner's own bytes (the banner, the dump) still pass");
}

// An exception taken inside uart_putc (after the gate, before the byte) can
// print through the gate again on the same cpu. Its exit, or its refusal by a
// claim that landed meanwhile, must leave the outer byte marked in flight.
void test_extinction_tx_gate_counts_nested_bytes(void) {
    volatile uint8_t busy = 0;
    uint32_t word = 0;

    int outer = extinction_tx_gate_core(&busy, &word, 0);
    int inner = extinction_tx_gate_core(&busy, &word, 0);
    uint8_t both = busy;
    extinction_tx_exit_core(&busy);
    uint8_t after_inner = busy;

    word = 1;
    int refused = extinction_tx_gate_core(&busy, &word, 0);
    uint8_t after_refusal = busy;
    extinction_tx_exit_core(&busy);
    uint8_t after_outer = busy;

    TEST_ASSERT(outer == 1 && inner == 1 && both == 2,
        "a nested byte on the same cpu is counted, not merged with the outer one");
    TEST_ASSERT(after_inner == 1,
        "the nested byte's exit leaves the outer byte in flight, so a winner "
        "still waits for it");
    TEST_ASSERT(refused == 0 && after_refusal == 1,
        "a nested byte refused by a fresh claim leaves the outer byte in flight");
    TEST_ASSERT(after_outer == 0, "the outer byte's exit clears the cpu");
}

// The live gate, on the unclaimed console: the mark is per CPU, so the byte's
// thread must stay on its CPU from enter to exit. With IRQs masked nothing can
// move it, and the gate must not read thread state at all: every exception
// path and extinction() itself print masked, and TPIDR_EL1 may be wild there.
void test_extinction_tx_enter_holds_the_thread_on_its_cpu(void) {
    struct Thread *me = current_thread();
    u32 before = me->preempt_count;
    int token = extinction_tx_enter();
    u32 during = me->preempt_count;
    if (token) extinction_tx_exit(token);
    u32 after = me->preempt_count;

    irq_state_t s = spin_lock_irqsave(NULL);
    int mtoken = extinction_tx_enter();
    u32 mduring = me->preempt_count;
    if (mtoken) extinction_tx_exit(mtoken);
    spin_unlock_irqrestore(NULL, s);

    TEST_ASSERT(token == EXTINCTION_TX_HELD, "an unclaimed console admits the byte, held");
    TEST_ASSERT(during == before + 1u,
        "an admitted byte holds its thread's preempt count, so the thread cannot leave its cpu mid-byte");
    TEST_ASSERT(after == before, "exit releases the hold");
    TEST_ASSERT(mtoken == EXTINCTION_TX_BYTE && mduring == before,
        "with irqs masked the gate admits the byte without touching thread state");
}

// A preempt that came due while a byte held the thread is taken when the hold
// drops, not left for an IRQ that happens to land between two bytes: a thread
// printing a long line is otherwise non-preemptible for most of it.
void test_extinction_tx_exit_takes_a_pending_preempt(void) {
    unsigned cpu = smp_cpu_idx_self();
    sched_set_need_resched_for_test(cpu);
    int token = extinction_tx_enter();
    if (token) extinction_tx_exit(token);
    bool still = sched_need_resched_pending(cpu);
    sched_clear_need_resched_for_test(cpu);

    TEST_ASSERT(token == EXTINCTION_TX_HELD, "the suite runs with irqs on, so the byte is held");
    TEST_ASSERT(!still, "the hold's drop takes the preempt that came due under it");
}

void test_extinction_tx_wait_waits_out_a_peer_byte(void) {
    volatile uint8_t busy[4] = { 0, 0, 0, 0 };

    int clear = extinction_tx_wait_core(busy, 4, 0);

    busy[0] = 1;                                   // the winner's own flag
    int self_only = extinction_tx_wait_core(busy, 4, 0);

    busy[0] = 0;
    busy[2] = 1;                                   // a peer that never finishes
    int stuck = extinction_tx_wait_core(busy, 4, 0);

    TEST_ASSERT(clear == 1, "no byte in flight: the winner proceeds at once");
    TEST_ASSERT(self_only == 1,
        "the winner does not wait on its own flag (it would wait out its bound)");
    TEST_ASSERT(stuck == 0,
        "a peer byte still in flight is waited on, and the wait is bounded: it "
        "reports the miss rather than parking the dying machine");
}
