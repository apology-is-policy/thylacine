// SYS_MOUNT / SYS_UNMOUNT integration tests (P5-mount-syscall; stalk-2 re-key).
//
// Exercises the SVC handler's INNER integration with kernel/territory.c::mount
// and ::unmount. The path-resolution half (sys_mount_handler -> stalk ->
// mount-point Spoor) is exercised end-to-end by the userspace probes
// (/attach-probe, /stub-driver) + the joey cross-mount E2E; THIS file drives
// the inners sys_mount_for_proc / sys_unmount_for_proc directly with a
// kernel-allocated test Proc and a synthetic mount-point Spoor (mkmp), so the
// rights gate + flags check + table op are unit-tested without needing a
// resolvable namespace in the test Proc.
//
// stalk-2: the inners now take the RESOLVED mount-point Spoor (was a path_id_t
// target). mkmp() mints a devnone Spoor with a distinct qid.path; the mount
// table keys on its (dc, devno, qid.path) identity.
//
//   sys_mount.happy_path_grafts_pipe_spoor
//     sys_pipe_for_proc gives a KOBJ_SPOOR fd; sys_mount_for_proc grafts it at
//     mount point mp; territory_nmounts goes 0->1; the mount-table holds one
//     extra spoor_ref so the Spoor survives handle_close on the original fd.
//
//   sys_mount.idempotent_on_duplicate
//     Mount the same (mp, source) twice; nmounts stays at 1; no refcount churn.
//
//   sys_mount.rejects_bad_fd
//     Out-of-range / negative / closed fd -> -1.
//
//   sys_mount.rejects_missing_right_read
//     dup the source fd with reduced rights (no READ); -> -1.
//
//   sys_mount.rejects_invalid_flags
//     flags with bits outside MREPL|MBEFORE|MAFTER|MCREATE -> -1; no entry.
//
//   sys_mount.rejects_null_territory
//     sys_mount_for_proc on a Proc with NULL territory -> -1.
//
//   sys_unmount.removes_entry_and_drops_ref
//     Set up a mount; sys_unmount_for_proc on the same mount point; nmounts
//     1->0; the Spoor's ring is freed when the user's last fd is also closed.
//
//   sys_unmount.rejects_nonexistent_target
//     sys_unmount_for_proc on an un-mounted mount point -> -1.
//
//   sys_mount.caller_close_keeps_mount_alive
//     After mount, handle_close on the source fd; the mount-table entry's ref
//     keeps the Spoor alive; only Territory destruction frees it.

#include "test.h"

#include <thylacine/dev.h>
#include <thylacine/handle.h>
#include <thylacine/pipe.h>
#include <thylacine/proc.h>
#include <thylacine/spoor.h>
#include <thylacine/territory.h>
#include <thylacine/types.h>

extern struct Dev devnone;

// Inner SVC handlers (extern; defined in kernel/syscall.c) -- stalk-2 Spoor-keyed.
extern int sys_pipe_for_proc(struct Proc *p, hidx_t *out_rd, hidx_t *out_wr);
extern int sys_mount_for_proc(struct Proc *p, hidx_t source_fd,
                              struct Spoor *mountpoint, u32 flags);
extern int sys_unmount_for_proc(struct Proc *p, struct Spoor *mountpoint);

void test_sys_mount_happy_path_grafts_pipe_spoor(void);
void test_sys_mount_idempotent_on_duplicate(void);
void test_sys_mount_rejects_bad_fd(void);
void test_sys_mount_rejects_missing_right_read(void);
void test_sys_mount_rejects_invalid_flags(void);
void test_sys_mount_rejects_null_territory(void);
void test_sys_unmount_removes_entry_and_drops_ref(void);
void test_sys_unmount_rejects_nonexistent_target(void);
void test_sys_mount_caller_close_keeps_mount_alive(void);

