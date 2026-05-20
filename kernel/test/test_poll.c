// poll mechanism + SYS_POLL tests (P5-poll-a).
//
// Exercises:
//   - The register-then-observe discipline at the first scan (any fd
//     ready → return without sleeping).
//   - The sleep + wake-on-readiness slow path (consumer thread sleeps in
//     tsleep; boot writes / closes → consumer wakes with the right
//     revents).
//   - Timeout 0 (non-blocking probe) / timeout > 0 (deadline fires) /
//     timeout < 0 (infinite, woken by an event).
//   - Multi-fd readiness (one of N ready returns 1 with the right
//     pollfd flagged).
//   - POLLHUP on the surviving end after the other endpoint closes.
//   - POLLNVAL for an invalid fd; POLLERR for a write-end whose read
//     end has closed.
//   - The NULL Dev.poll slot — always-ready POSIX-correct path.
//   - Bad-argument rejection (nfds == 0, nfds > PROC_HANDLE_MAX).
//
// Tests exercise `sys_poll_for_proc` (the testable core) directly; the
// SYS_POLL user-VA wrapper is exercised by the integration probe.

#include "test.h"

#include <thylacine/dev.h>
#include <thylacine/handle.h>
#include <thylacine/pipe.h>
#include <thylacine/poll.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/spoor.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

extern s64 sys_poll_for_proc(struct Proc *p, struct pollfd *kfds,
                             u64 nfds, s32 timeout_ms);

void test_poll_ready_immediately_pollin(void);
void test_poll_ready_immediately_pollout(void);
void test_poll_timeout_zero_not_ready(void);
void test_poll_timeout_positive_fires(void);
void test_poll_block_then_wake_pollin(void);
void test_poll_pollhup_on_close_write_end(void);
void test_poll_multi_fd_one_ready(void);
void test_poll_bad_fd_revents_pollnval(void);
void test_poll_bad_args_rejected(void);
void test_poll_always_ready_null_dev_poll(void);
void test_poll_pollerr_on_write_after_read_close(void);
void test_poll_unregister_after_fast_path(void);

// =============================================================================
// Helpers: test Proc + per-test Spoor → fd installation.
// =============================================================================

static struct Proc *make_test_proc(void) {
    struct Proc *p = proc_alloc();
    return p;
}

