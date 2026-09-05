// Kernel pipe blocking-mode tests (P5-pipe-blocking).
//
// Exercises the wait/wake protocol per `specs/pipe.tla`. Each test
// composes a boot thread + one consumer thread. The consumer either:
//   - reads from an empty pipe (registers a read poll-hook and sleeps; boot writes
//     or closes the write end to wake it), or
//   - writes to a full pipe (registers a write poll-hook and sleeps; boot reads or
//     closes the read end to wake it).
//
// The pattern mirrors `kernel/test/test_rendez.c::test_rendez_basic_
// handoff`: spawn consumer + ready + sched (yield to consumer). After
// the yield returns, consumer has reached its first sleep. Boot does
// the wake-triggering action. Boot sched()s again; consumer runs to
// completion + calls sched() at the end (parks; never returns from
// entry).
//
// Coverage:
//
//   pipe_blocking.write_wakes_sleeping_reader
//     Consumer reads on empty → sleeps. Boot writes 5 bytes → reader
//     wakes + drains.
//
//   pipe_blocking.read_wakes_sleeping_writer
//     Boot fills the buffer. Consumer writes 1 more byte → sleeps.
//     Boot drains some bytes → writer wakes + appends.
//
//   pipe_blocking.close_write_end_wakes_reader_with_eof
//     Consumer reads on empty → sleeps. Boot closes the write end →
//     reader wakes; read returns 0 (EOF).
//
//   pipe_blocking.close_read_end_wakes_writer_with_epipe
//     Boot fills the buffer. Consumer writes 1 more byte → sleeps.
//     Boot closes the read end → writer wakes; write returns -1
//     (EPIPE).
//
//   pipe_blocking.multi_readers_share_one_empty_pipe
//   pipe_blocking.multi_writers_share_one_full_pipe
//     THREE consumers block on the same direction of one pipe -- the shape
//     every EL0 program that forks or dups a pipe end produces (a jobserver,
//     `make -j 2>&1 | tee`, a prefork pool). Under the per-direction Rendez
//     the second sleeper EXTINCTED the kernel ("rendez already has a
//     waiter"); under the poll_list all sleep. Each test proves three things:
//     no second-sleeper extinction, RE-SAMPLE (one edge wakes all, one
//     consumes, the rest re-sample and sleep again), and WAKE-ALL (a close
//     edge with TWO still asleep releases both -- a wake-one bug strands one).

#include "test.h"

#include <thylacine/dev.h>
#include <thylacine/errno.h>
#include <thylacine/pipe.h>
#include <thylacine/proc.h>
#include <thylacine/rendez.h>
#include <thylacine/sched.h>
#include <thylacine/spoor.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

// =============================================================================
// Per-test shared state. All tests run on the single boot CPU
// (single-CPU at v1.0); per-test re-init at entry prevents cross-test
// contamination.
// =============================================================================

static struct Spoor *g_rd;
static struct Spoor *g_wr;
static volatile long g_consumer_result;
static u8            g_consumer_buf[PIPE_BUF_SIZE];

static long dev_write(struct Spoor *c, const void *buf, long n) {
    return c->dev->write(c, buf, n, 0);
}

static long dev_read(struct Spoor *c, void *buf, long n) {
    return c->dev->read(c, buf, n, 0);
}

// =============================================================================
// Forward decls.
// =============================================================================

void test_pipe_blocking_write_wakes_sleeping_reader(void);
void test_pipe_blocking_read_wakes_sleeping_writer(void);
void test_pipe_blocking_close_write_end_wakes_reader_with_eof(void);
void test_pipe_blocking_close_read_end_wakes_writer_with_epipe(void);

// =============================================================================
// Consumer entries. Each: do one blocking op; record result; park.
// =============================================================================

static void consumer_read_entry(void) {
    g_consumer_result = dev_read(g_rd, g_consumer_buf, (long)sizeof(g_consumer_buf));
    sched();    // park — boot doesn't yield back to us
}

static void consumer_write_one_byte_entry(void) {
    static const u8 byte = 0x42;
    g_consumer_result = dev_write(g_wr, &byte, 1L);
    sched();
}

// The SECOND consumer of the two-waiter tests: its own result slot + buffer,
// so the test can tell which of the two waiters an edge released.
static volatile long g_consumer2_result;
static u8            g_consumer2_buf[PIPE_BUF_SIZE];