// Mint a synthetic mount-point Spoor with a distinct identity (devnone dc '-',
// devno 0, the given qid.path). The mount table keys on (dc, devno, qid.path).
static struct Spoor *mkmp(u64 qid_path) {
    struct Spoor *mp = spoor_alloc(&devnone);
    if (mp) mp->qid.path = qid_path;
    return mp;
}

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
    struct Spoor *mp = mkmp(42u);
    TEST_ASSERT(mp != NULL, "mkmp");

    hidx_t fd_rd = -1, fd_wr = -1;
    TEST_EXPECT_EQ(sys_pipe_for_proc(p, &fd_rd, &fd_wr), 0, "sys_pipe");
    TEST_EXPECT_EQ(territory_nmounts(p->territory), 0,
        "no mounts before");

    // Mount the read end at mount point mp. territory.c::mount
    // bumps the Spoor's refcount; the handle still holds its own ref.
    TEST_EXPECT_EQ(sys_mount_for_proc(p, fd_rd, mp, 0), 0,
        "sys_mount returns 0");
    TEST_EXPECT_EQ(territory_nmounts(p->territory), 1,
        "one entry installed");

    spoor_unref(mp);
    drop_test_proc(p);
}

void test_sys_mount_idempotent_on_duplicate(void) {
    struct Proc *p = make_test_proc_with_territory();
    TEST_ASSERT(p != NULL, "proc + territory alloc");
    struct Spoor *mp = mkmp(42u);
    TEST_ASSERT(mp != NULL, "mkmp");

    hidx_t fd_rd = -1, fd_wr = -1;
    TEST_EXPECT_EQ(sys_pipe_for_proc(p, &fd_rd, &fd_wr), 0, "sys_pipe");

    TEST_EXPECT_EQ(sys_mount_for_proc(p, fd_rd, mp, 0), 0, "first mount");
    TEST_EXPECT_EQ(territory_nmounts(p->territory), 1,
        "one entry after first mount");

    // Duplicate (same mount-point identity + same Spoor source) -> no-op
    // success. The C-API returns 0 without touching nmounts or the refcount.
    TEST_EXPECT_EQ(sys_mount_for_proc(p, fd_rd, mp, 0), 0,
        "duplicate mount is idempotent (returns 0)");
    TEST_EXPECT_EQ(territory_nmounts(p->territory), 1,
        "still one entry after duplicate");

    spoor_unref(mp);
    drop_test_proc(p);
}

void test_sys_mount_rejects_bad_fd(void) {
    struct Proc *p = make_test_proc_with_territory();
    TEST_ASSERT(p != NULL, "proc + territory alloc");
    struct Spoor *mp = mkmp(42u);
    TEST_ASSERT(mp != NULL, "mkmp");

    // Out-of-range fd. handle_get rejects via h < 0 || h >= PROC_HANDLE_MAX.
    TEST_EXPECT_EQ(sys_mount_for_proc(p, (hidx_t)9999, mp, 0), -1,
        "mount with out-of-range fd -> -1");
    // Negative fd (raw u64 -> hidx_t saturates to negative int).
    TEST_EXPECT_EQ(sys_mount_for_proc(p, (hidx_t)-1, mp, 0), -1,
        "mount with negative fd -> -1");
    // Closed fd. Allocate a pipe, close the read end, then try to mount it.
    hidx_t fd_rd = -1, fd_wr = -1;
    TEST_EXPECT_EQ(sys_pipe_for_proc(p, &fd_rd, &fd_wr), 0, "sys_pipe");
    TEST_EXPECT_EQ(handle_close(p, fd_rd), 0, "close fd_rd");
    TEST_EXPECT_EQ(sys_mount_for_proc(p, fd_rd, mp, 0), -1,
        "mount with closed fd -> -1");
    TEST_EXPECT_EQ(territory_nmounts(p->territory), 0,
        "no entries installed");

    spoor_unref(mp);
    drop_test_proc(p);
}

