// P5-stratumd-stub-bringup-b — kernel-internal rejection-path tests
// for SYS_SPAWN_WITH_FDS.
//
// The happy path is exercised end-to-end at EL0 by
// `userspace.stub_driver_round_trip` (kernel/test/test_stub_driver.c)
// which runs the full production-shape orchestration. These tests
// cover the rejection branches that the happy path doesn't surface.
//
// Coverage:
//
//   sys_spawn_with_fds.rejects_oversize_fd_count
//     fd_count > SYS_SPAWN_MAX_FDS → -1.
//
//   sys_spawn_with_fds.rejects_bad_fd
//     fd_list contains a non-existent fd → -1.
//
//   sys_spawn_with_fds.rejects_non_spoor_fd
//     fd_list contains a non-KOBJ_SPOOR handle → -1.
//     (Burrow handle in this test; ARCH I-5 already prohibits MMIO /
//     IRQ / DMA from cross-Proc transfer at the syscall layer.)
//
//   sys_spawn_with_fds.rejects_missing_binary
//     name = "nonexistent" → -1.
//
//   sys_spawn_with_fds.zero_count_succeeds
//     fd_count = 0 is the same as SYS_SPAWN (no inheritance).
//     Happy path with "hello".

#include "test.h"

#include <thylacine/burrow.h>
#include <thylacine/dev.h>
#include <thylacine/handle.h>
#include <thylacine/pipe.h>
#include <thylacine/poll.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

extern int sys_spawn_with_fds_for_proc(struct Proc *p, const char *name,
                                        size_t name_len,
                                        const u32 *fds, u32 fd_count);

void test_sys_spawn_with_fds_rejects_oversize_fd_count(void);
void test_sys_spawn_with_fds_rejects_bad_fd(void);
void test_sys_spawn_with_fds_rejects_non_spoor_fd(void);
void test_sys_spawn_with_fds_rejects_missing_binary(void);
void test_sys_spawn_with_fds_zero_count_succeeds(void);
void test_sys_spawn_with_fds_child_rights_subset_of_parent(void);
void test_sys_spawn_killed_child_delivers_pipe_eof_before_reap(void);
void test_sys_spawn_joined_multithread_child_delivers_pipe_eof_before_reap(void);

static void drain_zombies(void) {
    int status = 0;
    while (wait_pid(&status) > 0) { /* drain */ }
}

void test_sys_spawn_with_fds_rejects_oversize_fd_count(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    u32 fds[SYS_SPAWN_MAX_FDS + 1] = { 0 };
    int rc = sys_spawn_with_fds_for_proc(t->proc, "hello", 5,
                                         fds, SYS_SPAWN_MAX_FDS + 1u);
    TEST_EXPECT_EQ(rc, -1, "fd_count > SYS_SPAWN_MAX_FDS → -1");
}

void test_sys_spawn_with_fds_rejects_bad_fd(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    // PROC_HANDLE_MAX is the upper bound; an fd of UINT32_MAX is
    // clearly out of range. handle_get returns NULL → handler returns -1.
    u32 fds[1] = { 0xFFFFFFFFu };
    int rc = sys_spawn_with_fds_for_proc(t->proc, "hello", 5, fds, 1);
    TEST_EXPECT_EQ(rc, -1, "fd outside handle-table range → -1");
}

void test_sys_spawn_with_fds_rejects_non_spoor_fd(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    // Allocate an anonymous Burrow handle in kproc; verify spawn
    // rejects it because kind != KOBJ_SPOOR. Use a small page-aligned
    // size so the Burrow allocation is cheap.
    struct Burrow *b = burrow_create_anon(4096);
    TEST_ASSERT(b != NULL, "burrow_create_anon");

    hidx_t bh = handle_alloc(t->proc, KOBJ_BURROW,
                             RIGHT_READ | RIGHT_WRITE | RIGHT_MAP, b);
    TEST_ASSERT(bh >= 0, "handle_alloc for burrow");

    u32 fds[1] = { (u32)bh };
    int rc = sys_spawn_with_fds_for_proc(t->proc, "hello", 5, fds, 1);
    TEST_EXPECT_EQ(rc, -1, "non-KOBJ_SPOOR fd → -1");

    // Clean up the burrow handle; the test Proc is kproc and will
    // accumulate state otherwise.
    handle_close(t->proc, bh);
}

void test_sys_spawn_with_fds_rejects_missing_binary(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    int rc = sys_spawn_with_fds_for_proc(t->proc, "nonexistent", 11,
                                         NULL, 0);
    TEST_EXPECT_EQ(rc, -1, "missing binary → -1");
}