static void consumer2_read_entry(void) {
    g_consumer2_result = dev_read(g_rd, g_consumer2_buf, (long)sizeof(g_consumer2_buf));
    sched();
}

static void consumer2_write_one_byte_entry(void) {
    static const u8 byte = 0x43;
    g_consumer2_result = dev_write(g_wr, &byte, 1L);
    sched();
}

// A third consumer, so a single edge can be shown to release MORE THAN ONE
// blocked waiter (the wake-ALL property specs/pipe.tla now pins) AND a woken
// waiter can be shown to re-sample and re-sleep when a peer took the bytes.
static volatile long g_consumer3_result;
static u8            g_consumer3_buf[PIPE_BUF_SIZE];

static void consumer3_read_entry(void) {
    g_consumer3_result = dev_read(g_rd, g_consumer3_buf, (long)sizeof(g_consumer3_buf));
    sched();
}

static void consumer3_write_one_byte_entry(void) {
    static const u8 byte = 0x44;
    g_consumer3_result = dev_write(g_wr, &byte, 1L);
    sched();
}

// =============================================================================
// Tests.
// =============================================================================

void test_pipe_blocking_write_wakes_sleeping_reader(void) {
    g_rd = NULL;
    g_wr = NULL;
    g_consumer_result = -999;
    TEST_EXPECT_EQ(pipe_create(&g_rd, &g_wr), 0, "create");

    struct Thread *consumer = thread_create(kproc(), consumer_read_entry);
    TEST_ASSERT(consumer != NULL, "thread_create");
    ready(consumer);
    // Yield to consumer. It enters dev_read; pipe is empty, !write_eof
    // → registers a read poll-hook and sleeps. Scheduler picks boot again; we resume.
    TEST_YIELD_UNTIL(consumer->state == THREAD_SLEEPING);
    TEST_EXPECT_EQ(consumer->state, THREAD_SLEEPING,
        "consumer should be SLEEPING after reaching dev_read on empty");

    // Boot side: write 5 bytes. devpipe_write appends + wakes
    // the poll_list. Consumer transitions to RUNNABLE.
    const u8 payload[] = { 0x11, 0x22, 0x33, 0x44, 0x55 };
    TEST_EXPECT_EQ(dev_write(g_wr, payload, (long)sizeof(payload)),
                   (long)sizeof(payload),
        "boot writes payload");
    TEST_EXPECT_NE(consumer->state, THREAD_SLEEPING,
        "consumer left the rendez after write");

    // Yield. Consumer resumes inside sleep's loop; the ring readable
    // (count > 0); sleep returns; loop re-takes lock; drains; wakes
    // (no waiting writer — no-op); returns. Consumer sets
    // g_consumer_result + sched()s back.
    TEST_YIELD_UNTIL(g_consumer_result != -999);
    TEST_EXPECT_EQ(g_consumer_result, (long)sizeof(payload),
        "consumer drained payload-length bytes");
    for (size_t i = 0; i < sizeof(payload); i++) {
        TEST_ASSERT(g_consumer_buf[i] == payload[i],
            "consumer's bytes match what boot wrote");
    }

    // Reap the consumer: it ran its op then parked in a trailing sched()
    // (RUNNABLE, never returns from its entry). Without this it leaks as a
    // runnable thread for the rest of the boot -- the band-NORMAL half of the
    // #857 quiescence pollution. Matches test_cons / test_sched hygiene.
    thread_free(consumer);
    spoor_clunk(g_rd);
    spoor_clunk(g_wr);
}