void test_sys_mount_rejects_missing_right_read(void) {
    struct Proc *p = make_test_proc_with_territory();
    TEST_ASSERT(p != NULL, "proc + territory alloc");
    struct Spoor *mp = mkmp(42u);
    TEST_ASSERT(mp != NULL, "mkmp");

    hidx_t fd_rd = -1, fd_wr = -1;
    TEST_EXPECT_EQ(sys_pipe_for_proc(p, &fd_rd, &fd_wr), 0, "sys_pipe");

    // Dup the read fd with WRITE-only rights (subset of original
    // READ|WRITE|TRANSFER). The resulting handle has WRITE but not READ.
    hidx_t fd_wronly = handle_dup(p, fd_rd, RIGHT_WRITE);
    TEST_ASSERT(fd_wronly >= 0, "dup with WRITE-only succeeded");

    // sys_mount_for_proc requires RIGHT_READ on the source handle.
    TEST_EXPECT_EQ(sys_mount_for_proc(p, fd_wronly, mp, 0), -1,
        "mount on WRITE-only fd -> -1");
    TEST_EXPECT_EQ(territory_nmounts(p->territory), 0,
        "no entry installed");

    spoor_unref(mp);
    drop_test_proc(p);
}

void test_sys_mount_rejects_invalid_flags(void) {
    struct Proc *p = make_test_proc_with_territory();
    TEST_ASSERT(p != NULL, "proc + territory alloc");
    struct Spoor *mp = mkmp(42u);
    TEST_ASSERT(mp != NULL, "mkmp");

    hidx_t fd_rd = -1, fd_wr = -1;
    TEST_EXPECT_EQ(sys_pipe_for_proc(p, &fd_rd, &fd_wr), 0, "sys_pipe");

    // Bits outside MREPL|MBEFORE|MAFTER|MCREATE (= 0x000F).
    TEST_EXPECT_EQ(sys_mount_for_proc(p, fd_rd, mp, 0x10), -1,
        "flags 0x10 -> -1");
    TEST_EXPECT_EQ(sys_mount_for_proc(p, fd_rd, mp, 0xFFFFFFFFu), -1,
        "flags 0xFFFFFFFFu -> -1");
    TEST_EXPECT_EQ(territory_nmounts(p->territory), 0,
        "no entry installed for invalid flags");

    // Valid flags accepted (MREPL).
    TEST_EXPECT_EQ(sys_mount_for_proc(p, fd_rd, mp, MREPL), 0,
        "MREPL flag accepted");

    spoor_unref(mp);
    drop_test_proc(p);
}

