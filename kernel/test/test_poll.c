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

#include <thylacine/cons.h>
#include <thylacine/dev.h>
#include <thylacine/devsrv.h>
#include <thylacine/handle.h>
#include <thylacine/pipe.h>
#include <thylacine/poll.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/spoor.h>
#include <thylacine/srvconn.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

extern s64 sys_poll_for_proc(struct Proc *p, struct pollfd *kfds,
                             u64 nfds, s32 timeout_ms);

// devsrv test-support — non-static cores of the SYS_POST_SERVICE +
// SYS_SRV_ACCEPT syscall + the test-only registry reset.
extern int sys_srv_accept_for_proc(struct Proc *p, hidx_t service_h);
extern void srv_registry_reset(void);

// #103: the console Dev + its IRQ-side input hook + the test drive of the
// deferred-wake relay -- to exercise a real sys_poll_for_proc-blocked thread
// woken through the cons deferred path (nora's exact path).
extern struct Dev devcons;

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

void test_poll_devsrv_listener_immediate_pollin(void);
void test_poll_devsrv_listener_empty_not_ready(void);
void test_poll_devsrv_listener_block_then_wake(void);
void test_poll_devsrv_listener_pollhup_on_tombstone(void);
void test_poll_devsrv_conn_pollin_on_send(void);
void test_poll_devsrv_conn_pollout_immediate(void);
void test_poll_devsrv_conn_pollhup_on_teardown(void);
void test_poll_devsrv_conn_block_then_wake_pollin(void);
void test_poll_null_obj_spoor_pollnval(void);
void test_poll_mixed_spoor_and_srv(void);
void test_poll_max_nfds(void);

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

    // Reap the poll helper: it parked in a trailing sched() (RUNNABLE, never
    // returns from its entry). Without this it leaks as a runnable thread for
    // the rest of the boot -- the band-NORMAL half of the #857 quiescence
    // pollution. Matches test_cons / test_sched hygiene.
    thread_free(consumer);
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

    thread_free(consumer);          // reap the parked poll helper (see block_then_wake)
    drop_test_proc(p);
}

