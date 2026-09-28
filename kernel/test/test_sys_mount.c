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
//     mount point mp with MREPL (a pipe is a file, and only a replacement
//     mounts at a file); territory_nmounts goes 0->1; the mount-table holds one
//     extra spoor_ref so the Spoor survives handle_close on the original fd.
//
//   sys_mount.idempotent_on_duplicate
//     A flagless mount of the same directory at the same directory point
//     twice; nmounts stays at 1 and the source's ref at 2 (no refcount churn).
//
//   sys_mount.rejects_bad_fd
//     Out-of-range / negative / closed fd -> -1.
//
//   sys_mount.rejects_missing_right_read
//     dup the source fd with reduced rights (no READ); -> -1.
//
//   sys_mount.rejects_invalid_flags
//     flags with bits outside MREPL|MBEFORE|MAFTER|MCREATE|MNOEXEC -> -1; no
//     entry. Plus the positive half: MNOEXEC (#217) IS accepted.
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
//
//   sys_mount.refuses_a_type_mismatch
//     Plan 9's Emount, first half (ARCH 9.6.1): a file over a directory and a
//     directory over a file -> -T_E_NOTDIR under every placement, no entry, the
//     source's ref unchanged. Controls: a directory over a directory and a file
//     over a file (MREPL) are accepted.
//
//   sys_mount.refuses_all_but_mrepl_at_a_file
//     Emount's second half: at a file point every mount without MREPL ->
//     -T_E_NOTDIR: MBEFORE, MAFTER, and a flagless mount (it appends here;
//     Plan 9's flag 0 is MREPL), each with every subset of MCREATE / MNOEXEC /
//     MPHENO_LINUX -- all 24 refused sets. MREPL with each subset is accepted,
//     each replacing the last, and a flagless mount beside the member it
//     installed is refused and leaves the point as it was.
//
//   sys_mount.type_check_reads_only_qtdir
//     The check compares directory-ness, not the type byte: a file carrying
//     QTAPPEND|QTEXCL over a plain file or a symlink point, and a directory
//     carrying QTAPPEND over a plain directory, are accepted; a directory over
//     a symlink point (the point is the link, DISTRO D-1) -> -T_E_NOTDIR.
//
//   sys_mount.accepts_an_ordered_mount_at_a_directory
//     The refusals' positive control through the inner: MBEFORE and MAFTER of
//     a directory over a directory each start a union (the new member and the
//     covered point), and the unmount takes both away with the point's ref.

#include "test.h"

#include <thylacine/dev.h>
#include <thylacine/errno.h>
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
void test_sys_mount_refuses_a_type_mismatch(void);
void test_sys_mount_refuses_all_but_mrepl_at_a_file(void);
void test_sys_mount_type_check_reads_only_qtdir(void);
void test_sys_mount_accepts_an_ordered_mount_at_a_directory(void);

// Mint a synthetic mount-point Spoor with a distinct identity (devnone dc '-',
// devno 0, the given qid.path). The mount table keys on (dc, devno, qid.path).
static struct Spoor *mkmp(u64 qid_path) {
    struct Spoor *mp = spoor_alloc(&devnone);
    if (mp) mp->qid.path = qid_path;
    return mp;
}

// A devnone Spoor of the given type, installed in `p`'s handle table with
// RIGHT_READ (the table owns the ref and clunks it at proc_free). *out gets the
// Spoor so a test can watch its ref.
static hidx_t install_typed_source(struct Proc *p, u8 type, u64 qid_path,
                                   struct Spoor **out) {
    struct Spoor *s = spoor_alloc(&devnone);
    if (!s) return -1;
    s->qid.path = qid_path;
    s->qid.type = type;
    *out = s;
    return handle_alloc(p, KOBJ_SPOOR, RIGHT_READ, s);
}

static struct Spoor *mkdirmp(u64 qid_path) {
    struct Spoor *mp = mkmp(qid_path);
    if (mp) mp->qid.type = QTDIR;
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
    TEST_EXPECT_EQ(sys_mount_for_proc(p, fd_rd, mp, MREPL), 0,
        "sys_mount returns 0");
    TEST_EXPECT_EQ(territory_nmounts(p->territory), 1,
        "one entry installed");

    spoor_unref(mp);
    drop_test_proc(p);
}