void test_sys_mount_rejects_null_territory(void) {
    // Allocate a Proc WITHOUT a Territory (the test_sys_pipe pattern).
    // sys_mount_for_proc must reject before touching anything.
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc");
    TEST_ASSERT(p->territory == NULL, "Proc has no territory");
    struct Spoor *mp = mkmp(42u);
    TEST_ASSERT(mp != NULL, "mkmp");

    TEST_EXPECT_EQ(sys_mount_for_proc(p, 0, mp, 0), -1,
        "mount on NULL-territory Proc -> -1");
    TEST_EXPECT_EQ(sys_unmount_for_proc(p, mp), -1,
        "unmount on NULL-territory Proc -> -1");

    spoor_unref(mp);
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

void test_sys_unmount_removes_entry_and_drops_ref(void) {
    struct Proc *p = make_test_proc_with_territory();
    TEST_ASSERT(p != NULL, "proc + territory alloc");
    struct Spoor *mp = mkmp(42u);
    TEST_ASSERT(mp != NULL, "mkmp");
    u64 pipe_freed_before  = pipe_total_freed();

    hidx_t fd_rd = -1, fd_wr = -1;
    TEST_EXPECT_EQ(sys_pipe_for_proc(p, &fd_rd, &fd_wr), 0, "sys_pipe");
    TEST_EXPECT_EQ(sys_mount_for_proc(p, fd_rd, mp, 0), 0, "mount");
    TEST_EXPECT_EQ(territory_nmounts(p->territory), 1, "1 mount");

    // Close the handle table fds. Mount-table entry's ref keeps the
    // Spoor (and its ring) alive.
    TEST_EXPECT_EQ(handle_close(p, fd_rd), 0, "close fd_rd");
    TEST_EXPECT_EQ(handle_close(p, fd_wr), 0, "close fd_wr");
    TEST_EXPECT_EQ(pipe_total_freed() - pipe_freed_before, 0ull,
        "ring still alive — mount-table holds the ref");

    // Unmount drops the per-entry ref; ring is now freed.
    TEST_EXPECT_EQ(sys_unmount_for_proc(p, mp), 0, "unmount");
    TEST_EXPECT_EQ(territory_nmounts(p->territory), 0, "no mounts");
    TEST_EXPECT_EQ(pipe_total_freed() - pipe_freed_before, 1ull,
        "ring freed after unmount");

    spoor_unref(mp);
    drop_test_proc(p);
}

void test_sys_unmount_rejects_nonexistent_target(void) {
    struct Proc *p = make_test_proc_with_territory();
    TEST_ASSERT(p != NULL, "proc + territory alloc");
    struct Spoor *mp42 = mkmp(42u);
    struct Spoor *mp43 = mkmp(43u);
    TEST_ASSERT(mp42 != NULL && mp43 != NULL, "mkmp");

    // No mounts yet; any unmount should fail.
    TEST_EXPECT_EQ(sys_unmount_for_proc(p, mp42), -1,
        "unmount of unmounted point -> -1");

    // Add a mount; unmount of a DIFFERENT mount point still fails.
    hidx_t fd_rd = -1, fd_wr = -1;
    TEST_EXPECT_EQ(sys_pipe_for_proc(p, &fd_rd, &fd_wr), 0, "sys_pipe");
    TEST_EXPECT_EQ(sys_mount_for_proc(p, fd_rd, mp42, 0), 0, "mount at 42");
    TEST_EXPECT_EQ(sys_unmount_for_proc(p, mp43), -1,
        "unmount of unrelated point -> -1");
    TEST_EXPECT_EQ(territory_nmounts(p->territory), 1,
        "mount at 42 intact");

    spoor_unref(mp42);
    spoor_unref(mp43);
    drop_test_proc(p);
}

void test_sys_mount_caller_close_keeps_mount_alive(void) {
    // The lifecycle invariant from ARCH §9.6.6: "Caller can close the
    // attach_9p fd after `mount` — the mount table holds the ref."
    //
    // Verify the same property for the pipe-Spoor case: mount(fd) +
    // close(fd) is legal; the mount-table entry's ref keeps the Spoor
    // alive. Drop the Territory (via proc_free -> territory_unref ->
    // mount-entry's spoor_unref) and only THEN is the ring freed.
    struct Proc *p = make_test_proc_with_territory();
    TEST_ASSERT(p != NULL, "proc + territory alloc");
    u64 pipe_freed_before  = pipe_total_freed();
    u64 spoor_freed_before = spoor_total_freed();
    // The mount-point Spoor (mkmp) is a THIRD Spoor; keep it alive across the
    // "both Spoors freed == 2" assertion (the two pipe Spoors), then unref it.
    struct Spoor *mp = mkmp(42u);
    TEST_ASSERT(mp != NULL, "mkmp");

    hidx_t fd_rd = -1, fd_wr = -1;
    TEST_EXPECT_EQ(sys_pipe_for_proc(p, &fd_rd, &fd_wr), 0, "sys_pipe");
    TEST_EXPECT_EQ(sys_mount_for_proc(p, fd_rd, mp, 0), 0, "mount fd_rd");

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
    // ref. THAT's when the ring + the two pipe Spoors finally go. The mp
    // Spoor is still alive here (its own ref), so the delta is exactly 2.
    drop_test_proc(p);
    TEST_EXPECT_EQ(pipe_total_freed() - pipe_freed_before, 1ull,
        "ring freed by Territory destruction");
    TEST_EXPECT_EQ(spoor_total_freed() - spoor_freed_before, 2ull,
        "both pipe Spoors freed");

    spoor_unref(mp);
}
