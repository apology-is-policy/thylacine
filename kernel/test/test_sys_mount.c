// SYS_MOUNT / SYS_UNMOUNT integration tests (P5-mount-syscall).
//
// Exercises the SVC handler's integration with kernel/territory.c::mount
// and ::unmount. The handlers themselves (sys_mount_handler /
// sys_unmount_handler) are static SVC wrappers that bounce through the
// non-static inners sys_mount_for_proc / sys_unmount_for_proc; we call
// the inners directly with a kernel-allocated test Proc whose Territory
// is fresh, mirroring the pattern used by test_sys_pipe.c.
//
// The mount-table primitive's correctness is pinned by specs/territory.tla
// (MountRefcountConsistency + ForkClone) and exercised by test_territory_mount.c.
// THIS file's job is the syscall-boundary checks:
//
//   sys_mount.happy_path_grafts_pipe_spoor
//     sys_pipe_for_proc gives a KOBJ_SPOOR fd; sys_mount_for_proc
//     grafts it at path_id=42; territory_nmounts goes 0→1; the
//     mount-table holds one extra spoor_ref so the Spoor survives
//     handle_close on the original fd.
//
//   sys_mount.idempotent_on_duplicate
//     Mount the same (target, source) twice; nmounts stays at 1; no
//     refcount churn (matches territory.c::mount's no-op-on-duplicate
//     precondition).
//
//   sys_mount.rejects_bad_fd
//     Out-of-range fd → -1; wrong-kind fd (we'd need a non-KOBJ_SPOOR
//     here, but proc_alloc's handle table starts empty so 9999 is
//     out-of-range and gives the same -1) → -1.
//
//   sys_mount.rejects_missing_right_read
//     dup the source fd with reduced rights (T_RIGHT_WRITE only — no
//     READ); sys_mount_for_proc on the dup'd fd returns -1.
//
//   sys_mount.rejects_invalid_flags
//     flags with bits outside MREPL|MBEFORE|MAFTER|MCREATE → -1; no
//     entry installed.
//
//   sys_mount.rejects_null_territory
//     sys_mount_for_proc on a Proc with NULL territory → -1.
//
//   sys_unmount.removes_entry_and_drops_ref
//     Set up a mount; sys_unmount_for_proc on the same target_path_id;
//     nmounts goes 1→0; the Spoor's ring is freed when the user's last
//     fd is also closed (proves the mount-table held the per-entry ref
//     while it was installed).
//
//   sys_unmount.rejects_nonexistent_target
//     sys_unmount_for_proc on an unmounted path_id → -1.
//
//   sys_mount.caller_close_keeps_mount_alive
//     After mount, t_close-equivalent (handle_close) on the source fd;
//     mount-table entry survives; the Spoor is freed only after
//     sys_unmount AND the source fd was already closed.

#include "test.h"

#include <thylacine/handle.h>
#include <thylacine/pipe.h>
#include <thylacine/proc.h>
#include <thylacine/spoor.h>
#include <thylacine/territory.h>
#include <thylacine/types.h>

// Inner SVC handlers (extern; defined in kernel/syscall.c).
extern int sys_pipe_for_proc(struct Proc *p, hidx_t *out_rd, hidx_t *out_wr);
extern int sys_mount_for_proc(struct Proc *p, hidx_t source_fd,
                              path_id_t target, u32 flags);
extern int sys_unmount_for_proc(struct Proc *p, path_id_t target);

void test_sys_mount_happy_path_grafts_pipe_spoor(void);
void test_sys_mount_idempotent_on_duplicate(void);
void test_sys_mount_rejects_bad_fd(void);
void test_sys_mount_rejects_missing_right_read(void);
void test_sys_mount_rejects_invalid_flags(void);
void test_sys_mount_rejects_null_territory(void);
void test_sys_unmount_removes_entry_and_drops_ref(void);
void test_sys_unmount_rejects_nonexistent_target(void);
void test_sys_mount_caller_close_keeps_mount_alive(void);