void test_sys_spawn_with_fds_zero_count_succeeds(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    // fd_count=0 should behave like SYS_SPAWN (no inheritance).
    int pid = sys_spawn_with_fds_for_proc(t->proc, "hello", 5, NULL, 0);
    TEST_ASSERT(pid > 0, "fd_count=0 + valid binary → positive pid");

    int status = -1;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid reaps the spawned child");
    TEST_EXPECT_EQ(status, 0, "/hello exits status=0 even without inherited fds");
}

// R15 F231 close: regression for the rights-elevation bug.
// Pre-fix, the child's handle_alloc hardcoded RIGHT_READ|WRITE|TRANSFER
// regardless of the parent's slot rights. This test creates a Spoor
// with RIGHT_READ only in the parent, spawns a child with that fd in
// the inheritance list, and inspects the child's slot rights via
// proc_find_by_pid + polling for the thunk's handle_alloc to complete.
//
// The child is pipe-sink (NOT /hello): pipe-sink BLOCKS reading fd 0
// until EOF, so its handle table -- and the inherited fd 0 -- stays live
// for inspection. This matters since #926: a Proc's fds now close at
// EXIT (the zombie transition), so a fast-exiting child like /hello would
// close fd 0 before this poll could observe it. A child blocked on its
// inherited fd 0 keeps it live deterministically. We hold the write end
// (no data), so pipe-sink blocks; once the thunk's handle_alloc installs
// fd 0 the slot stays put. Inspect rights, then feed the payload + close
// the write end so pipe-sink reads EOF and exits 0 -- which additionally
// proves the inherited RIGHT_READ fd 0 is actually readable by the child.
void test_sys_spawn_with_fds_child_rights_subset_of_parent(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    // Create a pipe pair; install the read end in kproc's handle
    // table with RIGHT_READ only (the explicit narrowing).
    struct Spoor *rd = NULL, *wr = NULL;
    TEST_EXPECT_EQ(pipe_create(&rd, &wr), 0, "pipe_create");

    const rights_t parent_rights = RIGHT_READ;
    hidx_t parent_fd = handle_alloc(t->proc, KOBJ_SPOOR, parent_rights, rd);
    TEST_ASSERT(parent_fd >= 0, "handle_alloc with RIGHT_READ only");

    u32 fds[1] = { (u32)parent_fd };
    int pid = sys_spawn_with_fds_for_proc(t->proc, "pipe-sink", 9, fds, 1);
    TEST_ASSERT(pid > 0, "spawn with 1 inherited fd returns pid");

    struct Proc *child = proc_find_by_pid(pid);
    TEST_ASSERT(child != NULL, "proc_find_by_pid for spawned child");

    // Poll for the child's slot to appear (thunk's handle_alloc runs
    // async on the child's CPU). Bounded to 1024 sched yields. pipe-sink
    // then blocks reading fd 0 (we hold wr, no data yet), so once the slot
    // is installed it stays live and the child cannot exit-and-close it
    // underneath us -- the poll catches it deterministically.
    struct Handle child_slot;
    bool got_child = false;
    for (int i = 0; i < 1024; i++) {
        if (handle_get(child, 0, &child_slot) == 0) { got_child = true; break; }
        sched();
    }
    TEST_ASSERT(got_child,
        "child has handle at fd 0 after bounded poll");
    TEST_EXPECT_EQ((u64)child_slot.kind, (u64)KOBJ_SPOOR,
        "child fd 0 is KOBJ_SPOOR");

    // The critical invariant: child's rights must equal parent's rights
    // (or be a strict subset). Pre-fix, this would be
    // RIGHT_READ|WRITE|TRANSFER (hardcoded), which is a SUPERSET of the
    // parent's RIGHT_READ — an I-6 violation.
    TEST_EXPECT_EQ((u64)child_slot.rights, (u64)parent_rights,
        "child slot rights == parent slot rights (R15 F231)");
    handle_put(&child_slot);

    // Feed pipe-sink the exact payload it expects, then close the write
    // end so its next read returns EOF -- it then exits 0. This unblocks
    // the child + proves the inherited fd 0 is readable with RIGHT_READ.
    const char payload[] = "PIPE-DATA-OK\n";
    wr->dev->write(wr, payload, (long)(sizeof(payload) - 1), 0);
    spoor_clunk(wr); // deliver EOF; releases the write end

    int status = -1;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid reaps the spawned child");
    TEST_EXPECT_EQ(status, 0, "pipe-sink exits status=0 (read inherited fd 0)");

    // Clean up parent-side handle (wr already clunked above).
    handle_close(t->proc, parent_fd);
}

