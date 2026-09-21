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

// V-5c-2: poll.sleep_for_waits MEASURES the sleep, so it needs the clock.
#include "../../arch/arm64/timer.h"

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
void test_poll_devsrv_client_row(void);
void test_poll_devsrv_client_wakes_on_reply_only(void);
void test_poll_devsrv_server_pollout_wakes_on_client_drain(void);
void test_poll_devsrv_client_pollout_wakes_on_server_blocking_drain(void);
void test_poll_devsrv_client_kernel_attached_pollnval(void);
void test_poll_devsrv_client_wakes_on_teardown(void);
void test_poll_timeout_survives_a_busy_list(void);
void test_poll_death_ends_a_noise_driven_poll(void);
void test_poll_stop_parks_a_noise_driven_poll(void);
void test_poll_backstop_sleeps_through_noise(void);
void test_poll_backstop_keeps_the_deadline(void);
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
static volatile bool   g_poll_exited;
static struct Proc    *g_pollee_proc;
static hidx_t          g_pollee_fd;

// Block on a single fd with timeout=-1 (forever). Record poll's return
// + the revents of pollfds[0], then park TERMINALLY. Every poller entry here
// publishes its detail fields first and its result LAST, with a release store
// the waiters acquire-load: a waiter that sees the result then reads the rest.
// A trailing bare sched() was a YIELD: the helper stayed RUNNABLE, an idle peer
// could steal and run it, and the thread_free that reaped it raced a running
// thread (B-0 audit round 5 F2/F3).
static void consumer_poll_forever_entry(void) {
    struct pollfd pfds[1] = {
        { .fd = g_pollee_fd, .events = POLLIN, .revents = 0 },
    };
    s64 r = sys_poll_for_proc(g_pollee_proc, pfds, 1, -1);
    g_poll_revents = pfds[0].revents;
    __atomic_store_n(&g_poll_result, r, __ATOMIC_RELEASE);
    test_kthread_park_terminal(&g_poll_exited);
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
    g_poll_result  = -999; g_poll_exited = false;
    g_poll_revents = 0;

    struct Thread *consumer = thread_create(kproc(), consumer_poll_forever_entry);
    TEST_ASSERT(consumer != NULL, "thread_create");
    ready(consumer);
    // Yield. Consumer enters sys_poll_for_proc, scans (no data), tsleep
    // on its private rendez.
    TEST_YIELD_UNTIL(consumer->state == THREAD_SLEEPING);
    TEST_EXPECT_EQ(consumer->state, THREAD_SLEEPING,
        "consumer SLEEPING in poll");

    // Boot side: write a byte to the pipe. devpipe_write calls
    // poll_waiter_list_wake; the consumer's poll_waiter fires, the
    // private rendez signals, consumer transitions to RUNNABLE.
    static const u8 payload = 0xAB;
    long n = wr->dev->write(wr, &payload, 1, 0);
    TEST_EXPECT_EQ(n, 1L, "boot writes 1 byte");
    TEST_EXPECT_NE(consumer->state, THREAD_SLEEPING,
        "consumer left the rendez after poll-list signal");

    TEST_YIELD_UNTIL(__atomic_load_n(&g_poll_result, __ATOMIC_ACQUIRE) != -999);
    TEST_EXPECT_EQ(g_poll_result, 1L, "consumer's poll returns 1");
    TEST_EXPECT_EQ((s64)g_poll_revents, (s64)POLLIN,
        "consumer's revents = POLLIN");

    // Reap the poll helper: it parked in a trailing sched() (RUNNABLE, never
    // returns from its entry). Without this it leaks as a runnable thread for
    // the rest of the boot -- the band-NORMAL half of the #857 quiescence
    // pollution. Matches test_cons / test_sched hygiene.
    test_kthread_join_free(consumer, &g_poll_exited);
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
    g_poll_result  = -999; g_poll_exited = false;
    g_poll_revents = 0;

    struct Thread *consumer = thread_create(kproc(), consumer_poll_forever_entry);
    TEST_ASSERT(consumer != NULL, "thread_create");
    ready(consumer);
    TEST_YIELD_UNTIL(consumer->state == THREAD_SLEEPING);
    TEST_EXPECT_EQ(consumer->state, THREAD_SLEEPING,
        "consumer SLEEPING in poll");

    // Boot side: close the write end via the handle table — drops the
    // ring's ref, marks write_eof, walks poll list, wakes consumer.
    TEST_EXPECT_EQ(handle_close(p, hwr), 0, "close write end");
    TEST_EXPECT_NE(consumer->state, THREAD_SLEEPING,
        "consumer wakes after write-end close");

    TEST_YIELD_UNTIL(__atomic_load_n(&g_poll_result, __ATOMIC_ACQUIRE) != -999);
    TEST_EXPECT_EQ(g_poll_result, 1L, "consumer's poll returns 1");
    TEST_ASSERT((g_poll_revents & POLLHUP) != 0,
        "consumer's revents includes POLLHUP (POSIX hang-up)");

    test_kthread_join_free(consumer, &g_poll_exited);          // reap the parked poll helper (see block_then_wake)
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
    TEST_YIELD_UNTIL(sched_runnable_count() == 0u);   // #92-audit F4: settle console_mgr, verified

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
    g_poll_result  = -999; g_poll_exited = false;
    g_poll_revents = 0;

    struct Thread *consumer = thread_create(kproc(), consumer_poll_forever_entry);
    TEST_ASSERT(consumer != NULL, "thread_create");
    ready(consumer);

    // Consumer enters sys_poll_for_proc on the cons fd: cons_poll registers a
    // waiter on g_cons.poll_list; the empty ring is not POLLIN-ready; tsleep.
    TEST_YIELD_UNTIL(consumer->state == THREAD_SLEEPING);
    TEST_EXPECT_EQ(consumer->state, THREAD_SLEEPING,
        "consumer SLEEPING in poll on /dev/cons");

    // Producer (the IRQ side): a data byte arms poll_wake_pending + wakes
    // console_mgr -- but does NOT walk the hook list (deferred, not IRQ-safe).
    //
    // Both facts below are asserted as IMPLICATIONS with a deliberate read
    // order, because neither holds on its own under SMP: cons_rx_input wakes
    // console_mgr, and a peer CPU may dispatch it inside this window, where it
    // runs the relay for an entirely good reason -- clearing the flag AND waking
    // the poller. A bare "flag is armed" or "poller is still SLEEPING" read
    // therefore fails spuriously on a healthy kernel. cons_service_deferred is
    // the flag's ONLY consumer and always walks the hook list, so "cleared"
    // implies "relay ran" implies "poller woken", which is what makes the two
    // one-directional forms exact.
    cons_rx_input((u8)'k', false);

    // Armed: flag read FIRST. Still set -> armed. Cleared -> the relay already
    // ran, which itself proves it was armed, and the later state read sees the
    // wake. Only "never armed" leaves both false.
    // #92-audit F2: "cleared" does NOT imply "the wake landed". The relay
    // clears the flag under g_cons.lock, releases it, and only then calls
    // poll_waiter_list_wake (the walk is kept out of g_cons.lock's hold). A
    // concurrent console_mgr on a peer CPU
    // can therefore be observed post-clear, pre-wake -- both reads false, and
    // the implication misfires on a healthy kernel. If the flag reads cleared,
    // the wake is in flight, so waiting for it is sound and bounded.
    bool pending_first = cons_test_pollwake_pending();
    if (!pending_first)
        TEST_YIELD_UNTIL(consumer->state != THREAD_SLEEPING);
    TEST_ASSERT(pending_first || consumer->state != THREAD_SLEEPING,
        "data byte armed poll_wake_pending");

    // Deferred: state read FIRST. If the poller is awake, re-read the flag; a
    // flag still armed at the LATER read was armed at the earlier one too, so
    // the wake cannot have come from the relay -- it came from IRQ context,
    // which is the defect under test.
    bool woken_first = (consumer->state != THREAD_SLEEPING);
    TEST_ASSERT(!woken_first || !cons_test_pollwake_pending(),
        "the IRQ producer did NOT wake the real poller (deferred)");

    // The deferred relay (what console_mgr runs in process context), driven
    // synchronously so the wake is independent of scheduler order. THIS is the
    // #103 crux: the relay must reach the sys_poll_for_proc-blocked thread, not
    // merely flip a synthetic flag.
    // #92-audit F2: this call NO-OPS if a concurrent console_mgr already
    // drained the flags -- and that mgr may still be pre-wake (it acts with
    // g_cons.lock released). Waiting for the wake is sound either way: the
    // relay has run, synchronously here or on the peer, so the wake is in
    // flight and the deadline still fails loudly if the #103 defect returns.
    cons_test_service_deferred();
    TEST_YIELD_UNTIL(consumer->state != THREAD_SLEEPING);
    TEST_EXPECT_NE(consumer->state, THREAD_SLEEPING,
        "the deferred relay woke the real /dev/cons poller (the #103 assertion)");
    TEST_ASSERT(!cons_test_pollwake_pending(), "relay consumed poll_wake_pending");

    // Let the consumer resume + record. console_mgr is also RUNNABLE (the
    // cons_rx_input wake) but re-sleeps on the now-drained cond; a bounded yield
    // loop runs both regardless of order.
    TEST_YIELD_UNTIL(__atomic_load_n(&g_poll_result, __ATOMIC_ACQUIRE) != -999);
    TEST_EXPECT_EQ(g_poll_result, 1L, "consumer's poll returns 1");
    TEST_EXPECT_EQ((s64)g_poll_revents, (s64)POLLIN, "consumer's revents = POLLIN");

    test_kthread_join_free(consumer, &g_poll_exited);
    cons_test_reset();
    TEST_YIELD_UNTIL(sched_runnable_count() == 0u);   // #92-audit F4: re-settle, verified
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
    int h = devsrv_post_listener(p, root, name, name_len, SRV_MODE_BYTE, false);
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
static volatile bool g_listener_poll_exited;
static struct Proc *g_listener_proc;
static hidx_t       g_listener_fd;

static void listener_poll_forever_entry(void) {
    struct pollfd pfds[1] = {
        { .fd = g_listener_fd, .events = POLLIN, .revents = 0 },
    };
    s64 r = sys_poll_for_proc(g_listener_proc, pfds, 1, -1);
    g_listener_poll_revents = pfds[0].revents;
    __atomic_store_n(&g_listener_poll_result, r, __ATOMIC_RELEASE);
    test_kthread_park_terminal(&g_listener_poll_exited);
}

void test_poll_devsrv_listener_block_then_wake(void) {
    srv_registry_reset();

    struct Proc *corvus = make_marked_test_proc();
    TEST_ASSERT(corvus != NULL, "corvus proc");
    int svc_h = post_svc_byte(corvus, "corvus", 6);
    TEST_ASSERT(svc_h >= 0, "post \"corvus\"");

    g_listener_proc         = corvus;
    g_listener_fd           = (hidx_t)svc_h;
    g_listener_poll_result  = -999; g_listener_poll_exited = false;
    g_listener_poll_revents = 0;

    struct Thread *poller = thread_create(kproc(), listener_poll_forever_entry);
    TEST_ASSERT(poller != NULL, "thread_create");
    ready(poller);
    TEST_YIELD_UNTIL(poller->state == THREAD_SLEEPING);
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
    TEST_EXPECT_NE(poller->state, THREAD_SLEEPING,
        "listener-poll wakes after the connect");

    TEST_YIELD_UNTIL(__atomic_load_n(&g_listener_poll_result, __ATOMIC_ACQUIRE) != -999);
    TEST_EXPECT_EQ(g_listener_poll_result, 1L, "listener-poll returns 1");
    TEST_EXPECT_EQ((s64)g_listener_poll_revents, (s64)POLLIN,
        "wakeup revents = POLLIN");

    test_kthread_join_free(poller, &g_listener_poll_exited);            // reap the parked poll helper (see block_then_wake)
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
    g_listener_poll_result  = -999; g_listener_poll_exited = false;
    g_listener_poll_revents = 0;

    struct Thread *poller = thread_create(kproc(), listener_poll_forever_entry);
    TEST_ASSERT(poller != NULL, "thread_create");
    ready(poller);
    TEST_YIELD_UNTIL(poller->state == THREAD_SLEEPING);
    TEST_EXPECT_EQ(poller->state, THREAD_SLEEPING,
        "listener-poll is SLEEPING");

    // Boot side: reset the registry, which tombstones every service and
    // wakes every listener poll list (the regression that proves
    // tombstone-as-readiness-edge — corvus's listener-poll should not
    // hang past its service's death).
    srv_registry_reset();
    TEST_EXPECT_NE(poller->state, THREAD_SLEEPING,
        "listener-poll wakes on the tombstone");

    TEST_YIELD_UNTIL(__atomic_load_n(&g_listener_poll_result, __ATOMIC_ACQUIRE) != -999);
    TEST_EXPECT_EQ(g_listener_poll_result, 1L, "listener-poll returns 1");
    TEST_ASSERT((g_listener_poll_revents & POLLHUP) != 0,
        "tombstone revents includes POLLHUP");

    test_kthread_join_free(poller, &g_listener_poll_exited);            // reap the parked poll helper (see block_then_wake)
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
static volatile bool g_conn_poll_exited;
static struct Proc *g_conn_poll_proc;
static hidx_t       g_conn_poll_fd;

static void conn_poll_forever_entry(void) {
    struct pollfd pfds[1] = {
        { .fd = g_conn_poll_fd, .events = POLLIN, .revents = 0 },
    };
    s64 r = sys_poll_for_proc(g_conn_poll_proc, pfds, 1, -1);
    g_conn_poll_revents = pfds[0].revents;
    __atomic_store_n(&g_conn_poll_result, r, __ATOMIC_RELEASE);
    test_kthread_park_terminal(&g_conn_poll_exited);
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
    g_conn_poll_result  = -999; g_conn_poll_exited = false;
    g_conn_poll_revents = 0;

    struct Thread *poller = thread_create(kproc(), conn_poll_forever_entry);
    TEST_ASSERT(poller != NULL, "thread_create");
    ready(poller);
    TEST_YIELD_UNTIL(poller->state == THREAD_SLEEPING);
    TEST_EXPECT_EQ(poller->state, THREAD_SLEEPING,
        "connection-poll is SLEEPING — c2s empty");

    // Boot side: the kernel-client side queues bytes → POLLIN edge wakes
    // the poller.
    static const u8 frame[3] = { 0xDE, 0xAD, 0xBE };
    TEST_EXPECT_EQ(srvconn_client_send(cn, frame, 3), 3L,
        "client-side queues 3 bytes");
    TEST_EXPECT_NE(poller->state, THREAD_SLEEPING,
        "connection-poll wakes on the send");

    TEST_YIELD_UNTIL(__atomic_load_n(&g_conn_poll_result, __ATOMIC_ACQUIRE) != -999);
    TEST_EXPECT_EQ(g_conn_poll_result, 1L, "connection-poll returns 1");
    TEST_EXPECT_EQ((s64)g_conn_poll_revents, (s64)POLLIN,
        "wakeup revents = POLLIN");

    test_kthread_join_free(poller, &g_conn_poll_exited);            // reap the parked poll helper (see block_then_wake)
    srv_registry_reset();
    drop_test_proc(client);
    drop_test_proc(corvus);
}

// =============================================================================
// The CLIENT endpoint + the re-arm (2026-09-21; ARCH 23.3).
//
// Until this date devsrv_poll sent a CSRVCLIENT Spoor to the SERVER-endpoint
// sample: a client was told POLLIN while its OWN unread request sat in c2s, was
// never woken by its reply, and -- sys_poll_for_proc's half -- any wake whose
// re-sample found nothing returned 0. Each test below names the half it pins.
// =============================================================================

// A generalized poller: fd / events / timeout from globals.
static struct Proc  *g_cp_proc;
static hidx_t        g_cp_fd;
static s16           g_cp_events;
static s32           g_cp_timeout;
static volatile s64  g_cp_result;
static volatile bool g_cp_exited;
static volatile s16  g_cp_revents;

static void cp_poll_entry(void) {
    struct pollfd pfds[1] = {
        { .fd = g_cp_fd, .events = g_cp_events, .revents = 0 },
    };
    s64 r = sys_poll_for_proc(g_cp_proc, pfds, 1, g_cp_timeout);
    // revents BEFORE the result: the tests wait on g_cp_result and then read
    // g_cp_revents, possibly from another CPU.
    g_cp_revents = pfds[0].revents;
    __atomic_store_n(&g_cp_result, r, __ATOMIC_RELEASE);
    test_kthread_park_terminal(&g_cp_exited);
}

struct cp_fixture {
    struct Proc    *corvus, *client;
    struct SrvConn *cn;
    int             svc_h, client_h, conn_h;
};

static bool cp_setup(struct cp_fixture *f) {
    srv_registry_reset();
    f->corvus = make_marked_test_proc();
    if (!f->corvus) return false;
    f->svc_h = post_svc_byte(f->corvus, "corvus", 6);
    if (f->svc_h < 0) return false;
    f->client = make_test_proc();
    if (!f->client) return false;
    struct Spoor *cs = connect_byte(f->client, "corvus");
    if (!cs) return false;
    f->client_h = handle_alloc(f->client, KOBJ_SPOOR, RIGHT_READ | RIGHT_WRITE, cs);
    if (f->client_h < 0) return false;
    f->cn = devsrv_conn_of(cs);
    f->conn_h = sys_srv_accept_for_proc(f->corvus, (hidx_t)f->svc_h);
    return f->conn_h >= 0 && f->cn != NULL;
}

static void cp_teardown(struct cp_fixture *f) {
    srv_registry_reset();
    drop_test_proc(f->client);
    drop_test_proc(f->corvus);
}

static s16 cp_sample(struct Proc *p, int h, s16 events) {
    struct pollfd pfds[1] = { { .fd = (hidx_t)h, .events = events, .revents = 0 } };
    (void)sys_poll_for_proc(p, pfds, 1, 0);
    return pfds[0].revents;
}

// The client row, bit by bit, against the server row on the SAME connection.
void test_poll_devsrv_client_row(void) {
    struct cp_fixture f;
    TEST_ASSERT(cp_setup(&f), "fixture");
    const s16 io = POLLIN | POLLOUT;

    TEST_EXPECT_EQ((s64)cp_sample(f.client, f.client_h, io), (s64)POLLOUT,
        "fresh client: room to write, nothing to read");

    // THE defect: the client's own unread request is the SERVER's POLLIN.
    static const u8 req[7] = { 'r','e','q','u','e','s','t' };
    TEST_EXPECT_EQ(srvconn_client_send(f.cn, req, 7), 7L, "client queues a request");
    TEST_EXPECT_EQ((s64)cp_sample(f.client, f.client_h, io), (s64)POLLOUT,
        "a client is NOT readable because its own request is unread");
    TEST_EXPECT_EQ((s64)cp_sample(f.corvus, f.conn_h, io), (s64)(POLLIN | POLLOUT),
        "the server is");

    static const u8 rep[5] = { 'r','e','p','l','y' };
    TEST_EXPECT_EQ(srvconn_server_send(f.cn, rep, 5), 5L, "server queues a reply");
    TEST_EXPECT_EQ((s64)cp_sample(f.client, f.client_h, io), (s64)(POLLIN | POLLOUT),
        "the reply makes the client readable");

    // Teardown with the reply still buffered: bytes first, then the latches.
    srvconn_teardown(f.cn);
    TEST_EXPECT_EQ((s64)cp_sample(f.client, f.client_h, io),
        (s64)(POLLIN | POLLHUP | POLLERR), "buffered bytes + both EOFs");
    u8 in[8];
    TEST_EXPECT_EQ(srvconn_io_nonblock(f.cn, false, false, in, sizeof in), 5L, "drain the reply");
    TEST_EXPECT_EQ((s64)cp_sample(f.client, f.client_h, io), (s64)(POLLHUP | POLLERR),
        "pipe-like: no POLLIN at a drained EOF");

    cp_teardown(&f);
}

// A parked client poller: walks for the OTHER endpoint's edges must not end its
// poll (NoSpuriousZero), and the reply must (the lost wake).
void test_poll_devsrv_client_wakes_on_reply_only(void) {
    struct cp_fixture f;
    TEST_ASSERT(cp_setup(&f), "fixture");

    g_cp_proc = f.client;  g_cp_fd = (hidx_t)f.client_h;
    g_cp_events = POLLIN;  g_cp_timeout = 30000;
    g_cp_result = -999; g_cp_exited = false;    g_cp_revents = 0;

    struct Thread *poller = thread_create(kproc(), cp_poll_entry);
    TEST_ASSERT(poller != NULL, "thread_create");
    ready(poller);
    TEST_YIELD_UNTIL(poller->state == THREAD_SLEEPING);

    // c2s FILL: the server's POLLIN edge, noise for this poller. Pre-fix the
    // server-endpoint sample called this POLLIN and the poll returned 1 with
    // nothing to read; with the right sample but no re-arm it returned 0 --
    // "timed out", 30 s early.
    u64 resleeps = poll_total_resleeps();
    static const u8 req[3] = { 1, 2, 3 };
    TEST_EXPECT_EQ(srvconn_client_send(f.cn, req, 3), 3L, "request queued");
    TEST_YIELD_UNTIL(poll_total_resleeps() > resleeps && poller->state == THREAD_SLEEPING);
    TEST_EXPECT_EQ(g_cp_result, -999L, "the client's own request does not end its poll");

    // c2s DRAIN: the client's POLLOUT edge, which it did not ask about.
    resleeps = poll_total_resleeps();
    u8 in[8];
    TEST_EXPECT_EQ(srvconn_server_recv(f.cn, in, sizeof in), 3L, "server consumes it");
    TEST_YIELD_UNTIL(poll_total_resleeps() > resleeps && poller->state == THREAD_SLEEPING);
    TEST_EXPECT_EQ(g_cp_result, -999L, "nor does the server reading it");

    // s2c FILL: the edge this poller asked about. Pre-fix nothing walked the
    // list here and the poller slept until its timeout.
    static const u8 rep[4] = { 9, 8, 7, 6 };
    TEST_EXPECT_EQ(srvconn_server_send(f.cn, rep, 4), 4L, "reply queued");
    TEST_YIELD_UNTIL(__atomic_load_n(&g_cp_result, __ATOMIC_ACQUIRE) != -999);
    TEST_EXPECT_EQ(g_cp_result, 1L, "the reply ends it");
    TEST_EXPECT_EQ((s64)g_cp_revents, (s64)POLLIN, "revents = POLLIN");

    test_kthread_join_free(poller, &g_cp_exited);
    cp_teardown(&f);
}

// A server that polled POLLOUT on a full s2c is woken by a BLOCKING client
// drain (pre-fix only srvconn_io_nonblock's drain walked the list).
void test_poll_devsrv_server_pollout_wakes_on_client_drain(void) {
    struct cp_fixture f;
    TEST_ASSERT(cp_setup(&f), "fixture");

    static u8 chunk[1024];
    for (;;) {
        long put = srvconn_server_send(f.cn, chunk, sizeof chunk);
        if (put < (long)sizeof chunk) break;
    }
    TEST_EXPECT_EQ((s64)(cp_sample(f.corvus, f.conn_h, POLLOUT) & POLLOUT), 0L,
        "s2c is full: not writable");

    g_cp_proc = f.corvus;  g_cp_fd = (hidx_t)f.conn_h;
    g_cp_events = POLLOUT; g_cp_timeout = -1;
    g_cp_result = -999; g_cp_exited = false;    g_cp_revents = 0;

    struct Thread *poller = thread_create(kproc(), cp_poll_entry);
    TEST_ASSERT(poller != NULL, "thread_create");
    ready(poller);
    TEST_YIELD_UNTIL(poller->state == THREAD_SLEEPING);

    TEST_ASSERT(srvconn_client_recv(f.cn, chunk, sizeof chunk) > 0, "the client drains");
    TEST_YIELD_UNTIL(__atomic_load_n(&g_cp_result, __ATOMIC_ACQUIRE) != -999);
    TEST_EXPECT_EQ(g_cp_result, 1L, "the drain ends the server's poll");
    TEST_EXPECT_EQ((s64)g_cp_revents, (s64)POLLOUT, "revents = POLLOUT");

    test_kthread_join_free(poller, &g_cp_exited);
    cp_teardown(&f);
}

// The mirror: a client that polled POLLOUT on a full c2s is woken by the
// server's BLOCKING read -- the byte-mode POSIX-server read() path. That walk
// in srvconn_server_recv_blocking had no witness: deleting it passed every
// kernel test (B-0 audit round 4 F9). The assertion is WHEN the poll returns,
// not what it returns: without the walk the poll still reports POLLOUT -- its
// timeout pass re-samples and finds the room -- so the first form of this
// test, which asserted only the result, PASSED on the sabotaged kernel
// (measured). The timeout is bounded so that kernel still reaps cleanly.
#define CP_WAKE_LATE_NS (500ull * 1000ull * 1000ull)
void test_poll_devsrv_client_pollout_wakes_on_server_blocking_drain(void) {
    struct cp_fixture f;
    TEST_ASSERT(cp_setup(&f), "fixture");

    static u8 chunk[1024];
    for (;;) {
        long put = srvconn_client_send(f.cn, chunk, sizeof chunk);
        if (put < (long)sizeof chunk) break;
    }
    TEST_EXPECT_EQ((s64)(cp_sample(f.client, f.client_h, POLLOUT) & POLLOUT), 0L,
        "c2s is full: the client is not writable");

    g_cp_proc = f.client;  g_cp_fd = (hidx_t)f.client_h;
    g_cp_events = POLLOUT; g_cp_timeout = 3000;
    g_cp_result = -999; g_cp_exited = false;    g_cp_revents = 0;

    struct Thread *poller = thread_create(kproc(), cp_poll_entry);
    TEST_ASSERT(poller != NULL, "thread_create");
    ready(poller);
    TEST_YIELD_UNTIL(poller->state == THREAD_SLEEPING);

    const char *err = NULL;
    u64 drained = timer_now_ns();
    if (srvconn_server_recv_blocking(f.cn, chunk, sizeof chunk) <= 0)
        err = "the server drains through the blocking read";
    TEST_YIELD_UNTIL_SOFT(__atomic_load_n(&g_cp_result, __ATOMIC_ACQUIRE) != -999);
    u64 woke = timer_now_ns();
    if (!err && __atomic_load_n(&g_cp_result, __ATOMIC_ACQUIRE) == -999)
        err = "the drain woke the client's poll (it was still parked 2 s later)";
    else if (!err && woke - drained >= CP_WAKE_LATE_NS)
        err = "the poll ended at the drain, not at its timeout pass";
    else if (!err && g_cp_result != 1)
        err = "it returns 1";
    else if (!err && g_cp_revents != POLLOUT)
        err = "revents = POLLOUT";
    // A kernel without the walk returns at the 3 s timeout: wait it out so the
    // poller is reaped, never freed while still asleep.
    TEST_YIELD_UNTIL_SOFT(__atomic_load_n(&g_cp_result, __ATOMIC_ACQUIRE) != -999);
    if (__atomic_load_n(&g_cp_result, __ATOMIC_ACQUIRE) != -999) test_kthread_join_free(poller, &g_cp_exited);
    cp_teardown(&f);
    TEST_ASSERT(err == NULL, err ? err : "client POLLOUT on the blocking drain");
}

// The teardown edge, from the CLIENT's side: a client parked in poll(POLLIN) on
// its own connection is woken by srvconn_teardown's walk and sees the hang-up
// at once. Nothing witnessed that walk (B-0 audit round 5 F4): without it the
// poll still reports POLLHUP -- its timeout pass re-samples the latched eof --
// so the on-device prover that should have caught it passed ten seconds late.
// As with the drain above, the assertion is WHEN it returns.
void test_poll_devsrv_client_wakes_on_teardown(void) {
    struct cp_fixture f;
    TEST_ASSERT(cp_setup(&f), "fixture");
    TEST_EXPECT_EQ((s64)(cp_sample(f.client, f.client_h, POLLIN) & (POLLIN | POLLHUP)), 0L,
        "nothing to read and no hang-up yet");

    g_cp_proc = f.client;  g_cp_fd = (hidx_t)f.client_h;
    g_cp_events = POLLIN;  g_cp_timeout = 3000;
    g_cp_result = -999; g_cp_exited = false;    g_cp_revents = 0;

    struct Thread *poller = thread_create(kproc(), cp_poll_entry);
    TEST_ASSERT(poller != NULL, "thread_create");
    ready(poller);
    TEST_YIELD_UNTIL(poller->state == THREAD_SLEEPING);

    const char *err = NULL;
    u64 torn = timer_now_ns();
    srvconn_teardown(f.cn);
    TEST_YIELD_UNTIL_SOFT(__atomic_load_n(&g_cp_result, __ATOMIC_ACQUIRE) != -999);
    u64 woke = timer_now_ns();
    if (__atomic_load_n(&g_cp_result, __ATOMIC_ACQUIRE) == -999)
        err = "the teardown woke the client's poll (it was still parked)";
    else if (woke - torn >= CP_WAKE_LATE_NS)
        err = "the poll ended at the teardown, not at its timeout pass";
    else if (g_cp_result != 1)
        err = "it returns 1";
    else if ((g_cp_revents & POLLHUP) == 0)
        err = "revents carries POLLHUP (s2c.eof, the direction the client reads)";
    // A kernel without the walk returns at the 3 s timeout: wait it out so the
    // poller is reaped, never freed while still asleep.
    TEST_YIELD_UNTIL_SOFT(__atomic_load_n(&g_cp_result, __ATOMIC_ACQUIRE) != -999);
    if (__atomic_load_n(&g_cp_result, __ATOMIC_ACQUIRE) != -999)
        test_kthread_join_free(poller, &g_cp_exited);
    cp_teardown(&f);
    TEST_ASSERT(err == NULL, err ? err : "client POLLIN woken by the teardown");
}

// A kernel-attached conn's rings are the kernel 9P client's.
void test_poll_devsrv_client_kernel_attached_pollnval(void) {
    struct cp_fixture f;
    TEST_ASSERT(cp_setup(&f), "fixture");
    srvconn_set_kernel_attached(f.cn);
    TEST_EXPECT_EQ((s64)cp_sample(f.client, f.client_h, POLLIN | POLLOUT), (s64)POLLNVAL,
        "client endpoint of a kernel-attached conn");
    TEST_ASSERT((cp_sample(f.corvus, f.conn_h, POLLOUT) & POLLNVAL) == 0,
        "the server endpoint is unaffected");
    cp_teardown(&f);
}

// A producer that never stops walking the list must not hold a timed poller
// past its deadline: tsleep prefers a set flag to a passed deadline, so the
// re-arm loop carries its own test (specs/poll.tla PollTerminates).
//
// The window is between the loop's re-register and tsleep's cond check, and a
// producer on another thread lands in it only by luck -- measured: with the
// loop's deadline test removed, a send/recv producer still let this pass. So
// the producer here is the polled object itself: a Dev whose .poll walks its
// own hook list on EVERY sample, never ready. Each re-sample then re-flags the
// hook inside the window, deterministically, and tsleep never sleeps.
//
// The walking stops on its own BUSY_WALK_NS after the FIRST sample so a kernel
// WITHOUT the deadline test still returns (0, at the first quiet tsleep)
// instead of spinning a CPU for the rest of the suite. What separates the two
// is WHEN the last sample happened: at the 50 ms deadline, or at the end of the
// walking. Every interval is measured from the first sample, never from before
// thread_create: a poller the scheduler starts late must not read as one that
// returned late (B-0 audit round 4 F9).
//
// Every poll here that pins one of the loop's OWN checks -- this deadline test,
// and the die-check and stop park below -- runs with the noise backstop out of
// the way (NO_BACKSTOP): at its real budget the backstop ends a noise-driven
// poll within a couple of milliseconds by itself, so a kernel with the check
// removed would pass. The backstop has tests of its own after these.
#define BUSY_WALK_NS   (1000ull * 1000ull * 1000ull)
#define BUSY_LATE_NS   ( 500ull * 1000ull * 1000ull)
#define NO_BACKSTOP    (~0ull)
static struct poll_waiter_list g_busy_list = POLL_WAITER_LIST_INIT;
static u64 g_busy_first_sample_ns;     // 0 until the first sample
static u64 g_busy_until_ns;            // first sample + BUSY_WALK_NS
static u64 g_busy_last_sample_ns;
static volatile u64  g_busy_samples;
static volatile bool g_busy_ready;     // report POLLIN (the way out of a poll(-1))

static short busy_poll(struct Spoor *c, short events, struct poll_waiter *pw) {
    (void)c;
    if (pw) poll_waiter_list_register(&g_busy_list, pw);
    u64 now = timer_now_ns();
    if (g_busy_first_sample_ns == 0) {
        g_busy_first_sample_ns = now;
        g_busy_until_ns        = now + BUSY_WALK_NS;
    }
    g_busy_last_sample_ns = now;
    g_busy_samples++;
    if (g_busy_ready) return (short)(events & POLLIN);
    if (now < g_busy_until_ns) poll_waiter_list_wake(&g_busy_list);
    return 0;
}

static struct Dev g_busy_dev = {
    .dc   = (int)'~',
    .name = "pollbusy",
    .poll = busy_poll,
};

static void busy_reset(void) {
    g_busy_first_sample_ns = 0;
    g_busy_until_ns        = 0;
    g_busy_last_sample_ns  = 0;
    g_busy_samples         = 0;
    g_busy_ready           = false;
}

void test_poll_timeout_survives_a_busy_list(void) {
    struct Proc *p = make_test_proc();
    TEST_ASSERT(p != NULL, "test proc");
    hidx_t h = install_spoor(p, dev_simple_attach(&g_busy_dev, 0), RIGHT_READ);
    TEST_ASSERT(h >= 0, "busy fd installed");

    g_cp_proc = p;         g_cp_fd = h;
    g_cp_events = POLLIN;  g_cp_timeout = 50;
    g_cp_result = -999; g_cp_exited = false;    g_cp_revents = 0;
    busy_reset();
    u64 resleeps0 = poll_total_resleeps();
    u64 budget0   = poll_spin_budget_set_for_test(NO_BACKSTOP);

    struct Thread *poller = thread_create(kproc(), cp_poll_entry);
    TEST_ASSERT(poller != NULL, "thread_create");
    ready(poller);
    TEST_YIELD_UNTIL(__atomic_load_n(&g_cp_result, __ATOMIC_ACQUIRE) != -999);
    (void)poll_spin_budget_set_for_test(budget0);

    TEST_EXPECT_EQ(g_cp_result, 0L, "timed out");
    TEST_ASSERT(poll_total_resleeps() > resleeps0,
        "non-vacuous: the loop re-slept on a flagged-but-empty wake");
    TEST_ASSERT(g_busy_samples >= 3, "non-vacuous: the object was re-sampled");
    TEST_ASSERT(g_busy_last_sample_ns - g_busy_first_sample_ns < BUSY_LATE_NS,
        "returned at ITS deadline, not when the producer went quiet");

    test_kthread_join_free(poller, &g_cp_exited);
    drop_test_proc(p);
}

// A dying poller is not held by a producer's noise, and a stopped one parks
// (specs/poll.tla DeathTerminates / StopHonoured; B-0 audit round 4 F2).
// tsleep's own die-check and stop detour sit BEHIND its cond test, so a
// producer that re-flags the hook in every pass -- the busy Dev, again --
// keeps every tsleep returning AWOKEN before either check. The loop's own
// checks end the poll (death) or park it (stop) on the next pass; a kernel
// without them reaches tsleep's checks only when the walking stops, BUSY_WALK_NS
// after the first sample. The poller is a thread of a real (non-kproc) Proc,
// the only kind whose death and stop flags the checks read; the flags are set
// the way the #811 cascade and the stop delivery set them, without the
// machine-wide IPI (test_rendez_death_interrupts_sleep's reasoning). A noise
// pass yields (sched_yield_hint), which is what lets this thread run at all.
static struct Proc  *g_pn_proc;
static hidx_t        g_pn_fd;
static volatile s64  g_pn_result;
static volatile s16  g_pn_revents;
static volatile u64  g_pn_return_ns;
static volatile bool g_pn_exited;
static u64           g_pn_budget0;

static void pn_poll_entry(void) {
    struct pollfd pfds[1] = {
        { .fd = g_pn_fd, .events = POLLIN, .revents = 0 },
    };
    s64 r = sys_poll_for_proc(g_pn_proc, pfds, 1, -1);
    g_pn_return_ns = timer_now_ns();
    g_pn_revents   = pfds[0].revents;
    __atomic_store_n(&g_pn_result, r, __ATOMIC_RELEASE);
    test_kthread_park_terminal(&g_pn_exited);
}

// Start a poll(-1) on the busy Dev from a thread of a fresh Proc, with the
// noise backstop's budget set to `budget_ns` until pn_finish. NULL on any
// failure (nothing started).
static struct Thread *pn_start(u64 budget_ns) {
    g_pn_budget0 = poll_spin_budget_set_for_test(budget_ns);
    g_pn_proc = proc_alloc();
    if (!g_pn_proc) return NULL;
    g_pn_fd = install_spoor(g_pn_proc, dev_simple_attach(&g_busy_dev, 0), RIGHT_READ);
    if (g_pn_fd < 0) return NULL;
    busy_reset();
    g_pn_result = -999; g_pn_revents = 0; g_pn_return_ns = 0; g_pn_exited = false;
    struct Thread *poller = thread_create(g_pn_proc, pn_poll_entry);
    if (!poller) return NULL;
    ready(poller);
    return poller;
}

// Ends a poll the test could not end (a kernel without the fix): stop the
// noise, report POLLIN, walk the list; then reap. Safe on every path.
static void pn_finish(struct Thread *poller) {
    if (poller) {
        __atomic_store_n(&g_pn_proc->debug_stop_req, 0u, __ATOMIC_RELEASE);
        (void)wakeup(&poller->debug_rendez);
        g_busy_ready = true;
        poll_waiter_list_wake(&g_busy_list);
        TEST_YIELD_UNTIL_SOFT(__atomic_load_n(&g_pn_result, __ATOMIC_ACQUIRE) != -999);
        test_kthread_join_free(poller, &g_pn_exited);
    }
    if (g_pn_proc) {
        g_pn_proc->state = PROC_STATE_ZOMBIE;
        proc_free(g_pn_proc);
        g_pn_proc = NULL;
    }
    busy_reset();
    (void)poll_spin_budget_set_for_test(g_pn_budget0);
}

void test_poll_death_ends_a_noise_driven_poll(void) {
    struct Thread *poller = pn_start(NO_BACKSTOP);
    const char *err = poller ? NULL : "poller setup";
    if (!err) {
        TEST_YIELD_UNTIL_SOFT(g_busy_samples >= 3);
        if (g_busy_samples < 3) err = "non-vacuous: the poller is circling on the noise";
    }
    u64 died = 0;
    if (!err) {
        died = timer_now_ns();
        __atomic_store_n(&g_pn_proc->group_exit_msg, "killed", __ATOMIC_RELEASE);
        TEST_YIELD_UNTIL_SOFT(__atomic_load_n(&g_pn_result, __ATOMIC_ACQUIRE) != -999);
        if (__atomic_load_n(&g_pn_result, __ATOMIC_ACQUIRE) == -999)                        err = "the dying poll returned";
        else if (g_pn_result != 0)                      err = "a dying poll returns 0";
        else if (g_pn_return_ns - died >= BUSY_LATE_NS) err = "the loop's own die-check ended it, not the producer going quiet";
        else if (!poll_waiter_list_empty(&g_busy_list)) err = "no hook left behind";
    }
    pn_finish(poller);
    TEST_ASSERT(err == NULL, err ? err : "death");
}

void test_poll_stop_parks_a_noise_driven_poll(void) {
    struct Thread *poller = pn_start(NO_BACKSTOP);
    const char *err = poller ? NULL : "poller setup";
    if (!err) {
        TEST_YIELD_UNTIL_SOFT(g_busy_samples >= 3);
        if (g_busy_samples < 3) err = "non-vacuous: the poller is circling on the noise";
    }
    if (!err) {
        u64 stopped = timer_now_ns();
        __atomic_store_n(&g_pn_proc->debug_stop_req, 1u, __ATOMIC_RELEASE);
        TEST_YIELD_UNTIL_SOFT((poller->state == THREAD_SLEEPING &&
                               poller->rendez_blocked_on == &poller->debug_rendez) ||
                              timer_now_ns() - stopped >= BUSY_LATE_NS);
        if (!(poller->state == THREAD_SLEEPING &&
              poller->rendez_blocked_on == &poller->debug_rendez))
            err = "the stop parked the poller on its debug_rendez before the producer went quiet";
        else if (!poll_waiter_list_empty(&g_busy_list))
            err = "the loop parks with its hook OFF the list";
        else if (__atomic_load_n(&g_pn_result, __ATOMIC_ACQUIRE) != -999)
            err = "a stop parks the poll, it does not end it";
    }
    if (!err) {
        u64 frozen = g_busy_samples;
        for (int i = 0; i < 1000; i++) sched();
        if (g_busy_samples != frozen) err = "a parked poller samples nothing";
    }
    if (!err) {
        // Resume the way proc_debug_resume does (flag cleared BEFORE the wake),
        // with readiness waiting: the resumed pass re-registers and returns it.
        g_busy_ready = true;
        __atomic_store_n(&g_pn_proc->debug_stop_req, 0u, __ATOMIC_RELEASE);
        (void)wakeup(&poller->debug_rendez);
        TEST_YIELD_UNTIL_SOFT(__atomic_load_n(&g_pn_result, __ATOMIC_ACQUIRE) != -999);
        if (g_pn_result != 1)               err = "the resumed poll returns the readiness";
        else if (g_pn_revents != POLLIN)    err = "revents = POLLIN";
    }
    pn_finish(poller);
    TEST_ASSERT(err == NULL, err ? err : "stop");
}

// The noise backstop (specs/poll.tla SpinBounded / BackoffHoldsNoHook; B-0
// audit round 5 F1). A syscall runs IRQ-masked end to end, so a poll(-1) the
// busy Dev keeps awake held its CPU with interrupts masked for the producer's
// whole walking window -- the SAK included -- and any unprivileged program can
// be that producer. At the real budget the poller must keep really SLEEPING
// while the noise goes on, again and again, with its hook OFF the list each
// time; and readiness must still end the poll.
//
// Under this producer a flag-sensitive tsleep never sleeps (the Dev re-flags
// the hook inside every sample), so a poller seen SLEEPING while the producer
// walks is in the backoff. pn_backoff_look takes one consistent look at it:
// SLEEPING at both ends, no switch-out (nsleeps) and no new backoff begun
// (the counter is bumped before a backoff's tsleep) in between -- so the list
// read in the middle falls inside ONE backoff, never across a re-register.
// Returns 1 asleep and hookless, 0 asleep with its hook LISTED (the failure),
// -1 no consistent look.
static int pn_backoff_look(struct Thread *poller) {
    u64 b1 = poll_total_backoffs();
    u64 n1 = __atomic_load_n(&poller->nsleeps, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (poller->state != THREAD_SLEEPING) return -1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    bool hookless = poll_waiter_list_empty(&g_busy_list);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (poller->state != THREAD_SLEEPING) return -1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&poller->nsleeps, __ATOMIC_RELAXED) != n1) return -1;
    if (poll_total_backoffs() != b1) return -1;
    if (timer_now_ns() >= g_busy_until_ns) return -1;   // the noise is over
    return hookless ? 1 : 0;
}

#define BACKSTOP_WATCH_NS (100ull * 1000ull * 1000ull)

void test_poll_backstop_sleeps_through_noise(void) {
    u64 b0 = poll_total_backoffs();
    struct Thread *poller = pn_start(POLL_SPIN_BUDGET_NS);
    const char *err = poller ? NULL : "poller setup";
    if (!err) {
        TEST_YIELD_UNTIL_SOFT(g_busy_samples >= 3);
        if (g_busy_samples < 3) err = "non-vacuous: the poller is circling on the noise";
    }
    if (!err) {
        u64 s0 = __atomic_load_n(&poller->nsleeps, __ATOMIC_RELAXED);
        u64 t0 = timer_now_ns();
        int looks = 0, hooked = 0;
        while (timer_now_ns() - t0 < BACKSTOP_WATCH_NS) {
            int v = pn_backoff_look(poller);
            if (v == 1) looks++;
            if (v == 0) hooked++;
            sched();
        }
        u64 slept = __atomic_load_n(&poller->nsleeps, __ATOMIC_RELAXED) - s0;
        if (hooked)
            err = "a backed-off poller holds NO hook (BackoffHoldsNoHook)";
        else if (looks == 0)
            err = "non-vacuous: the poller was seen asleep while the producer walked";
        else if (poll_total_backoffs() - b0 < 3)
            err = "the backstop fires again and again while the noise lasts";
        else if (slept < 3)
            err = "each backoff really slept: the poller's nsleeps moved";
        else if (__atomic_load_n(&g_pn_result, __ATOMIC_ACQUIRE) != -999)
            err = "the backoff never ends the poll: poll(-1) returns only on readiness";
    }
    if (!err) {
        // No wake: a poller mid-backoff is on no list to be walked. The
        // re-register after its sleep samples the readiness.
        g_busy_ready = true;
        TEST_YIELD_UNTIL_SOFT(__atomic_load_n(&g_pn_result, __ATOMIC_ACQUIRE) != -999);
        if (g_pn_result != 1)            err = "readiness still ends a backed-off poll";
        else if (g_pn_revents != POLLIN) err = "revents = POLLIN";
    }
    pn_finish(poller);
    TEST_ASSERT(err == NULL, err ? err : "backstop");
}

// The backoff's sleep is capped at the poll's own deadline, and its TIMEDOUT
// there is the poll's: a timed poll under the same noise backs off and returns
// 0 AT its deadline -- not at the first backoff's end (a spurious 0,
// NoSpuriousZero) and not when the producer goes quiet. Timed from the first
// sample, as test_poll_timeout_survives_a_busy_list is.
#define BACKSTOP_TIMEOUT_MS 30
#define BACKSTOP_EARLY_NS   (25ull * 1000ull * 1000ull)

void test_poll_backstop_keeps_the_deadline(void) {
    struct Proc *p = make_test_proc();
    TEST_ASSERT(p != NULL, "test proc");
    hidx_t h = install_spoor(p, dev_simple_attach(&g_busy_dev, 0), RIGHT_READ);
    TEST_ASSERT(h >= 0, "busy fd installed");

    g_cp_proc = p;         g_cp_fd = h;
    g_cp_events = POLLIN;  g_cp_timeout = BACKSTOP_TIMEOUT_MS;
    g_cp_result = -999; g_cp_exited = false;    g_cp_revents = 0;
    busy_reset();
    u64 budget0 = poll_spin_budget_set_for_test(POLL_SPIN_BUDGET_NS);
    u64 b0      = poll_total_backoffs();

    struct Thread *poller = thread_create(kproc(), cp_poll_entry);
    TEST_ASSERT(poller != NULL, "thread_create");
    ready(poller);
    TEST_YIELD_UNTIL(__atomic_load_n(&g_cp_result, __ATOMIC_ACQUIRE) != -999);
    (void)poll_spin_budget_set_for_test(budget0);

    TEST_EXPECT_EQ(g_cp_result, 0L, "timed out");
    TEST_ASSERT(poll_total_backoffs() > b0,
        "non-vacuous: the noise drove the poller into the backstop");
    TEST_ASSERT(g_busy_last_sample_ns - g_busy_first_sample_ns >= BACKSTOP_EARLY_NS,
        "returned AT its deadline, not at a backoff's end");
    TEST_ASSERT(g_busy_last_sample_ns - g_busy_first_sample_ns < BUSY_LATE_NS,
        "returned at its deadline, not when the producer went quiet");

    test_kthread_join_free(poller, &g_cp_exited);
    drop_test_proc(p);
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

// V-5c-2: the zero-fd sleep. `select(0, NULL, NULL, NULL, &tv)` is the classic
// portable sleep and `poll(NULL, 0, ms)` is its twin; both route here, because
// sys_poll_for_proc rejects nfds == 0 and that rejection is a native ABI worth
// leaving alone.
//
// THIS TEST MEASURES THE CLOCK ON PURPOSE. The in-guest leg (viv-pheno-probe
// L97) can only assert that the call returns 0, which a kernel that never
// waited at all would also satisfy -- the phenotype has no clock_gettime row,
// so the guest cannot tell the difference. Here timer_now_ns() is reachable, so
// this is the half that proves the sleep is a sleep.
void test_poll_sleep_for_waits(void);
void test_poll_sleep_for_waits(void) {
    // A zero timeout is a no-op, not a trip through the scheduler.
    u64 t0 = timer_now_ns();
    TEST_EXPECT_EQ(sys_poll_sleep_for(0), (s64)0, "a zero sleep returns 0");
    u64 zero_elapsed = timer_now_ns() - t0;
    TEST_ASSERT(zero_elapsed < 5000000ull,
                "and returns promptly -- it must not park for a zero timeout");

    // A real timeout must actually consume wall time. The floor is deliberately
    // under the request (the deadline is a floor, and the tick granularity plus
    // scheduling can land slightly either side of an exact 40 ms), but it is far
    // enough above zero that a no-op implementation cannot pass.
    t0 = timer_now_ns();
    TEST_EXPECT_EQ(sys_poll_sleep_for(40), (s64)0, "a 40ms sleep returns 0");
    u64 elapsed = timer_now_ns() - t0;
    TEST_ASSERT(elapsed >= 20000000ull,
                "and actually waited -- a no-op return would land near zero");

    // Nothing signals the private Rendez, so the wait ends on its deadline
    // rather than on a wake. A sleep that returned early every time would fail
    // the floor above; one that never returned would hang the suite here.
    TEST_EXPECT_EQ(sys_poll_sleep_for(10), (s64)0, "a short sleep also returns 0");
}