void test_pipe_blocking_read_wakes_sleeping_writer(void) {
    g_rd = NULL;
    g_wr = NULL;
    g_consumer_result = -999;
    TEST_EXPECT_EQ(pipe_create(&g_rd, &g_wr), 0, "create");

    // Boot fills the buffer completely so consumer's write blocks.
    static u8 fill[PIPE_BUF_SIZE];
    for (size_t i = 0; i < PIPE_BUF_SIZE; i++) fill[i] = (u8)(i & 0xff);
    TEST_EXPECT_EQ(dev_write(g_wr, fill, (long)PIPE_BUF_SIZE),
                   (long)PIPE_BUF_SIZE,
        "boot fills the buffer");

    struct Thread *consumer = thread_create(kproc(), consumer_write_one_byte_entry);
    TEST_ASSERT(consumer != NULL, "thread_create");
    ready(consumer);
    TEST_YIELD_UNTIL(consumer->state == THREAD_SLEEPING);
    TEST_EXPECT_EQ(consumer->state, THREAD_SLEEPING,
        "consumer should be SLEEPING after reaching dev_write on full");

    // Boot drains 10 bytes — makes space — walks the poll_list.
    u8 drain[10];
    TEST_EXPECT_EQ(dev_read(g_rd, drain, 10L), 10L, "boot drains 10 bytes");
    TEST_EXPECT_NE(consumer->state, THREAD_SLEEPING,
        "consumer left the rendez after read");

    TEST_YIELD_UNTIL(g_consumer_result != -999);
    TEST_EXPECT_EQ(g_consumer_result, 1L,
        "consumer wrote 1 byte after wake");

    thread_free(consumer);          // reap the parked helper (see write_wakes)
    spoor_clunk(g_rd);
    spoor_clunk(g_wr);
}

void test_pipe_blocking_close_write_end_wakes_reader_with_eof(void) {
    g_rd = NULL;
    g_wr = NULL;
    g_consumer_result = -999;
    TEST_EXPECT_EQ(pipe_create(&g_rd, &g_wr), 0, "create");

    struct Thread *consumer = thread_create(kproc(), consumer_read_entry);
    TEST_ASSERT(consumer != NULL, "thread_create");
    ready(consumer);
    TEST_YIELD_UNTIL(consumer->state == THREAD_SLEEPING);
    TEST_EXPECT_EQ(consumer->state, THREAD_SLEEPING,
        "consumer SLEEPING on empty read");

    // Boot closes the write end. devpipe_close sets write_eof + wakes
    // the poll_list. Consumer wakes; sees write_eof; returns 0 (EOF).
    spoor_clunk(g_wr);
    TEST_EXPECT_NE(consumer->state, THREAD_SLEEPING,
        "consumer left the rendez after close");

    TEST_YIELD_UNTIL(g_consumer_result != -999);
    TEST_EXPECT_EQ(g_consumer_result, 0L,
        "consumer read returns 0 (EOF) after write end closed");

    thread_free(consumer);          // reap the parked helper (see write_wakes)
    spoor_clunk(g_rd);
}

void test_pipe_blocking_close_read_end_wakes_writer_with_epipe(void) {
    g_rd = NULL;
    g_wr = NULL;
    g_consumer_result = -999;
    TEST_EXPECT_EQ(pipe_create(&g_rd, &g_wr), 0, "create");

    // Boot fills the buffer so consumer's write blocks.
    static u8 fill[PIPE_BUF_SIZE];
    for (size_t i = 0; i < PIPE_BUF_SIZE; i++) fill[i] = (u8)(i & 0xff);
    TEST_EXPECT_EQ(dev_write(g_wr, fill, (long)PIPE_BUF_SIZE),
                   (long)PIPE_BUF_SIZE,
        "boot fills the buffer");

    struct Thread *consumer = thread_create(kproc(), consumer_write_one_byte_entry);
    TEST_ASSERT(consumer != NULL, "thread_create");
    ready(consumer);
    TEST_YIELD_UNTIL(consumer->state == THREAD_SLEEPING);
    TEST_EXPECT_EQ(consumer->state, THREAD_SLEEPING,
        "consumer SLEEPING on full write");

    // Boot closes the read end. devpipe_close sets read_eof + wakes
    // the poll_list. Consumer wakes; sees read_eof; returns -T_E_PIPE.
    spoor_clunk(g_rd);
    TEST_EXPECT_NE(consumer->state, THREAD_SLEEPING,
        "consumer left the rendez after close");

    TEST_YIELD_UNTIL(g_consumer_result != -999);
    // #100 (ER-3): the value matters -- this is the BLOCKED writer's arm, so
    // it proves the errno survives the wake path too, not just the immediate
    // read_eof reject the non-blocking sibling covers. The wait is BOUNDED
    // (#134) rather than a bare sched(): a lost wake must fail this test, not
    // hang the suite.
    TEST_EXPECT_EQ(g_consumer_result, (long)(-T_E_PIPE),
        "consumer write returns -T_E_PIPE after read end closed");

    thread_free(consumer);          // reap the parked helper (see write_wakes)
    spoor_clunk(g_wr);
}

