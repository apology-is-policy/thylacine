// Kernel pipe blocking-mode tests (P5-pipe-blocking).
//
// Exercises the wait/wake protocol per `specs/pipe.tla`. Each test
// composes a boot thread + one consumer thread. The consumer either:
//   - reads from an empty pipe (sleeps on read_rendez; boot writes
//     or closes the write end to wake it), or
//   - writes to a full pipe (sleeps on write_rendez; boot reads or
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

#include "test.h"

#include <thylacine/dev.h>
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
    // → sleeps on read_rendez. Scheduler picks boot again; we resume.
    sched();
    TEST_EXPECT_EQ(consumer->state, THREAD_SLEEPING,
        "consumer should be SLEEPING after reaching dev_read on empty");

    // Boot side: write 5 bytes. devpipe_write appends + wakes
    // read_rendez. Consumer transitions to RUNNABLE.
    const u8 payload[] = { 0x11, 0x22, 0x33, 0x44, 0x55 };
    TEST_EXPECT_EQ(dev_write(g_wr, payload, (long)sizeof(payload)),
                   (long)sizeof(payload),
        "boot writes payload");
    TEST_EXPECT_EQ(consumer->state, THREAD_RUNNABLE,
        "consumer wakes to RUNNABLE after write");

    // Yield. Consumer resumes inside sleep's loop; cond_can_read TRUE
    // (count > 0); sleep returns; loop re-takes lock; drains; wakes
    // (no waiting writer — no-op); returns. Consumer sets
    // g_consumer_result + sched()s back.
    sched();
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
    sched();
    TEST_EXPECT_EQ(consumer->state, THREAD_SLEEPING,
        "consumer should be SLEEPING after reaching dev_write on full");

    // Boot drains 10 bytes — makes space — wakes write_rendez.
    u8 drain[10];
    TEST_EXPECT_EQ(dev_read(g_rd, drain, 10L), 10L, "boot drains 10 bytes");
    TEST_EXPECT_EQ(consumer->state, THREAD_RUNNABLE,
        "consumer wakes to RUNNABLE after read");

    sched();
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
    sched();
    TEST_EXPECT_EQ(consumer->state, THREAD_SLEEPING,
        "consumer SLEEPING on empty read");

    // Boot closes the write end. devpipe_close sets write_eof + wakes
    // read_rendez. Consumer wakes; sees write_eof; returns 0 (EOF).
    spoor_clunk(g_wr);
    TEST_EXPECT_EQ(consumer->state, THREAD_RUNNABLE,
        "consumer wakes to RUNNABLE after close");

    sched();
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
    sched();
    TEST_EXPECT_EQ(consumer->state, THREAD_SLEEPING,
        "consumer SLEEPING on full write");

    // Boot closes the read end. devpipe_close sets read_eof + wakes
    // write_rendez. Consumer wakes; sees read_eof; returns -1 (EPIPE).
    spoor_clunk(g_rd);
    TEST_EXPECT_EQ(consumer->state, THREAD_RUNNABLE,
        "consumer wakes to RUNNABLE after close");

    sched();
    TEST_EXPECT_EQ(g_consumer_result, -1L,
        "consumer write returns -1 (EPIPE) after read end closed");

    thread_free(consumer);          // reap the parked helper (see write_wakes)
    spoor_clunk(g_wr);
}
