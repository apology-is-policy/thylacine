// P5-spawn-wait — kernel-internal tests for SYS_SPAWN + SYS_WAIT_PID.
//
// The userspace happy path is exercised by the boot CPU's /joey
// (`usr/joey/joey.c`): /joey calls t_spawn("hello", ...), t_wait_pid,
// and verifies the child exits 0. The tests here cover rejection paths
// + concurrent reaper sanity that the live boot can't surface.
//
// Coverage:
//
//   sys_spawn.happy_path
//     sys_spawn_for_proc("hello", 5) returns a positive pid; wait_pid
//     reaps the child with status=0.
//
//   sys_spawn.rejects_null_name
//     NULL name → -1.
//
//   sys_spawn.rejects_zero_len
//     name_len=0 → -1.
//
//   sys_spawn.rejects_oversize_name
//     name_len > SYS_SPAWN_NAME_MAX → -1.
//
//   sys_spawn.rejects_missing_binary
//     "nonexistent" → -1.
//
//   sys_spawn.rejects_embedded_nul
//     name buffer contains an embedded NUL inside name_len → -1.
//
//   sys_wait_pid.no_children_returns_neg1
//     wait_pid on the current thread when no children are outstanding
//     returns -1 immediately.

#include "test.h"

#include <thylacine/proc.h>
#include <thylacine/syscall.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

extern int sys_spawn_for_proc(struct Proc *p, const char *name, size_t name_len);

void test_sys_spawn_happy_path(void);
void test_sys_spawn_rejects_null_name(void);
void test_sys_spawn_rejects_zero_len(void);
void test_sys_spawn_rejects_oversize_name(void);
void test_sys_spawn_rejects_missing_binary(void);
void test_sys_spawn_rejects_embedded_nul(void);
void test_sys_wait_pid_no_children_returns_neg1(void);

// Reap any leftover zombies from prior tests so wait_pid coverage stays
// deterministic. wait_pid returns -1 when no children remain; loop until
// drained.
static void drain_zombies(void) {
    int status = 0;
    while (wait_pid(&status) > 0) { /* drain */ }
}

void test_sys_spawn_happy_path(void) {
    drain_zombies();

    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");

    int pid = sys_spawn_for_proc(t->proc, "hello", 5);
    TEST_ASSERT(pid > 0, "sys_spawn(\"hello\") returns positive pid");

    int status = -1;
    int reaped = wait_pid(&status);
    TEST_EXPECT_EQ(reaped, pid, "wait_pid reaps the spawned child");
    TEST_EXPECT_EQ(status, 0, "/hello exits status=0");
}

void test_sys_spawn_rejects_null_name(void) {
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");
    TEST_EXPECT_EQ(sys_spawn_for_proc(t->proc, NULL, 5), -1,
        "NULL name → -1");
}

void test_sys_spawn_rejects_zero_len(void) {
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");
    TEST_EXPECT_EQ(sys_spawn_for_proc(t->proc, "hello", 0), -1,
        "name_len=0 → -1");
}

void test_sys_spawn_rejects_oversize_name(void) {
    char name[SYS_SPAWN_NAME_MAX + 2];
    for (size_t i = 0; i < sizeof(name); i++) name[i] = 'a';
    name[sizeof(name) - 1] = '\0';

    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");
    TEST_EXPECT_EQ(sys_spawn_for_proc(t->proc, name, SYS_SPAWN_NAME_MAX + 1u),
        -1, "name_len > SYS_SPAWN_NAME_MAX → -1");
}

void test_sys_spawn_rejects_missing_binary(void) {
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");
    TEST_EXPECT_EQ(sys_spawn_for_proc(t->proc, "nonexistent", 11), -1,
        "devramfs_lookup miss → -1");
}

void test_sys_spawn_rejects_embedded_nul(void) {
    // name = "abc" but tell the function name_len=5; bytes 3 and 4 are
    // NUL — embedded NUL inside the in-band length is rejected.
    char name[] = { 'a', 'b', 'c', '\0', '\0', '\0' };
    struct Thread *t = current_thread();
    TEST_ASSERT(t && t->proc, "current thread has Proc");
    TEST_EXPECT_EQ(sys_spawn_for_proc(t->proc, name, 5), -1,
        "embedded NUL inside name_len → -1");
}

void test_sys_wait_pid_no_children_returns_neg1(void) {
    drain_zombies();

    int status = 0;
    int rc = wait_pid(&status);
    TEST_EXPECT_EQ(rc, -1, "wait_pid with no children → -1");
}