// Test Proc helper. Mirrors test_sys_pipe.c::make_test_proc but also
// installs a fresh Territory so the mount-table primitives have a place
// to write. proc_free's territory_unref releases the test Territory on
// drop_test_proc.
static struct Proc *make_test_proc_with_territory(void) {
    struct Proc *p = proc_alloc();
    if (!p) return NULL;
    p->territory = territory_alloc();
    if (!p->territory) {
        p->state = PROC_STATE_ZOMBIE;
        proc_free(p);
        return NULL;
    }
    return p;
}

static void drop_test_proc(struct Proc *p) {
    if (!p) return;
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

void test_sys_mount_happy_path_grafts_pipe_spoor(void) {
    struct Proc *p = make_test_proc_with_territory();
    TEST_ASSERT(p != NULL, "proc + territory alloc");

    hidx_t fd_rd = -1, fd_wr = -1;
    TEST_EXPECT_EQ(sys_pipe_for_proc(p, &fd_rd, &fd_wr), 0, "sys_pipe");
    TEST_EXPECT_EQ(territory_nmounts(p->territory), 0,
        "no mounts before");

    // Mount the read end at target_path_id=42. territory.c::mount
    // bumps the Spoor's refcount; the handle still holds its own ref.
    TEST_EXPECT_EQ(sys_mount_for_proc(p, fd_rd, 42, 0), 0,
        "sys_mount returns 0");
    TEST_EXPECT_EQ(territory_nmounts(p->territory), 1,
        "one entry installed");

    drop_test_proc(p);
}

void test_sys_mount_idempotent_on_duplicate(void) {
    struct Proc *p = make_test_proc_with_territory();
    TEST_ASSERT(p != NULL, "proc + territory alloc");

    hidx_t fd_rd = -1, fd_wr = -1;
    TEST_EXPECT_EQ(sys_pipe_for_proc(p, &fd_rd, &fd_wr), 0, "sys_pipe");

    TEST_EXPECT_EQ(sys_mount_for_proc(p, fd_rd, 42, 0), 0, "first mount");
    TEST_EXPECT_EQ(territory_nmounts(p->territory), 1,
        "one entry after first mount");

    // Duplicate (same target + same Spoor source) → no-op success.
    // Spec invariant: <<path, s>> \notin mounts[p] precondition; the
    // C-API returns 0 without touching nmounts or the refcount.
    TEST_EXPECT_EQ(sys_mount_for_proc(p, fd_rd, 42, 0), 0,
        "duplicate mount is idempotent (returns 0)");
    TEST_EXPECT_EQ(territory_nmounts(p->territory), 1,
        "still one entry after duplicate");

    drop_test_proc(p);
}

void test_sys_mount_rejects_bad_fd(void) {
    struct Proc *p = make_test_proc_with_territory();
    TEST_ASSERT(p != NULL, "proc + territory alloc");

    // Out-of-range fd. handle_get rejects via h < 0 || h >= PROC_HANDLE_MAX.
    TEST_EXPECT_EQ(sys_mount_for_proc(p, (hidx_t)9999, 42, 0), -1,
        "mount with out-of-range fd → -1");
    // Negative fd (raw u64 → hidx_t saturates to negative int).
    TEST_EXPECT_EQ(sys_mount_for_proc(p, (hidx_t)-1, 42, 0), -1,
        "mount with negative fd → -1");
    // Closed fd. Allocate a pipe, close the read end, then try to mount it.
    hidx_t fd_rd = -1, fd_wr = -1;
    TEST_EXPECT_EQ(sys_pipe_for_proc(p, &fd_rd, &fd_wr), 0, "sys_pipe");
    TEST_EXPECT_EQ(handle_close(p, fd_rd), 0, "close fd_rd");
    TEST_EXPECT_EQ(sys_mount_for_proc(p, fd_rd, 42, 0), -1,
        "mount with closed fd → -1");
    TEST_EXPECT_EQ(territory_nmounts(p->territory), 0,
        "no entries installed");

    drop_test_proc(p);
}

void test_sys_mount_rejects_missing_right_read(void) {
    struct Proc *p = make_test_proc_with_territory();
    TEST_ASSERT(p != NULL, "proc + territory alloc");

    hidx_t fd_rd = -1, fd_wr = -1;
    TEST_EXPECT_EQ(sys_pipe_for_proc(p, &fd_rd, &fd_wr), 0, "sys_pipe");

    // Dup the read fd with WRITE-only rights (subset of original
    // READ|WRITE|TRANSFER). handle_dup's RightsCeiling check accepts;
    // the resulting handle has WRITE but not READ.
    hidx_t fd_wronly = handle_dup(p, fd_rd, RIGHT_WRITE);
    TEST_ASSERT(fd_wronly >= 0, "dup with WRITE-only succeeded");

    // sys_mount_for_proc requires RIGHT_READ on the source handle.
    TEST_EXPECT_EQ(sys_mount_for_proc(p, fd_wronly, 42, 0), -1,
        "mount on WRITE-only fd → -1");
    TEST_EXPECT_EQ(territory_nmounts(p->territory), 0,
        "no entry installed");

    drop_test_proc(p);
}

void test_sys_mount_rejects_invalid_flags(void) {
    struct Proc *p = make_test_proc_with_territory();
    TEST_ASSERT(p != NULL, "proc + territory alloc");

    hidx_t fd_rd = -1, fd_wr = -1;
    TEST_EXPECT_EQ(sys_pipe_for_proc(p, &fd_rd, &fd_wr), 0, "sys_pipe");

    // Bits outside MREPL|MBEFORE|MAFTER|MCREATE (= 0x000F).
    TEST_EXPECT_EQ(sys_mount_for_proc(p, fd_rd, 42, 0x10), -1,
        "flags 0x10 → -1");
    TEST_EXPECT_EQ(sys_mount_for_proc(p, fd_rd, 42, 0xFFFFFFFFu), -1,
        "flags 0xFFFFFFFFu → -1");
    TEST_EXPECT_EQ(territory_nmounts(p->territory), 0,
        "no entry installed for invalid flags");

    // Valid flags accepted (MREPL).
    TEST_EXPECT_EQ(sys_mount_for_proc(p, fd_rd, 42, MREPL), 0,
        "MREPL flag accepted");

    drop_test_proc(p);
}

void test_sys_mount_rejects_null_territory(void) {
    // Allocate a Proc WITHOUT a Territory (the test_sys_pipe pattern).
    // sys_mount_for_proc must reject before touching anything.
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc");
    TEST_ASSERT(p->territory == NULL, "Proc has no territory");

    TEST_EXPECT_EQ(sys_mount_for_proc(p, 0, 42, 0), -1,
        "mount on NULL-territory Proc → -1");
    TEST_EXPECT_EQ(sys_unmount_for_proc(p, 42), -1,
        "unmount on NULL-territory Proc → -1");

    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

void test_sys_unmount_removes_entry_and_drops_ref(void) {
    struct Proc *p = make_test_proc_with_territory();
    TEST_ASSERT(p != NULL, "proc + territory alloc");
    u64 pipe_freed_before  = pipe_total_freed();

    hidx_t fd_rd = -1, fd_wr = -1;
    TEST_EXPECT_EQ(sys_pipe_for_proc(p, &fd_rd, &fd_wr), 0, "sys_pipe");
    TEST_EXPECT_EQ(sys_mount_for_proc(p, fd_rd, 42, 0), 0, "mount");
    TEST_EXPECT_EQ(territory_nmounts(p->territory), 1, "1 mount");

    // Close the handle table fd. Mount-table entry's ref keeps the
    // Spoor (and its ring) alive. The other pipe end (fd_wr) is also
    // closed below; the ring's per-endpoint refcount is then 1 — held
    // by the mount-table entry's spoor_ref → endpoint priv → ring ref.
    TEST_EXPECT_EQ(handle_close(p, fd_rd), 0, "close fd_rd");
    TEST_EXPECT_EQ(handle_close(p, fd_wr), 0, "close fd_wr");
    TEST_EXPECT_EQ(pipe_total_freed() - pipe_freed_before, 0ull,
        "ring still alive — mount-table holds the ref");

    // Unmount drops the per-entry ref; ring is now freed.
    TEST_EXPECT_EQ(sys_unmount_for_proc(p, 42), 0, "unmount");
    TEST_EXPECT_EQ(territory_nmounts(p->territory), 0, "no mounts");
    TEST_EXPECT_EQ(pipe_total_freed() - pipe_freed_before, 1ull,
        "ring freed after unmount");

    drop_test_proc(p);
}

void test_sys_unmount_rejects_nonexistent_target(void) {
    struct Proc *p = make_test_proc_with_territory();
    TEST_ASSERT(p != NULL, "proc + territory alloc");

    // No mounts yet; any unmount should fail.
    TEST_EXPECT_EQ(sys_unmount_for_proc(p, 42), -1,
        "unmount of unmounted path → -1");

    // Add a mount; unmount of a DIFFERENT path still fails.
    hidx_t fd_rd = -1, fd_wr = -1;
    TEST_EXPECT_EQ(sys_pipe_for_proc(p, &fd_rd, &fd_wr), 0, "sys_pipe");
    TEST_EXPECT_EQ(sys_mount_for_proc(p, fd_rd, 42, 0), 0, "mount at 42");
    TEST_EXPECT_EQ(sys_unmount_for_proc(p, 43), -1,
        "unmount of unrelated path → -1");
    TEST_EXPECT_EQ(territory_nmounts(p->territory), 1,
        "mount at 42 intact");

    drop_test_proc(p);
}

void test_sys_mount_caller_close_keeps_mount_alive(void) {
    // The lifecycle invariant from ARCH §9.6.6: "Caller can close the
    // attach_9p fd after `mount` — the mount table holds the ref."
    //
    // Verify the same property for the pipe-Spoor case: mount(fd) +
    // close(fd) is legal; the mount-table entry's ref keeps the Spoor
    // alive. Drop the Territory (via proc_free → territory_unref →
    // mount-entry's spoor_unref) and only THEN is the ring freed.
    struct Proc *p = make_test_proc_with_territory();
    TEST_ASSERT(p != NULL, "proc + territory alloc");
    u64 pipe_freed_before  = pipe_total_freed();
    u64 spoor_freed_before = spoor_total_freed();

    hidx_t fd_rd = -1, fd_wr = -1;
    TEST_EXPECT_EQ(sys_pipe_for_proc(p, &fd_rd, &fd_wr), 0, "sys_pipe");
    TEST_EXPECT_EQ(sys_mount_for_proc(p, fd_rd, 42, 0), 0, "mount fd_rd");

    // Caller closes the source fd. The mount-table's ref keeps the
    // Spoor alive.
    TEST_EXPECT_EQ(handle_close(p, fd_rd), 0, "close fd_rd");
    TEST_EXPECT_EQ(pipe_total_freed() - pipe_freed_before, 0ull,
        "ring not freed — mount-table holds ref");
    TEST_EXPECT_EQ(spoor_total_freed() - spoor_freed_before, 0ull,
        "Spoor not freed — mount-table holds ref");

    // Close fd_wr too — the ring's other endpoint ref is dropped.
    // Ring is still alive (mount-table's ref via fd_rd's Spoor).
    TEST_EXPECT_EQ(handle_close(p, fd_wr), 0, "close fd_wr");
    TEST_EXPECT_EQ(pipe_total_freed() - pipe_freed_before, 0ull,
        "ring still alive after both user fds closed");

    // Territory destruction (via proc_free) drops the mount-entry's
    // ref. THAT's when the ring + Spoors finally go.
    drop_test_proc(p);
    TEST_EXPECT_EQ(pipe_total_freed() - pipe_freed_before, 1ull,
        "ring freed by Territory destruction");
    TEST_EXPECT_EQ(spoor_total_freed() - spoor_freed_before, 2ull,
        "both Spoors freed");
}