static void drop_test_proc(struct Proc *p) {
    if (!p) return;
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// Install a Spoor into p's handle table by TRANSFERRING the caller's
// existing reference: handle_alloc does not bump, so after a successful
// install the handle owns the ref and `sp` becomes a borrowed pointer
// (valid only until the handle is closed). Mirrors the
// test_attach_probe install pattern. Returns the hidx or -1 on failure.
static hidx_t install_spoor(struct Proc *p, struct Spoor *sp, rights_t r) {
    if (!sp) return (hidx_t)-1;
    return handle_alloc(p, KOBJ_SPOOR, r, sp);
}

// =============================================================================
// Fast-path tests — no sleep.
// =============================================================================

void test_poll_ready_immediately_pollin(void) {
    struct Proc *p = make_test_proc();
    TEST_ASSERT(p != NULL, "test proc");

    struct Spoor *rd = NULL, *wr = NULL;
    TEST_EXPECT_EQ(pipe_create(&rd, &wr), 0, "pipe_create");

    hidx_t hrd = install_spoor(p, rd, RIGHT_READ);
    hidx_t hwr = install_spoor(p, wr, RIGHT_WRITE);
    TEST_ASSERT(hrd >= 0 && hwr >= 0, "fds installed");

    // Pre-write a byte so POLLIN is ready at the first scan.
    static const u8 payload = 0x55;
    long n = wr->dev->write(wr, &payload, 1, 0);
    TEST_EXPECT_EQ(n, 1L, "wrote payload");

    struct pollfd pfds[1] = {
        { .fd = hrd, .events = POLLIN, .revents = 0 },
    };
    u64 before_slept = poll_total_slept();
    s64 ret = sys_poll_for_proc(p, pfds, 1, 0);
    TEST_EXPECT_EQ(ret, 1L, "poll returns 1 (immediate ready)");
    TEST_EXPECT_EQ((s64)pfds[0].revents, (s64)POLLIN, "revents = POLLIN");
    TEST_EXPECT_EQ(poll_total_slept(), before_slept,
        "fast path did NOT sleep");

    drop_test_proc(p);
}

void test_poll_ready_immediately_pollout(void) {
    struct Proc *p = make_test_proc();
    struct Spoor *rd = NULL, *wr = NULL;
    TEST_EXPECT_EQ(pipe_create(&rd, &wr), 0, "pipe_create");

    hidx_t hrd = install_spoor(p, rd, RIGHT_READ);
    hidx_t hwr = install_spoor(p, wr, RIGHT_WRITE);
    TEST_ASSERT(hrd >= 0 && hwr >= 0, "fds installed");

    // Empty buffer → POLLOUT is ready on the write end immediately.
    struct pollfd pfds[1] = {
        { .fd = hwr, .events = POLLOUT, .revents = 0 },
    };
    s64 ret = sys_poll_for_proc(p, pfds, 1, 0);
    TEST_EXPECT_EQ(ret, 1L, "poll returns 1 (POLLOUT ready)");
    TEST_EXPECT_EQ((s64)pfds[0].revents, (s64)POLLOUT,
        "revents = POLLOUT");

    drop_test_proc(p);
}

void test_poll_timeout_zero_not_ready(void) {
    struct Proc *p = make_test_proc();
    struct Spoor *rd = NULL, *wr = NULL;
    TEST_EXPECT_EQ(pipe_create(&rd, &wr), 0, "pipe_create");

    hidx_t hrd = install_spoor(p, rd, RIGHT_READ);
    hidx_t hwr = install_spoor(p, wr, RIGHT_WRITE);
    TEST_ASSERT(hrd >= 0 && hwr >= 0, "fds installed");

    // Empty pipe → POLLIN NOT ready. timeout=0 → return 0 immediately.
    struct pollfd pfds[1] = {
        { .fd = hrd, .events = POLLIN, .revents = 0 },
    };
    u64 before_slept = poll_total_slept();
    s64 ret = sys_poll_for_proc(p, pfds, 1, 0);
    TEST_EXPECT_EQ(ret, 0L, "poll returns 0 (timeout, no ready)");
    TEST_EXPECT_EQ((s64)pfds[0].revents, 0L, "revents = 0");
    TEST_EXPECT_EQ(poll_total_slept(), before_slept,
        "timeout=0 did NOT sleep");

    drop_test_proc(p);
}

void test_poll_timeout_positive_fires(void) {
    struct Proc *p = make_test_proc();
    struct Spoor *rd = NULL, *wr = NULL;
    TEST_EXPECT_EQ(pipe_create(&rd, &wr), 0, "pipe_create");

    hidx_t hrd = install_spoor(p, rd, RIGHT_READ);
    hidx_t hwr = install_spoor(p, wr, RIGHT_WRITE);
    TEST_ASSERT(hrd >= 0 && hwr >= 0, "fds installed");

    // Empty pipe + timeout=10ms → tsleep fires; return 0.
    struct pollfd pfds[1] = {
        { .fd = hrd, .events = POLLIN, .revents = 0 },
    };
    u64 before_slept = poll_total_slept();
    s64 ret = sys_poll_for_proc(p, pfds, 1, 10);
    TEST_EXPECT_EQ(ret, 0L, "poll returns 0 after timeout");
    TEST_EXPECT_EQ((s64)pfds[0].revents, 0L, "revents = 0");
    TEST_ASSERT(poll_total_slept() > before_slept,
        "tsleep path was taken (positive timeout, no immediate ready)");

    drop_test_proc(p);
}

// =============================================================================
// Slow-path tests — consumer thread + a wake event from boot.
// =============================================================================

static volatile s64    g_poll_result;
static volatile s16    g_poll_revents;
static struct Proc    *g_pollee_proc;
static hidx_t          g_pollee_fd;

// Block on a single fd with timeout=-1 (forever). Record poll's return
// + the revents of pollfds[0], then park.
static void consumer_poll_forever_entry(void) {
    struct pollfd pfds[1] = {
        { .fd = g_pollee_fd, .events = POLLIN, .revents = 0 },
    };
    g_poll_result  = sys_poll_for_proc(g_pollee_proc, pfds, 1, -1);
    g_poll_revents = pfds[0].revents;
    sched();    // park
}

void test_poll_block_then_wake_pollin(void) {
    struct Proc *p = make_test_proc();
    struct Spoor *rd = NULL, *wr = NULL;
    TEST_EXPECT_EQ(pipe_create(&rd, &wr), 0, "pipe_create");

    hidx_t hrd = install_spoor(p, rd, RIGHT_READ);
    hidx_t hwr = install_spoor(p, wr, RIGHT_WRITE);
    TEST_ASSERT(hrd >= 0 && hwr >= 0, "fds installed");

    g_pollee_proc  = p;
    g_pollee_fd    = hrd;
    g_poll_result  = -999;
    g_poll_revents = 0;

    struct Thread *consumer = thread_create(kproc(), consumer_poll_forever_entry);
    TEST_ASSERT(consumer != NULL, "thread_create");
    ready(consumer);
    // Yield. Consumer enters sys_poll_for_proc, scans (no data), tsleep
    // on its private rendez.
    sched();
    TEST_EXPECT_EQ(consumer->state, THREAD_SLEEPING,
        "consumer SLEEPING in poll");

    // Boot side: write a byte to the pipe. devpipe_write calls
    // poll_waiter_list_wake; the consumer's poll_waiter fires, the
    // private rendez signals, consumer transitions to RUNNABLE.
    static const u8 payload = 0xAB;
    long n = wr->dev->write(wr, &payload, 1, 0);
    TEST_EXPECT_EQ(n, 1L, "boot writes 1 byte");
    TEST_EXPECT_EQ(consumer->state, THREAD_RUNNABLE,
        "consumer wakes to RUNNABLE after poll-list signal");

    sched();
    TEST_EXPECT_EQ(g_poll_result, 1L, "consumer's poll returns 1");
    TEST_EXPECT_EQ((s64)g_poll_revents, (s64)POLLIN,
        "consumer's revents = POLLIN");

    drop_test_proc(p);
}

void test_poll_pollhup_on_close_write_end(void) {
    struct Proc *p = make_test_proc();
    struct Spoor *rd = NULL, *wr = NULL;
    TEST_EXPECT_EQ(pipe_create(&rd, &wr), 0, "pipe_create");

    hidx_t hrd = install_spoor(p, rd, RIGHT_READ);
    hidx_t hwr = install_spoor(p, wr, RIGHT_WRITE);
    TEST_ASSERT(hrd >= 0 && hwr >= 0, "fds installed");

    g_pollee_proc  = p;
    g_pollee_fd    = hrd;
    g_poll_result  = -999;
    g_poll_revents = 0;

    struct Thread *consumer = thread_create(kproc(), consumer_poll_forever_entry);
    TEST_ASSERT(consumer != NULL, "thread_create");
    ready(consumer);
    sched();
    TEST_EXPECT_EQ(consumer->state, THREAD_SLEEPING,
        "consumer SLEEPING in poll");

    // Boot side: close the write end via the handle table — drops the
    // ring's ref, marks write_eof, walks poll list, wakes consumer.
    TEST_EXPECT_EQ(handle_close(p, hwr), 0, "close write end");
    TEST_EXPECT_EQ(consumer->state, THREAD_RUNNABLE,
        "consumer wakes after write-end close");

    sched();
    TEST_EXPECT_EQ(g_poll_result, 1L, "consumer's poll returns 1");
    TEST_ASSERT((g_poll_revents & POLLHUP) != 0,
        "consumer's revents includes POLLHUP (POSIX hang-up)");

    drop_test_proc(p);
}

// =============================================================================
// Multi-fd + corner-case tests.
// =============================================================================

void test_poll_multi_fd_one_ready(void) {
    struct Proc *p = make_test_proc();

    // Three pipes; only the middle one has data.
    struct Spoor *rds[3] = {0}, *wrs[3] = {0};
    hidx_t hrds[3], hwrs[3];
    for (int i = 0; i < 3; i++) {
        TEST_EXPECT_EQ(pipe_create(&rds[i], &wrs[i]), 0, "pipe_create");
        hrds[i] = install_spoor(p, rds[i], RIGHT_READ);
        hwrs[i] = install_spoor(p, wrs[i], RIGHT_WRITE);
        TEST_ASSERT(hrds[i] >= 0 && hwrs[i] >= 0, "fds installed");
    }
    static const u8 payload = 0x11;
    long n = wrs[1]->dev->write(wrs[1], &payload, 1, 0);
    TEST_EXPECT_EQ(n, 1L, "wrote to pipe 1");

    struct pollfd pfds[3] = {
        { .fd = hrds[0], .events = POLLIN, .revents = 0 },
        { .fd = hrds[1], .events = POLLIN, .revents = 0 },
        { .fd = hrds[2], .events = POLLIN, .revents = 0 },
    };
    s64 ret = sys_poll_for_proc(p, pfds, 3, 0);
    TEST_EXPECT_EQ(ret, 1L, "poll returns 1 (only one ready)");
    TEST_EXPECT_EQ((s64)pfds[0].revents, 0L, "pfd 0 not ready");
    TEST_EXPECT_EQ((s64)pfds[1].revents, (s64)POLLIN, "pfd 1 ready");
    TEST_EXPECT_EQ((s64)pfds[2].revents, 0L, "pfd 2 not ready");

    drop_test_proc(p);
}

void test_poll_bad_fd_revents_pollnval(void) {
    struct Proc *p = make_test_proc();

    // hidx 0 has not been allocated — handle_get returns NULL → POLLNVAL.
    struct pollfd pfds[2] = {
        { .fd = 0,                          .events = POLLIN, .revents = 0 },
        { .fd = (s32)PROC_HANDLE_MAX + 100, .events = POLLIN, .revents = 0 },
    };
    s64 ret = sys_poll_for_proc(p, pfds, 2, 0);
    TEST_EXPECT_EQ(ret, 2L, "two POLLNVAL fds count as ready");
    TEST_EXPECT_EQ((s64)pfds[0].revents, (s64)POLLNVAL,
        "unallocated hidx → POLLNVAL");
    TEST_EXPECT_EQ((s64)pfds[1].revents, (s64)POLLNVAL,
        "out-of-range hidx → POLLNVAL");

    drop_test_proc(p);
}

void test_poll_bad_args_rejected(void) {
    struct Proc *p = make_test_proc();
    struct pollfd pfds[1] = {{ .fd = 0, .events = POLLIN, .revents = 0 }};

    TEST_EXPECT_EQ(sys_poll_for_proc(p, pfds, 0, 0), -1L,
        "nfds == 0 → -1");
    TEST_EXPECT_EQ(sys_poll_for_proc(p, pfds, PROC_HANDLE_MAX + 1, 0), -1L,
        "nfds > PROC_HANDLE_MAX → -1");
    TEST_EXPECT_EQ(sys_poll_for_proc(NULL, pfds, 1, 0), -1L,
        "p == NULL → -1");
    TEST_EXPECT_EQ(sys_poll_for_proc(p, NULL, 1, 0), -1L,
        "kfds == NULL → -1");

    drop_test_proc(p);
}

void test_poll_always_ready_null_dev_poll(void) {
    struct Proc *p = make_test_proc();

    // /dev/null has no readiness state — its Dev.poll slot is NULL.
    // poll on a devnull Spoor must report POLLIN | POLLOUT immediately
    // (POSIX-correct "always ready" for a regular file).
    struct Spoor *nul = devnull.attach("");
    TEST_ASSERT(nul != NULL, "devnull attach");
    hidx_t h = install_spoor(p, nul, RIGHT_READ | RIGHT_WRITE);
    TEST_ASSERT(h >= 0, "devnull fd installed");

    struct pollfd pfds[1] = {
        { .fd = h, .events = POLLIN | POLLOUT, .revents = 0 },
    };
    u64 before_slept = poll_total_slept();
    s64 ret = sys_poll_for_proc(p, pfds, 1, 0);
    TEST_EXPECT_EQ(ret, 1L, "always-ready fd returns 1");
    TEST_EXPECT_EQ((s64)pfds[0].revents, (s64)(POLLIN | POLLOUT),
        "NULL .poll → revents = requested POLLIN|POLLOUT");
    TEST_EXPECT_EQ(poll_total_slept(), before_slept,
        "always-ready fd did NOT sleep");

    drop_test_proc(p);
}

void test_poll_pollerr_on_write_after_read_close(void) {
    struct Proc *p = make_test_proc();
    struct Spoor *rd = NULL, *wr = NULL;
    TEST_EXPECT_EQ(pipe_create(&rd, &wr), 0, "pipe_create");

    hidx_t hrd = install_spoor(p, rd, RIGHT_READ);
    hidx_t hwr = install_spoor(p, wr, RIGHT_WRITE);
    TEST_ASSERT(hrd >= 0 && hwr >= 0, "fds installed");

    // Close the read end. The write end now polls POLLERR (no reader
    // — a write would EPIPE).
    TEST_EXPECT_EQ(handle_close(p, hrd), 0, "close read end");

    struct pollfd pfds[1] = {
        { .fd = hwr, .events = POLLOUT, .revents = 0 },
    };
    s64 ret = sys_poll_for_proc(p, pfds, 1, 0);
    TEST_EXPECT_EQ(ret, 1L, "poll returns 1");
    TEST_ASSERT((pfds[0].revents & POLLERR) != 0,
        "write end's revents includes POLLERR after read close");

    drop_test_proc(p);
}

void test_poll_unregister_after_fast_path(void) {
    // Regression: a fast-path return must unregister every hook so a
    // subsequent poll on the same fd works AND no stale hook fires.
    // We poll twice in a row; the second call must succeed identically.
    struct Proc *p = make_test_proc();
    struct Spoor *rd = NULL, *wr = NULL;
    TEST_EXPECT_EQ(pipe_create(&rd, &wr), 0, "pipe_create");
    hidx_t hrd = install_spoor(p, rd, RIGHT_READ);
    hidx_t hwr = install_spoor(p, wr, RIGHT_WRITE);
    TEST_ASSERT(hrd >= 0 && hwr >= 0, "fds installed");

    static const u8 payload = 0x99;
    long n = wr->dev->write(wr, &payload, 1, 0);
    TEST_EXPECT_EQ(n, 1L, "wrote payload");

    struct pollfd pfds[1] = {
        { .fd = hrd, .events = POLLIN, .revents = 0 },
    };
    TEST_EXPECT_EQ(sys_poll_for_proc(p, pfds, 1, 0), 1L, "first poll");
    pfds[0].revents = 0;
    TEST_EXPECT_EQ(sys_poll_for_proc(p, pfds, 1, 0), 1L, "second poll");
    TEST_EXPECT_EQ((s64)pfds[0].revents, (s64)POLLIN,
        "second poll's revents = POLLIN");

    drop_test_proc(p);
}