// THREE readers blocked on one empty pipe -- the fork/dup/thread-shared
// endpoint shape. Proves three properties in one run:
//   * NO SECOND-SLEEPER EXTINCTION: all three reach SLEEPING. On the retired
//     per-direction Rendez the second sleep() extincted the kernel.
//   * RE-SAMPLE: a 2-byte write wakes every hook; exactly ONE reader drains it,
//     the other two find the ring empty again and sleep again (the re-loop).
//   * WAKE-ALL: closing the write end is ONE edge with TWO readers still
//     asleep, and the EOF-return path takes no further wake, so nothing
//     cascades -- both must return 0. A wake-ONE bug (poll_waiter_list_wake
//     breaking after the head) releases one and strands the other at EOF
//     forever; specs/pipe.tla's pipe_buggy_wake_one_reader.cfg is the model
//     twin. (The write phase alone cannot witness wake-all: a woken reader's
//     drain re-walks the list, so under wake-one the drain re-wakes the next,
//     and the bug hides behind the cascade. Only the no-cascade EOF edge with
//     two waiters distinguishes it.)
void test_pipe_blocking_multi_readers_share_one_empty_pipe(void);
void test_pipe_blocking_multi_readers_share_one_empty_pipe(void) {
    g_rd = NULL;
    g_wr = NULL;
    g_consumer_result  = -999;
    g_consumer2_result = -999;
    g_consumer3_result = -999;
    TEST_EXPECT_EQ(pipe_create(&g_rd, &g_wr), 0, "create");

    struct Thread *cs[3];
    volatile long *rs[3] = { &g_consumer_result, &g_consumer2_result, &g_consumer3_result };
    cs[0] = thread_create(kproc(), consumer_read_entry);
    cs[1] = thread_create(kproc(), consumer2_read_entry);
    cs[2] = thread_create(kproc(), consumer3_read_entry);
    TEST_ASSERT(cs[0] && cs[1] && cs[2], "thread_create x3");
    for (int i = 0; i < 3; i++) {
        ready(cs[i]);
        TEST_YIELD_UNTIL(cs[i]->state == THREAD_SLEEPING);   // the 2nd/3rd sleeper
    }
    TEST_ASSERT(cs[0]->state == THREAD_SLEEPING && cs[1]->state == THREAD_SLEEPING
                && cs[2]->state == THREAD_SLEEPING,
        "all three readers SLEEPING on one empty pipe (no second-sleeper extinction)");

    // ONE 2-byte edge: exactly one reader drains it, two re-sample + re-sleep.
    const u8 payload[] = { 0xa1, 0xb2 };
    TEST_EXPECT_EQ(dev_write(g_wr, payload, 2L), 2L, "boot writes 2 bytes");
    TEST_YIELD_UNTIL(*rs[0] != -999 || *rs[1] != -999 || *rs[2] != -999);
    // Let the two that did not win settle back to SLEEPING.
    for (int i = 0; i < 3; i++)
        if (*rs[i] == -999) TEST_YIELD_UNTIL(cs[i]->state == THREAD_SLEEPING);
    int drained_idx = -1, asleep = 0, got_count = 0;
    for (int i = 0; i < 3; i++) {
        if (*rs[i] != -999) { drained_idx = i; got_count++; }
        else if (cs[i]->state == THREAD_SLEEPING) asleep++;
    }
    long drained_val = (drained_idx >= 0) ? *rs[drained_idx] : -1;

    // The wake-all edge: close with TWO readers still asleep -> both get EOF.
    spoor_clunk(g_wr);
    for (int i = 0; i < 3; i++)
        if (i != drained_idx) TEST_YIELD_UNTIL(*rs[i] != -999);
    int eof_count = 0;
    for (int i = 0; i < 3; i++)
        if (i != drained_idx && *rs[i] == 0) eof_count++;

    for (int i = 0; i < 3; i++) thread_free(cs[i]);
    spoor_clunk(g_rd);

    TEST_EXPECT_EQ(got_count, 1, "exactly one reader drained the write");
    TEST_EXPECT_EQ(drained_val, 2L, "...and it got both bytes");
    TEST_EXPECT_EQ(asleep, 2, "RE-SAMPLE: the other two woke, found nothing, and re-slept");
    TEST_EXPECT_EQ(eof_count, 2,
        "WAKE-ALL: one close edge released BOTH remaining readers with EOF "
        "(a wake-one bug strands one at EOF forever)");
}