// #68: proc_for_each callback -- kill the Proc whose pid matches *arg.
// Runs under g_proc_table_lock (proc_group_terminate's contract).
static int kill_pid_cb(struct Proc *p, void *arg) {
    int want = *(int *)arg;
    if (p->pid != want) return 0;
    proc_group_terminate(p, "kill");
    return 1;
}

// #68 regression (completes the #926 seam): a KILLED Proc's inherited pipe
// write end must deliver EOF at process TERMINATION -- from the last dying
// Thread's pre-ZOMBIE handle close in thread_exit_self -- NOT at reap.
//
// Pre-#68, a Proc dying via proc_group_terminate -> el0_return_die_check ->
// thread_exit_self deferred its handle close to proc_free (reap), so a
// parent draining the child's output pipe to EOF BEFORE reaping deadlocked
// (the nora gofmt-on-save hang / the ut `$(go ...)` substitution wedge --
// every Go binary exits multi-thread via SYS_EXIT_GROUP and takes this
// path; the killed-single-thread child here exercises the SAME
// thread_exit_self last-out close, deterministically).
//
// Shape: pipe A parks pipe-sink reading fd 0 (we hold A's write end, no
// data); pipe B's write end is the child's fd 1 (we close our copy after
// spawn, so the child's is the LAST write end). Kill the child, then --
// WITHOUT reaping -- poll B's read end for EOF within a bounded yield
// budget. Pre-fix the budget exhausts (EOF only at reap); post-fix the
// dying Thread's close delivers it. Bounded on failure, never a hang.
void test_sys_spawn_killed_child_delivers_pipe_eof_before_reap(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    struct Spoor *rd_a = NULL, *wr_a = NULL;   // A: child stdin (parks it)
    struct Spoor *rd_b = NULL, *wr_b = NULL;   // B: child fd 1 (the probe)
    TEST_EXPECT_EQ(pipe_create(&rd_a, &wr_a), 0, "pipe_create A");
    TEST_EXPECT_EQ(pipe_create(&rd_b, &wr_b), 0, "pipe_create B");

    hidx_t fd_a = handle_alloc(t->proc, KOBJ_SPOOR, RIGHT_READ, rd_a);
    hidx_t fd_b = handle_alloc(t->proc, KOBJ_SPOOR, RIGHT_WRITE, wr_b);
    TEST_ASSERT(fd_a >= 0 && fd_b >= 0, "handle_alloc for both pipe ends");

    u32 fds[2] = { (u32)fd_a, (u32)fd_b };
    int pid = sys_spawn_with_fds_for_proc(t->proc, "pipe-sink", 9, fds, 2);
    TEST_ASSERT(pid > 0, "spawn pipe-sink with fd0=A.rd fd1=B.wr");

    // Drop OUR copy of B's write end: the child's inherited fd 1 is now the
    // sole write end, so B's EOF tracks the CHILD's close exactly.
    handle_close(t->proc, fd_b);

    // Confirm the inherited fds are installed (they are dup'd into the
    // child's table during the parent's spawn call, so this succeeds at
    // once -- round-3 audit F2: it does NOT wait for the child to park in
    // its fd 0 read, and doesn't need to: the kill is sound at ANY point in
    // the child's run. Killed pre-first-syscall, the read's
    // register-then-observe sees die-pending and unwinds; killed while
    // parked, the #811 wake unwinds -- both routes reach thread_exit_self's
    // last-out close, which is what this test pins.)
    struct Proc *child = proc_find_by_pid(pid);
    TEST_ASSERT(child != NULL, "proc_find_by_pid for spawned child");
    struct Handle slot;
    bool installed = false;
    for (int i = 0; i < 4096; i++) {
        if (handle_get(child, 0, &slot) == 0) {
            handle_put(&slot);
            installed = true;
            break;
        }
        sched();
    }
    TEST_ASSERT(installed, "child's inherited fds installed (bounded poll)");

    // Kill it (I-26 mechanism; under g_proc_table_lock via proc_for_each).
    int want = pid;
    TEST_EXPECT_EQ(proc_for_each(kill_pid_cb, &want), 1,
        "kill callback found + terminated the child");

    // THE invariant: B becomes readable (EOF) BEFORE any reap. Sample-only
    // poll (pw == NULL); POLLHUP is output-only so it reports regardless of
    // the requested mask once the last writer is gone.
    bool eof_ready = false;
    for (int i = 0; i < 65536; i++) {
        short rev = rd_b->dev->poll(rd_b, POLLIN, NULL);
        if (rev & (POLLIN | POLLHUP)) { eof_ready = true; break; }
        sched();
    }
    TEST_ASSERT(eof_ready,
        "killed child's pipe write end delivered EOF BEFORE reap (#68)");

    char buf[8];
    long n = rd_b->dev->read(rd_b, buf, sizeof(buf), 0);
    TEST_EXPECT_EQ((int)n, 0, "read on B returns 0 (EOF), not data");

    // Only now reap. The killed child's status collapses to 1.
    int status = -1;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid reaps the killed child");
    TEST_EXPECT_EQ(status, 1, "killed child reports status 1");

    // Cleanup: our held ends + the fd_a handle (child-side copies died with
    // the child).
    spoor_clunk(wr_a);
    spoor_clunk(rd_b);
    handle_close(t->proc, fd_a);
}