void test_sys_mount_idempotent_on_duplicate(void) {
    struct Proc *p = make_test_proc_with_territory();
    TEST_ASSERT(p != NULL, "proc + territory alloc");
    struct Spoor *dsrc = NULL;
    hidx_t dfd = install_typed_source(p, QTDIR, 101u, &dsrc);
    TEST_ASSERT(dfd >= 0, "install the directory source");
    struct Spoor *mp = mkdirmp(42u);
    TEST_ASSERT(mp != NULL, "mkmp");

    // Flagless, so the second call takes mount()'s converge path, which only a
    // directory point can reach from SYS_MOUNT.
    TEST_EXPECT_EQ(sys_mount_for_proc(p, dfd, mp, 0), 0, "first mount");
    TEST_EXPECT_EQ(territory_nmounts(p->territory), 1,
        "one entry after first mount");
    TEST_EXPECT_EQ(dsrc->ref, 2, "the handle and the entry hold the source");

    // Duplicate (same mount-point identity + same Spoor source) -> no-op
    // success. The C-API returns 0 without touching nmounts or the refcount.
    TEST_EXPECT_EQ(sys_mount_for_proc(p, dfd, mp, 0), 0,
        "duplicate mount is idempotent (returns 0)");
    TEST_EXPECT_EQ(territory_nmounts(p->territory), 1,
        "still one entry after duplicate");
    TEST_EXPECT_EQ(dsrc->ref, 2, "the duplicate took no ref");

    drop_test_proc(p);
    spoor_unref(mp);
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

    // Bits outside MREPL|MBEFORE|MAFTER|MCREATE|MNOEXEC|MPHENO_LINUX (= 0x003F).
    // This test used to pin 0x10 as invalid and CAUGHT #217 widening the allowlist
    // to include it, then 0x20 and CAUGHT VIVARIUM section 13 assigning it to
    // MPHENO_LINUX -- which is the allowlist working, not the test being in the
    // way. The assertion is re-pointed at the lowest still-unassigned bit (now
    // 0x40) rather than deleted: what it guards (junk bits are refused, and
    // refused WITHOUT installing an entry) is exactly as load-bearing as before,
    // and the next flag to land should trip it again.
    TEST_EXPECT_EQ(sys_mount_for_proc(p, fd_rd, mp, 0x40), -1,
        "flags 0x40 (the lowest unassigned bit) -> -1");
    TEST_EXPECT_EQ(sys_mount_for_proc(p, fd_rd, mp, 0xFFFFFFFFu), -1,
        "flags 0xFFFFFFFFu -> -1");
    // A valid bit ORed with a junk bit is still refused wholesale -- an
    // allowlist that masked junk off instead of rejecting would pass the two
    // assertions above and silently honour the request.
    TEST_EXPECT_EQ(sys_mount_for_proc(p, fd_rd, mp, MREPL | 0x40), -1,
        "a valid flag ORed with junk is refused, not masked");
    // UM-8 F10: MREPL / MBEFORE / MAFTER are mutually-exclusive placement modes;
    // more than one set is refused wholesale (mount()'s dispatch would silently
    // take MREPL then MBEFORE). Each bit is individually VALID -- only the
    // COMBINATION is rejected, so these survive the invalid-bit allowlist above.
    TEST_EXPECT_EQ(sys_mount_for_proc(p, fd_rd, mp, MBEFORE | MAFTER), -1,
        "MBEFORE|MAFTER (two placements) -> -1");
    TEST_EXPECT_EQ(sys_mount_for_proc(p, fd_rd, mp, MREPL | MBEFORE), -1,
        "MREPL|MBEFORE (two placements) -> -1");
    TEST_EXPECT_EQ(territory_nmounts(p->territory), 0,
        "no entry installed for invalid flags");

    // Valid flags accepted (MREPL).
    TEST_EXPECT_EQ(sys_mount_for_proc(p, fd_rd, mp, MREPL), 0,
        "MREPL flag accepted");
    // #217: and MNOEXEC is now in the allowlist -- the positive half, without
    // which the syscall boundary could reject the flag and nothing would say so.
    TEST_EXPECT_EQ(sys_mount_for_proc(p, fd_rd, mp, MREPL | MNOEXEC), 0,
        "MREPL|MNOEXEC accepted at the syscall boundary");
    // VIVARIUM section 13: MPHENO_LINUX (0x20) is likewise now in the allowlist --
    // userspace (joey composing /viv/bin) can SET it, or the phenotype channel is
    // unreachable from EL0. MREPL of the pair already mounted replaces it (the
    // group replace), so layering it onto the entry above must return 0.
    TEST_EXPECT_EQ(sys_mount_for_proc(p, fd_rd, mp, MREPL | MPHENO_LINUX), 0,
        "MREPL|MPHENO_LINUX accepted at the syscall boundary");

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
    TEST_EXPECT_EQ(sys_mount_for_proc(p, fd_rd, mp, MREPL), 0, "mount");
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
    TEST_EXPECT_EQ(sys_mount_for_proc(p, fd_rd, mp42, MREPL), 0, "mount at 42");
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
    TEST_EXPECT_EQ(sys_mount_for_proc(p, fd_rd, mp, MREPL), 0, "mount fd_rd");

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

void test_sys_mount_refuses_a_type_mismatch(void) {
    struct Proc *p = make_test_proc_with_territory();
    TEST_ASSERT(p != NULL, "proc + territory alloc");
    struct Spoor *fsrc = NULL, *dsrc = NULL;
    hidx_t ffd = install_typed_source(p, QTFILE, 100u, &fsrc);
    hidx_t dfd = install_typed_source(p, QTDIR, 101u, &dsrc);
    TEST_ASSERT(ffd >= 0 && dfd >= 0, "install the file and directory sources");
    struct Spoor *dir_mp  = mkdirmp(42u);
    struct Spoor *file_mp = mkmp(43u);
    TEST_ASSERT(dir_mp && file_mp, "mkmp");

    const u32 placements[4] = { 0u, MREPL, MBEFORE, MAFTER };
    for (int i = 0; i < 4; i++) {
        TEST_EXPECT_EQ(sys_mount_for_proc(p, ffd, dir_mp, placements[i]),
            -T_E_NOTDIR, "a file over a directory -> ENOTDIR");
        TEST_EXPECT_EQ(sys_mount_for_proc(p, dfd, file_mp, placements[i]),
            -T_E_NOTDIR, "a directory over a file -> ENOTDIR");
    }
    TEST_EXPECT_EQ(territory_nmounts(p->territory), 0, "nothing installed");
    TEST_EXPECT_EQ(fsrc->ref, 1, "the refusal released the file source's lookup ref");
    TEST_EXPECT_EQ(dsrc->ref, 1, "the refusal released the directory source's lookup ref");
    TEST_EXPECT_EQ(dir_mp->ref, 1, "the directory point is not retained");

    // Controls one variable away: the same sources over a point of their own
    // type are accepted.
    TEST_EXPECT_EQ(sys_mount_for_proc(p, dfd, dir_mp, MREPL), 0,
        "control: a directory over a directory");
    TEST_EXPECT_EQ(sys_mount_for_proc(p, ffd, file_mp, MREPL), 0,
        "control: a file over a file (MREPL)");
    TEST_EXPECT_EQ(territory_nmounts(p->territory), 2, "control: two entries");

    drop_test_proc(p);
    spoor_unref(dir_mp);
    spoor_unref(file_mp);
}

void test_sys_mount_refuses_all_but_mrepl_at_a_file(void) {
    struct Proc *p = make_test_proc_with_territory();
    TEST_ASSERT(p != NULL, "proc + territory alloc");
    struct Spoor *fsrc = NULL, *fsrc2 = NULL;
    hidx_t ffd  = install_typed_source(p, QTFILE, 100u, &fsrc);
    hidx_t ffd2 = install_typed_source(p, QTFILE, 101u, &fsrc2);
    TEST_ASSERT(ffd >= 0 && ffd2 >= 0, "install the file sources");
    struct Spoor *file_mp  = mkmp(43u);
    struct Spoor *file_mp2 = mkmp(44u);
    TEST_ASSERT(file_mp && file_mp2, "mkmp");

    // Every placement short of MREPL, each with every subset of the rest.
    const u32 placements[3] = { 0u, MBEFORE, MAFTER };
    const u32 extras[3]     = { MCREATE, MNOEXEC, MPHENO_LINUX };
    for (int i = 0; i < 3; i++) {
        for (u32 m = 0; m < 8u; m++) {
            u32 f = placements[i];
            for (int b = 0; b < 3; b++)
                if (m & (1u << b)) f |= extras[b];
            TEST_EXPECT_EQ(sys_mount_for_proc(p, ffd, file_mp, f), -T_E_NOTDIR,
                "a mount at a file without MREPL -> ENOTDIR");
        }
    }
    TEST_EXPECT_EQ(territory_nmounts(p->territory), 0, "nothing installed");
    TEST_EXPECT_EQ(fsrc->ref, 1, "the refusal released the source's lookup ref");

    for (u32 m = 0; m < 8u; m++) {
        u32 f = MREPL;
        for (int b = 0; b < 3; b++)
            if (m & (1u << b)) f |= extras[b];
        TEST_EXPECT_EQ(sys_mount_for_proc(p, ffd, file_mp, f), 0,
            "control: MREPL of the same pair, with each subset");
    }
    TEST_EXPECT_EQ(territory_nmounts(p->territory), 1,
        "control: each MREPL replaced the last");
    TEST_EXPECT_EQ(fsrc->ref, 2, "control: the table and one mount hold the source");
    TEST_EXPECT_EQ(sys_mount_for_proc(p, ffd, file_mp2, MREPL | MNOEXEC), 0,
        "control: MREPL|MNOEXEC of a file over a file");
    TEST_EXPECT_EQ(territory_nmounts(p->territory), 2, "control: two entries");

    // What the flagless refusal prevents: appended beside the file already
    // mounted there, a second file would make a two-member group that stalk
    // searches as a directory, and the mount would return 0 and never show.
    TEST_EXPECT_EQ(sys_mount_for_proc(p, ffd2, file_mp, 0u), -T_E_NOTDIR,
        "a flagless mount beside a mounted file -> ENOTDIR");
    TEST_EXPECT_EQ(territory_nmounts(p->territory), 2,
        "the point keeps its one member");
    TEST_EXPECT_EQ(fsrc2->ref, 1, "the refusal released the second source's ref");

    drop_test_proc(p);
    spoor_unref(file_mp);
    spoor_unref(file_mp2);
}

void test_sys_mount_type_check_reads_only_qtdir(void) {
    struct Proc *p = make_test_proc_with_territory();
    TEST_ASSERT(p != NULL, "proc + territory alloc");
    struct Spoor *fsrc = NULL, *dsrc = NULL;
    hidx_t ffd = install_typed_source(p, QTAPPEND | QTEXCL, 100u, &fsrc);
    hidx_t dfd = install_typed_source(p, QTDIR | QTAPPEND, 101u, &dsrc);
    TEST_ASSERT(ffd >= 0 && dfd >= 0, "install the file and directory sources");
    struct Spoor *link_mp = mkmp(45u);
    struct Spoor *file_mp = mkmp(46u);
    struct Spoor *dir_mp  = mkdirmp(47u);
    TEST_ASSERT(link_mp && file_mp && dir_mp, "mkmp");
    link_mp->qid.type = QTSYMLINK;

    TEST_EXPECT_EQ(sys_mount_for_proc(p, dfd, link_mp, MREPL), -T_E_NOTDIR,
        "a directory over a symlink point -> ENOTDIR");
    TEST_EXPECT_EQ(territory_nmounts(p->territory), 0, "nothing installed");
    TEST_EXPECT_EQ(sys_mount_for_proc(p, ffd, link_mp, MREPL), 0,
        "a file with QTAPPEND|QTEXCL over a symlink point (MREPL)");
    TEST_EXPECT_EQ(sys_mount_for_proc(p, ffd, file_mp, MREPL), 0,
        "a file with QTAPPEND|QTEXCL over a plain file (MREPL)");
    TEST_EXPECT_EQ(sys_mount_for_proc(p, dfd, dir_mp, MREPL), 0,
        "a directory with QTAPPEND over a plain directory");
    TEST_EXPECT_EQ(territory_nmounts(p->territory), 3, "three entries");

    drop_test_proc(p);
    spoor_unref(link_mp);
    spoor_unref(file_mp);
    spoor_unref(dir_mp);
}

void test_sys_mount_accepts_an_ordered_mount_at_a_directory(void) {
    struct Proc *p = make_test_proc_with_territory();
    TEST_ASSERT(p != NULL, "proc + territory alloc");
    struct Spoor *dsrc = NULL;
    hidx_t dfd = install_typed_source(p, QTDIR, 101u, &dsrc);
    TEST_ASSERT(dfd >= 0, "install the directory source");
    struct Spoor *dir_mp = mkdirmp(42u);
    TEST_ASSERT(dir_mp != NULL, "mkmp");

    const u32 ordered[2] = { MBEFORE, MAFTER };
    for (int i = 0; i < 2; i++) {
        TEST_EXPECT_EQ(sys_mount_for_proc(p, dfd, dir_mp, ordered[i]), 0,
            "an ordered mount of a directory over a directory");
        TEST_EXPECT_EQ(territory_nmounts(p->territory), 2,
            "the union holds the new member and the covered point");
        TEST_EXPECT_EQ(dir_mp->ref, 2, "the covered member holds the point");
        TEST_EXPECT_EQ(sys_unmount_for_proc(p, dir_mp), 0, "unmount");
        TEST_EXPECT_EQ(territory_nmounts(p->territory), 0,
            "the covered point left with the last member");
        TEST_EXPECT_EQ(dir_mp->ref, 1, "and released the point");
    }

    drop_test_proc(p);
    spoor_unref(dir_mp);
}