// THREE writers blocked on one full pipe -- the `make -j | tee` shape. The
// writer twin of the reader test: no second-sleeper extinction (all three
// SLEEPING on full), re-sample (a 1-byte drain wakes all; one appends, two
// re-sample full + re-sleep), and wake-all (closing the READ end is one edge
// with two writers still asleep -> both return -T_E_PIPE; a wake-one bug
// strands one on the full ring forever).
void test_pipe_blocking_multi_writers_share_one_full_pipe(void);
void test_pipe_blocking_multi_writers_share_one_full_pipe(void) {
    g_rd = NULL;
    g_wr = NULL;
    g_consumer_result  = -999;
    g_consumer2_result = -999;
    g_consumer3_result = -999;
    TEST_EXPECT_EQ(pipe_create(&g_rd, &g_wr), 0, "create");

    static u8 fill[PIPE_BUF_SIZE];
    for (size_t i = 0; i < PIPE_BUF_SIZE; i++) fill[i] = (u8)(i & 0xff);
    TEST_EXPECT_EQ(dev_write(g_wr, fill, (long)PIPE_BUF_SIZE),
                   (long)PIPE_BUF_SIZE, "boot fills the buffer");

    struct Thread *cs[3];
    volatile long *rs[3] = { &g_consumer_result, &g_consumer2_result, &g_consumer3_result };
    cs[0] = thread_create(kproc(), consumer_write_one_byte_entry);
    cs[1] = thread_create(kproc(), consumer2_write_one_byte_entry);
    cs[2] = thread_create(kproc(), consumer3_write_one_byte_entry);
    TEST_ASSERT(cs[0] && cs[1] && cs[2], "thread_create x3");
    for (int i = 0; i < 3; i++) {
        ready(cs[i]);
        TEST_YIELD_UNTIL(cs[i]->state == THREAD_SLEEPING);
    }
    TEST_ASSERT(cs[0]->state == THREAD_SLEEPING && cs[1]->state == THREAD_SLEEPING
                && cs[2]->state == THREAD_SLEEPING,
        "all three writers SLEEPING on one full pipe (no second-sleeper extinction)");

    // ONE 1-byte drain: exactly one writer appends, two re-sample full + re-sleep.
    u8 one_drain[1];
    TEST_EXPECT_EQ(dev_read(g_rd, one_drain, 1L), 1L, "boot drains 1 byte");
    TEST_YIELD_UNTIL(*rs[0] != -999 || *rs[1] != -999 || *rs[2] != -999);
    for (int i = 0; i < 3; i++)
        if (*rs[i] == -999) TEST_YIELD_UNTIL(cs[i]->state == THREAD_SLEEPING);
    int wrote_idx = -1, asleep = 0, wrote_count = 0;
    for (int i = 0; i < 3; i++) {
        if (*rs[i] != -999) { wrote_idx = i; wrote_count++; }
        else if (cs[i]->state == THREAD_SLEEPING) asleep++;
    }
    long wrote_val = (wrote_idx >= 0) ? *rs[wrote_idx] : -1;

    // The wake-all edge: close the READ end with TWO writers still asleep ->
    // both return -T_E_PIPE.
    spoor_clunk(g_rd);
    for (int i = 0; i < 3; i++)
        if (i != wrote_idx) TEST_YIELD_UNTIL(*rs[i] != -999);
    int epipe_count = 0;
    for (int i = 0; i < 3; i++)
        if (i != wrote_idx && *rs[i] == (long)(-T_E_PIPE)) epipe_count++;

    for (int i = 0; i < 3; i++) thread_free(cs[i]);
    spoor_clunk(g_wr);

    TEST_EXPECT_EQ(wrote_count, 1, "exactly one writer appended after the drain");
    TEST_EXPECT_EQ(wrote_val, 1L, "...and it wrote its byte");
    TEST_EXPECT_EQ(asleep, 2, "RE-SAMPLE: the other two woke, found the ring full, and re-slept");
    TEST_EXPECT_EQ(epipe_count, 2,
        "WAKE-ALL: one close edge released BOTH remaining writers with EPIPE "
        "(a wake-one bug strands one on the full ring forever)");
}