// #103 regression: a REAL sys_poll_for_proc-blocked thread woken through the
// cons DEFERRED relay -- nora's exact path (a non-owner child blocking in poll
// on an INHERITED /dev/cons fd, woken when a key arrives). This composes the
// two halves the existing tests cover only SEPARATELY:
//   - test_cons_poll_deferred_wake exercises the relay (cons_rx_input ->
//     poll_wake_pending -> cons_service_deferred -> poll_waiter_list_wake) but
//     against a SYNTHETIC waiter whose rendez has no sleeper (the wakeup is a
//     no-op -- it never proves a real thread resumes);
//   - test_poll_block_then_wake exercises a real sys_poll_for_proc-blocked
//     thread but woken by a pipe's SYNCHRONOUS poll_waiter_list_wake.
// Neither tests the relay reaching a genuinely-tsleep'd poller. A lost relay
// (the #103 symptom) leaves the poller SLEEPING forever.
void test_poll_cons_deferred_block_then_wake(void) {
    cons_test_reset();
    sched();                                   // settle console_mgr to SLEEPING

    struct Proc *p = make_test_proc();
    TEST_ASSERT(p != NULL, "test proc");

    // A devcons Spoor handle -- the same object nora inherits as fd 0 (the
    // inherit copies the handle; it does not re-open/re-gate). poll dispatches
    // sp->dev->poll == devcons_poll == cons_poll, no console-attach gate.
    struct Spoor *cs = devcons.attach(NULL);
    TEST_ASSERT(cs != NULL, "devcons attach");
    hidx_t hc = install_spoor(p, cs, RIGHT_READ);
    TEST_ASSERT(hc >= 0, "cons fd installed");

    g_pollee_proc  = p;
    g_pollee_fd    = hc;
    g_poll_result  = -999;
    g_poll_revents = 0;

    struct Thread *consumer = thread_create(kproc(), consumer_poll_forever_entry);
    TEST_ASSERT(consumer != NULL, "thread_create");
    ready(consumer);

    // Consumer enters sys_poll_for_proc on the cons fd: cons_poll registers a
    // waiter on g_cons.poll_list; the empty ring is not POLLIN-ready; tsleep.
    sched();
    TEST_EXPECT_EQ(consumer->state, THREAD_SLEEPING,
        "consumer SLEEPING in poll on /dev/cons");

    // Producer (the IRQ side): a data byte arms poll_wake_pending + wakes
    // console_mgr -- but does NOT walk the hook list (deferred, not IRQ-safe).
    // The real blocked poller MUST still be SLEEPING here.
    cons_rx_input((u8)'k', false);
    TEST_ASSERT(cons_test_pollwake_pending(), "data byte armed poll_wake_pending");
    TEST_EXPECT_EQ(consumer->state, THREAD_SLEEPING,
        "the IRQ producer did NOT wake the real poller (deferred)");

    // The deferred relay (what console_mgr runs in process context), driven
    // synchronously so the wake is independent of scheduler order. THIS is the
    // #103 crux: the relay must reach the sys_poll_for_proc-blocked thread, not
    // merely flip a synthetic flag.
    cons_test_service_deferred();
    TEST_EXPECT_EQ(consumer->state, THREAD_RUNNABLE,
        "the deferred relay woke the real /dev/cons poller (the #103 assertion)");
    TEST_ASSERT(!cons_test_pollwake_pending(), "relay consumed poll_wake_pending");

    // Let the consumer resume + record. console_mgr is also RUNNABLE (the
    // cons_rx_input wake) but re-sleeps on the now-drained cond; a bounded yield
    // loop runs both regardless of order.
    for (int i = 0; i < 4 && g_poll_result == -999; i++) sched();
    TEST_EXPECT_EQ(g_poll_result, 1L, "consumer's poll returns 1");
    TEST_EXPECT_EQ((s64)g_poll_revents, (s64)POLLIN, "consumer's revents = POLLIN");

    thread_free(consumer);
    cons_test_reset();
    sched();                                   // re-settle console_mgr
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
    TEST_EXPECT_EQ(sys_poll_for_proc(p, pfds, POLL_MAX_NFDS + 1, 0), -1L,
        "nfds > POLL_MAX_NFDS → -1");
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

// =============================================================================
// devsrv .poll — the second .poll implementor (P5-poll-b).
//
// Two surfaces:
//   - The listener: a KObj_Srv handle whose obj is a SrvService (the
//     handle SYS_POST_SERVICE returned). POLLIN ↔ backlog non-empty,
//     POLLHUP ↔ service tombstoned. The poll routes through
//     srv_handle_poll (which discriminates by the obj's magic).
//   - The connection Spoor: corvus's KObj_Spoor server endpoint, with
//     POLLIN/POLLOUT/POLLHUP/POLLERR. Routes through devsrv_poll →
//     srvconn_poll.
// =============================================================================

// Helper: a Proc joey-marked so it can post a /srv service.
static struct Proc *make_marked_test_proc(void) {
    struct Proc *p = proc_alloc();
    if (!p) return NULL;
    proc_mark_may_post_service(p);
    return p;
}

// post_svc_byte — post a byte-mode service into the boot registry via the
// production create=post path (devsrv_post_listener on a transient boot /srv
// root). Replaces the retired SYS_POST_SERVICE name-only entry (stalk-3c).
// Byte mode so the per-test connect (connect_byte) returns without a server
// handshake; the listener-poll readiness is mode-independent. Returns the
// listener handle (>= 0) or -1.
static int post_svc_byte(struct Proc *p, const char *name, size_t name_len) {
    struct Spoor *root = devsrv_attach_registry(srv_boot_registry());
    if (!root) return -1;
    int h = devsrv_post_listener(p, root, name, name_len, SRV_MODE_BYTE);
    spoor_clunk(root);
    return h;
}

// connect_byte — open=connect to a byte-mode /srv service: walk /srv/<name>
// to a service-ref Spoor, then devsrv_open_connect -> a CLIENT-direction
// byte-conn endpoint Spoor (CSRVCLIENT). Returns the conn Spoor or NULL; the
// caller owns it (wrap in a KOBJ_SPOOR handle or spoor_clunk). The connect
// pushes the conn onto the poster's accept backlog (waking the listener-poll
// list), the side effect these tests rely on. `name` must be NUL-terminated.
static struct Spoor *connect_byte(struct Proc *p, const char *name) {
    struct Spoor *root = devsrv_attach_registry(srv_boot_registry());
    if (!root) return NULL;
    struct Spoor *sref = spoor_clone(root);
    if (!sref) { spoor_clunk(root); return NULL; }
    const char *names[1] = { name };
    struct Walkqid *w = devsrv.walk(root, sref, names, 1);
    if (!w) { spoor_clunk(sref); spoor_clunk(root); return NULL; }
    walkqid_free(w);
    struct Spoor *cs = devsrv_open_connect(p, sref, /*omode ORDWR*/ 2);
    spoor_clunk(sref);                 // the spent quarry (open-returns-new)
    spoor_clunk(root);
    return cs;
}

void test_poll_devsrv_listener_immediate_pollin(void) {
    srv_registry_reset();

    struct Proc *corvus = make_marked_test_proc();
    TEST_ASSERT(corvus != NULL, "corvus proc");
    int svc_h = post_svc_byte(corvus, "corvus", 6);
    TEST_ASSERT(svc_h >= 0, "post \"corvus\"");

    // A client opens — one entry on the listener's accept backlog.
    struct Proc *client = make_test_proc();
    TEST_ASSERT(client != NULL, "client proc");
    struct Spoor *cs = connect_byte(client, "corvus");
    TEST_ASSERT(cs != NULL, "client open=connect to /srv/corvus");
    int client_h = handle_alloc(client, KOBJ_SPOOR, RIGHT_READ | RIGHT_WRITE, cs);
    TEST_ASSERT(client_h >= 0, "client connects");

    // corvus polls its listener handle (the KObj_Srv from SYS_POST_SERVICE).
    // Backlog non-empty → POLLIN ready immediately, no sleep.
    struct pollfd pfds[1] = {
        { .fd = svc_h, .events = POLLIN, .revents = 0 },
    };
    u64 before_slept = poll_total_slept();
    s64 ret = sys_poll_for_proc(corvus, pfds, 1, 0);
    TEST_EXPECT_EQ(ret, 1L, "listener-poll returns 1 (POLLIN ready)");
    TEST_EXPECT_EQ((s64)pfds[0].revents, (s64)POLLIN,
        "listener revents = POLLIN");
    TEST_EXPECT_EQ(poll_total_slept(), before_slept,
        "fast path — no tsleep");

    srv_registry_reset();
    drop_test_proc(client);
    drop_test_proc(corvus);
}

void test_poll_devsrv_listener_empty_not_ready(void) {
    srv_registry_reset();

    struct Proc *corvus = make_marked_test_proc();
    TEST_ASSERT(corvus != NULL, "corvus proc");
    int svc_h = post_svc_byte(corvus, "corvus", 6);
    TEST_ASSERT(svc_h >= 0, "post \"corvus\"");

    // Empty backlog + timeout=0 → return 0, no sleep.
    struct pollfd pfds[1] = {
        { .fd = svc_h, .events = POLLIN, .revents = 0 },
    };
    u64 before_slept = poll_total_slept();
    s64 ret = sys_poll_for_proc(corvus, pfds, 1, 0);
    TEST_EXPECT_EQ(ret, 0L, "empty listener not ready");
    TEST_EXPECT_EQ((s64)pfds[0].revents, 0L, "revents = 0");
    TEST_EXPECT_EQ(poll_total_slept(), before_slept,
        "timeout=0 did not sleep");

    srv_registry_reset();
    drop_test_proc(corvus);
}

// Block-then-wake harness state for the listener-poll thread.
static volatile s64 g_listener_poll_result;
static volatile s16 g_listener_poll_revents;
static struct Proc *g_listener_proc;
static hidx_t       g_listener_fd;

static void listener_poll_forever_entry(void) {
    struct pollfd pfds[1] = {
        { .fd = g_listener_fd, .events = POLLIN, .revents = 0 },
    };
    g_listener_poll_result  = sys_poll_for_proc(g_listener_proc, pfds, 1, -1);
    g_listener_poll_revents = pfds[0].revents;
    sched();    // park
}

void test_poll_devsrv_listener_block_then_wake(void) {
    srv_registry_reset();

    struct Proc *corvus = make_marked_test_proc();
    TEST_ASSERT(corvus != NULL, "corvus proc");
    int svc_h = post_svc_byte(corvus, "corvus", 6);
    TEST_ASSERT(svc_h >= 0, "post \"corvus\"");

    g_listener_proc         = corvus;
    g_listener_fd           = (hidx_t)svc_h;
    g_listener_poll_result  = -999;
    g_listener_poll_revents = 0;

    struct Thread *poller = thread_create(kproc(), listener_poll_forever_entry);
    TEST_ASSERT(poller != NULL, "thread_create");
    ready(poller);
    sched();
    TEST_EXPECT_EQ(poller->state, THREAD_SLEEPING,
        "listener-poll is SLEEPING on its private rendez");

    // Boot side: a client open=connects -> devsrv_open_connect enqueues +
    // wakes the listener poll list.
    struct Proc *client = make_test_proc();
    TEST_ASSERT(client != NULL, "client proc");
    struct Spoor *cs = connect_byte(client, "corvus");
    TEST_ASSERT(cs != NULL, "client open=connect to /srv/corvus");
    int client_h = handle_alloc(client, KOBJ_SPOOR, RIGHT_READ | RIGHT_WRITE, cs);
    TEST_ASSERT(client_h >= 0, "client connects → wakes the poll list");
    TEST_EXPECT_EQ(poller->state, THREAD_RUNNABLE,
        "listener-poll wakes after the connect");

    sched();
    TEST_EXPECT_EQ(g_listener_poll_result, 1L, "listener-poll returns 1");
    TEST_EXPECT_EQ((s64)g_listener_poll_revents, (s64)POLLIN,
        "wakeup revents = POLLIN");

    thread_free(poller);            // reap the parked poll helper (see block_then_wake)
    srv_registry_reset();
    drop_test_proc(client);
    drop_test_proc(corvus);
}

void test_poll_devsrv_listener_pollhup_on_tombstone(void) {
    srv_registry_reset();

    struct Proc *corvus = make_marked_test_proc();
    TEST_ASSERT(corvus != NULL, "corvus proc");
    int svc_h = post_svc_byte(corvus, "corvus", 6);
    TEST_ASSERT(svc_h >= 0, "post \"corvus\"");

    g_listener_proc         = corvus;
    g_listener_fd           = (hidx_t)svc_h;
    g_listener_poll_result  = -999;
    g_listener_poll_revents = 0;

    struct Thread *poller = thread_create(kproc(), listener_poll_forever_entry);
    TEST_ASSERT(poller != NULL, "thread_create");
    ready(poller);
    sched();
    TEST_EXPECT_EQ(poller->state, THREAD_SLEEPING,
        "listener-poll is SLEEPING");

    // Boot side: reset the registry, which tombstones every service and
    // wakes every listener poll list (the regression that proves
    // tombstone-as-readiness-edge — corvus's listener-poll should not
    // hang past its service's death).
    srv_registry_reset();
    TEST_EXPECT_EQ(poller->state, THREAD_RUNNABLE,
        "listener-poll wakes on the tombstone");

    sched();
    TEST_EXPECT_EQ(g_listener_poll_result, 1L, "listener-poll returns 1");
    TEST_ASSERT((g_listener_poll_revents & POLLHUP) != 0,
        "tombstone revents includes POLLHUP");

    thread_free(poller);            // reap the parked poll helper (see block_then_wake)
    drop_test_proc(corvus);
}

void test_poll_devsrv_conn_pollin_on_send(void) {
    srv_registry_reset();

    struct Proc *corvus = make_marked_test_proc();
    TEST_ASSERT(corvus != NULL, "corvus proc");
    int svc_h = post_svc_byte(corvus, "corvus", 6);
    TEST_ASSERT(svc_h >= 0, "post \"corvus\"");

    struct Proc *client = make_test_proc();
    TEST_ASSERT(client != NULL, "client proc");
    struct Spoor *cs = connect_byte(client, "corvus");
    TEST_ASSERT(cs != NULL, "client open=connect to /srv/corvus");
    int client_h = handle_alloc(client, KOBJ_SPOOR, RIGHT_READ | RIGHT_WRITE, cs);
    TEST_ASSERT(client_h >= 0, "client connects");
    struct SrvConn *cn = devsrv_conn_of(cs);
    int conn_h = sys_srv_accept_for_proc(corvus, (hidx_t)svc_h);
    TEST_ASSERT(conn_h >= 0, "corvus accepts");

    // The kernel-client side queues bytes on c2s → corvus's endpoint
    // becomes POLLIN-ready.
    static const u8 frame[4] = { 0x05, 0x00, 0x00, 0x00 };
    TEST_EXPECT_EQ(srvconn_client_send(cn, frame, 4), 4L,
        "client-side queues 4 bytes on c2s");

    struct pollfd pfds[1] = {
        { .fd = conn_h, .events = POLLIN, .revents = 0 },
    };
    u64 before_slept = poll_total_slept();
    s64 ret = sys_poll_for_proc(corvus, pfds, 1, 0);
    TEST_EXPECT_EQ(ret, 1L, "connection poll returns 1");
    TEST_EXPECT_EQ((s64)pfds[0].revents, (s64)POLLIN,
        "revents = POLLIN");
    TEST_EXPECT_EQ(poll_total_slept(), before_slept,
        "fast path — no tsleep");

    srv_registry_reset();
    drop_test_proc(client);
    drop_test_proc(corvus);
}

void test_poll_devsrv_conn_pollout_immediate(void) {
    srv_registry_reset();

    struct Proc *corvus = make_marked_test_proc();
    TEST_ASSERT(corvus != NULL, "corvus proc");
    int svc_h = post_svc_byte(corvus, "corvus", 6);
    TEST_ASSERT(svc_h >= 0, "post \"corvus\"");

    struct Proc *client = make_test_proc();
    TEST_ASSERT(client != NULL, "client proc");
    struct Spoor *cs = connect_byte(client, "corvus");
    TEST_ASSERT(cs != NULL, "client open=connect to /srv/corvus");
    int client_h = handle_alloc(client, KOBJ_SPOOR, RIGHT_READ | RIGHT_WRITE, cs);
    TEST_ASSERT(client_h >= 0, "client connects");
    int conn_h = sys_srv_accept_for_proc(corvus, (hidx_t)svc_h);
    TEST_ASSERT(conn_h >= 0, "corvus accepts");

    // An empty s2c ring → POLLOUT immediately. corvus can write.
    struct pollfd pfds[1] = {
        { .fd = conn_h, .events = POLLOUT, .revents = 0 },
    };
    s64 ret = sys_poll_for_proc(corvus, pfds, 1, 0);
    TEST_EXPECT_EQ(ret, 1L, "empty-s2c POLLOUT returns 1");
    TEST_EXPECT_EQ((s64)pfds[0].revents, (s64)POLLOUT,
        "revents = POLLOUT");

    handle_close(corvus, (hidx_t)conn_h);
    srv_registry_reset();
    drop_test_proc(client);
    drop_test_proc(corvus);
}

void test_poll_devsrv_conn_pollhup_on_teardown(void) {
    srv_registry_reset();

    struct Proc *corvus = make_marked_test_proc();
    TEST_ASSERT(corvus != NULL, "corvus proc");
    int svc_h = post_svc_byte(corvus, "corvus", 6);
    TEST_ASSERT(svc_h >= 0, "post \"corvus\"");

    struct Proc *client = make_test_proc();
    TEST_ASSERT(client != NULL, "client proc");
    struct Spoor *cs = connect_byte(client, "corvus");
    TEST_ASSERT(cs != NULL, "client open=connect to /srv/corvus");
    int client_h = handle_alloc(client, KOBJ_SPOOR, RIGHT_READ | RIGHT_WRITE, cs);
    TEST_ASSERT(client_h >= 0, "client connects");
    struct SrvConn *cn = devsrv_conn_of(cs);
    int conn_h = sys_srv_accept_for_proc(corvus, (hidx_t)svc_h);
    TEST_ASSERT(conn_h >= 0, "corvus accepts");

    // The client closes its handle → connection torn down → POLLHUP and
    // POLLERR latch on both directions.
    TEST_EXPECT_EQ(handle_close(client, (hidx_t)client_h), 0,
        "client closes — teardown latches EOF on both rings");
    TEST_ASSERT(srvconn_is_live(cn) == false, "connection torn down");

    struct pollfd pfds[1] = {
        { .fd = conn_h, .events = POLLIN | POLLOUT, .revents = 0 },
    };
    s64 ret = sys_poll_for_proc(corvus, pfds, 1, 0);
    TEST_EXPECT_EQ(ret, 1L, "torn-connection poll returns 1");
    TEST_ASSERT((pfds[0].revents & POLLHUP) != 0,
        "torn-connection revents includes POLLHUP");
    TEST_ASSERT((pfds[0].revents & POLLERR) != 0,
        "torn-connection revents includes POLLERR (s2c.eof)");

    handle_close(corvus, (hidx_t)conn_h);
    srv_registry_reset();
    drop_test_proc(corvus);
}

// Block-then-wake harness state for the connection-poll thread.
static volatile s64 g_conn_poll_result;
static volatile s16 g_conn_poll_revents;
static struct Proc *g_conn_poll_proc;
static hidx_t       g_conn_poll_fd;

static void conn_poll_forever_entry(void) {
    struct pollfd pfds[1] = {
        { .fd = g_conn_poll_fd, .events = POLLIN, .revents = 0 },
    };
    g_conn_poll_result  = sys_poll_for_proc(g_conn_poll_proc, pfds, 1, -1);
    g_conn_poll_revents = pfds[0].revents;
    sched();    // park
}

void test_poll_devsrv_conn_block_then_wake_pollin(void) {
    srv_registry_reset();

    struct Proc *corvus = make_marked_test_proc();
    TEST_ASSERT(corvus != NULL, "corvus proc");
    int svc_h = post_svc_byte(corvus, "corvus", 6);
    TEST_ASSERT(svc_h >= 0, "post \"corvus\"");

    struct Proc *client = make_test_proc();
    TEST_ASSERT(client != NULL, "client proc");
    struct Spoor *cs = connect_byte(client, "corvus");
    TEST_ASSERT(cs != NULL, "client open=connect to /srv/corvus");
    int client_h = handle_alloc(client, KOBJ_SPOOR, RIGHT_READ | RIGHT_WRITE, cs);
    TEST_ASSERT(client_h >= 0, "client connects");
    struct SrvConn *cn = devsrv_conn_of(cs);
    int conn_h = sys_srv_accept_for_proc(corvus, (hidx_t)svc_h);
    TEST_ASSERT(conn_h >= 0, "corvus accepts");

    g_conn_poll_proc    = corvus;
    g_conn_poll_fd      = (hidx_t)conn_h;
    g_conn_poll_result  = -999;
    g_conn_poll_revents = 0;

    struct Thread *poller = thread_create(kproc(), conn_poll_forever_entry);
    TEST_ASSERT(poller != NULL, "thread_create");
    ready(poller);
    sched();
    TEST_EXPECT_EQ(poller->state, THREAD_SLEEPING,
        "connection-poll is SLEEPING — c2s empty");

    // Boot side: the kernel-client side queues bytes → POLLIN edge wakes
    // the poller.
    static const u8 frame[3] = { 0xDE, 0xAD, 0xBE };
    TEST_EXPECT_EQ(srvconn_client_send(cn, frame, 3), 3L,
        "client-side queues 3 bytes");
    TEST_EXPECT_EQ(poller->state, THREAD_RUNNABLE,
        "connection-poll wakes on the send");

    sched();
    TEST_EXPECT_EQ(g_conn_poll_result, 1L, "connection-poll returns 1");
    TEST_EXPECT_EQ((s64)g_conn_poll_revents, (s64)POLLIN,
        "wakeup revents = POLLIN");

    thread_free(poller);            // reap the parked poll helper (see block_then_wake)
    srv_registry_reset();
    drop_test_proc(client);
    drop_test_proc(corvus);
}

// =============================================================================
// Regression + coverage (P5-poll audit close #538).
// =============================================================================

void test_poll_null_obj_spoor_pollnval(void) {
    // F4 regression: a KObj_Spoor handle with NULL obj must report POLLNVAL,
    // not "always-ready." Allocating a malformed handle exercises the
    // poll_scan_one NULL-Spoor branch.
    struct Proc *p = make_test_proc();
    TEST_ASSERT(p != NULL, "test proc");

    hidx_t h = handle_alloc(p, KOBJ_SPOOR, RIGHT_READ, NULL);
    TEST_ASSERT(h >= 0, "NULL-obj spoor handle allocated");

    struct pollfd pfds[1] = {
        { .fd = h, .events = POLLIN | POLLOUT, .revents = 0 },
    };
    s64 ret = sys_poll_for_proc(p, pfds, 1, 0);
    TEST_EXPECT_EQ(ret, 1L, "POLLNVAL counts as ready");
    TEST_EXPECT_EQ((s64)pfds[0].revents, (s64)POLLNVAL,
        "NULL-obj KObj_Spoor → POLLNVAL (not always-ready)");

    drop_test_proc(p);
}

void test_poll_mixed_spoor_and_srv(void) {
    // Cross-kobj-kind coverage: poll one KObj_Spoor (a pipe read end) +
    // one KObj_Srv (corvus's listener) in a single call. Both unready
    // → returns 0 with no sleep; then make the listener ready via a
    // client connect → re-poll returns 1 with only the listener flagged.
    srv_registry_reset();

    struct Proc *corvus = make_marked_test_proc();
    TEST_ASSERT(corvus != NULL, "corvus proc");
    int svc_h = post_svc_byte(corvus, "corvus", 6);
    TEST_ASSERT(svc_h >= 0, "post \"corvus\"");

    struct Spoor *rd = NULL, *wr = NULL;
    TEST_EXPECT_EQ(pipe_create(&rd, &wr), 0, "pipe_create");
    hidx_t hrd = install_spoor(corvus, rd, RIGHT_READ);
    hidx_t hwr = install_spoor(corvus, wr, RIGHT_WRITE);
    TEST_ASSERT(hrd >= 0 && hwr >= 0, "pipe fds installed");
    (void)hwr;

    struct pollfd pfds[2] = {
        { .fd = hrd,   .events = POLLIN, .revents = 0 },
        { .fd = svc_h, .events = POLLIN, .revents = 0 },
    };
    s64 ret = sys_poll_for_proc(corvus, pfds, 2, 0);
    TEST_EXPECT_EQ(ret, 0L, "both unready → 0");
    TEST_EXPECT_EQ((s64)pfds[0].revents, 0L, "spoor not ready");
    TEST_EXPECT_EQ((s64)pfds[1].revents, 0L, "listener not ready");

    // Make the listener ready (client connects -> backlog non-empty).
    struct Proc *client = make_test_proc();
    TEST_ASSERT(client != NULL, "client proc");
    struct Spoor *cs = connect_byte(client, "corvus");
    TEST_ASSERT(cs != NULL, "client connects");
    int client_h = handle_alloc(client, KOBJ_SPOOR, RIGHT_READ | RIGHT_WRITE, cs);
    TEST_ASSERT(client_h >= 0, "client conn endpoint installed");

    pfds[0].revents = 0;
    pfds[1].revents = 0;
    ret = sys_poll_for_proc(corvus, pfds, 2, 0);
    TEST_EXPECT_EQ(ret, 1L, "one ready → 1");
    TEST_EXPECT_EQ((s64)pfds[0].revents, 0L, "spoor still not ready");
    TEST_EXPECT_EQ((s64)pfds[1].revents, (s64)POLLIN,
        "listener POLLIN ready (backlog non-empty)");

    srv_registry_reset();
    drop_test_proc(client);
    drop_test_proc(corvus);
}

void test_poll_max_nfds(void) {
    // Boundary coverage: nfds = POLL_MAX_NFDS = 64 (the poll-at-once cap,
    // decoupled from PROC_HANDLE_MAX so the larger fd table cannot
    // blow the waiters[]/held[] kstack frame). Exercises the stack
    // allocation of `waiters[64]` and the per-fd scan loop's bound.
    // All-POLLNVAL (cheapest setup) — the sweep walks all 64.
    struct Proc *p = make_test_proc();
    TEST_ASSERT(p != NULL, "test proc");

    struct pollfd pfds[POLL_MAX_NFDS];
    for (u32 i = 0; i < POLL_MAX_NFDS; i++) {
        pfds[i].fd      = (s32)i;       // unallocated → POLLNVAL
        pfds[i].events  = POLLIN;
        pfds[i].revents = 0;
    }
    s64 ret = sys_poll_for_proc(p, pfds, POLL_MAX_NFDS, 0);
    TEST_EXPECT_EQ(ret, (s64)POLL_MAX_NFDS,
        "every fd POLLNVAL → POLL_MAX_NFDS ready");
    bool all_pollnval = true;
    for (u32 i = 0; i < POLL_MAX_NFDS; i++) {
        if (pfds[i].revents != POLLNVAL) { all_pollnval = false; break; }
    }
    TEST_ASSERT(all_pollnval, "every revents = POLLNVAL");

    drop_test_proc(p);
}