// #68 round-2 F2 regression: a JOINED-then-exits native MULTI-thread Proc's
// inherited pipe write end must deliver EOF at process termination -- from
// exits()'s live_peers-gated close window -- NOT at reap.
//
// /thread-probe spawns worker Threads and joins each via the
// clear-child-tid torpor handshake; a worker's SYS_THREAD_EXIT leaves it
// THREAD_EXITING but UNREAPED (thread_count decrements only at reap), so
// main's t_exits() arrives with thread_count > 1 and live_peers == 0 --
// the exact shape the retired thread_count==1 gate skipped (round-2 F2):
// pre-fix the close deferred to reap, so the drain-before-reap parent
// below never saw EOF until it reaped. Post-fix exits()'s window closes
// the table pre-ZOMBIE. (The killed_child test above exercises the
// thread_exit_self site; this one pins the exits() site.)
//
// Shape: pipe B's write end is the child's fd 1 (we close our copy after
// spawn, so the child's is the LAST write end); child fd 0 gets pipe A's
// read end (unused by the probe -- a placeholder for the contiguous fd
// map). DRAIN B to EOF within a bounded yield budget BEFORE reaping --
// the literal drain-before-reap parent (draining as data arrives also
// means the probe can never park on a full pipe). Bounded on failure,
// never a hang: pre-fix the child still zombies fine, only the EOF
// ordering is violated, so the budget exhausts, the assert fails, and
// the reap below still succeeds.
void test_sys_spawn_joined_multithread_child_delivers_pipe_eof_before_reap(void) {
    drain_zombies();
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    struct Spoor *rd_a = NULL, *wr_a = NULL;   // A: child fd 0 placeholder
    struct Spoor *rd_b = NULL, *wr_b = NULL;   // B: child fd 1 (the probe)
    TEST_EXPECT_EQ(pipe_create(&rd_a, &wr_a), 0, "pipe_create A");
    TEST_EXPECT_EQ(pipe_create(&rd_b, &wr_b), 0, "pipe_create B");

    hidx_t fd_a = handle_alloc(t->proc, KOBJ_SPOOR, RIGHT_READ, rd_a);
    hidx_t fd_b = handle_alloc(t->proc, KOBJ_SPOOR, RIGHT_WRITE, wr_b);
    TEST_ASSERT(fd_a >= 0 && fd_b >= 0, "handle_alloc for both pipe ends");

    u32 fds[2] = { (u32)fd_a, (u32)fd_b };
    int pid = sys_spawn_with_fds_for_proc(t->proc, "thread-probe", 12, fds, 2);
    TEST_ASSERT(pid > 0, "spawn thread-probe with fd1=B.wr");

    // Drop OUR copy of B's write end: the child's inherited fd 1 is the
    // sole write end, so EOF tracks the CHILD's close exactly.
    handle_close(t->proc, fd_b);

    // The literal #926/#68 parent: drain B to EOF BEFORE any reap. The
    // probe's whole run (worker spawns + torpor joins + the fd-1 "ok"
    // line) rides inside the bounded budget; EOF == read 0 after the
    // child's exits()-window close drops the last write end.
    bool eof_before_reap = false;
    char buf[64];
    for (int i = 0; i < (1 << 18); i++) {
        short rev = rd_b->dev->poll(rd_b, POLLIN, NULL);
        if (rev & (POLLIN | POLLHUP)) {
            long n = rd_b->dev->read(rd_b, buf, sizeof(buf), 0);
            if (n == 0) { eof_before_reap = true; break; }
            TEST_ASSERT(n > 0, "pipe read never errors mid-drain");
        } else {
            sched();
        }
    }
    TEST_ASSERT(eof_before_reap,
        "joined-multithread child's pipe delivered EOF BEFORE reap (#68 F2)");

    // Only now reap.
    int status = -1;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid reaps thread-probe");
    TEST_EXPECT_EQ(status, 0, "thread-probe exits status 0");

    spoor_clunk(wr_a);
    spoor_clunk(rd_b);
    handle_close(t->proc, fd_a);
}
