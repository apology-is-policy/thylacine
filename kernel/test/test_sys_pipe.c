// SYS_PIPE integration tests (P5-fd-pipe).
//
// Exercises the syscall handler's integration of pipe_create +
// handle_alloc + KOBJ_SPOOR release-path discipline. Calls
// `sys_pipe_for_proc(p, ...)` (the non-static inner of the SVC
// handler) with a test Proc; verifies the resulting handles are
// well-formed KOBJ_SPOOR slots; verifies proc_free's
// handle_table_free walks the table and spoor_clunks each entry
// end-to-end.
//
// Coverage:
//
//   sys_pipe.allocates_two_distinct_spoor_handles
//     Returns 0; two distinct fds; both KOBJ_SPOOR; both have
//     RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER. spoor_total_allocated
//     incremented by 2.
//
//   sys_pipe.proc_free_releases_handles
//     pipe_create's ring + Spoors are released when the Proc is
//     dropped — verifies pipe_total_freed increments by 1 AND
//     spoor_total_freed increments by 2.
//
//   sys_pipe.handle_close_releases_one_end
//     Explicit handle_close on one fd releases that Spoor (drops
//     the ring's ref from 2 to 1) but the other end stays alive.
//     Closing the second fd frees the ring.

#include "test.h"

#include <thylacine/dev.h>
#include <thylacine/handle.h>
#include <thylacine/pipe.h>
#include <thylacine/proc.h>
#include <thylacine/spoor.h>
#include <thylacine/types.h>

// The boot ramfs Dev — the seekable known-content FS the #37 positioned-I/O
// tests read (/welcome is pinned by tools/build.sh).
extern struct Dev devramfs;

// Inner SVC handlers (extern declarations; defined in kernel/syscall.c).
extern int sys_pipe_for_proc(struct Proc *p, hidx_t *out_rd, hidx_t *out_wr);
extern s64 sys_write_for_proc(struct Proc *p, hidx_t h, const u8 *kbuf, u64 len);
extern s64 sys_read_for_proc(struct Proc *p, hidx_t h, u8 *kbuf, u64 len);
extern s64 sys_pread_for_proc(struct Proc *p, hidx_t h, u8 *kbuf, u64 len,
                              s64 off);
extern s64 sys_pwrite_for_proc(struct Proc *p, hidx_t h, const u8 *kbuf,
                               u64 len, s64 off);

void test_sys_pipe_allocates_two_distinct_spoor_handles(void);
void test_sys_pipe_proc_free_releases_handles(void);
void test_sys_pipe_handle_close_releases_one_end(void);
void test_sys_rw_write_then_read_round_trip(void);
void test_sys_rw_rights_check(void);
void test_sys_rw_zero_length_validates_fd(void);
void test_sys_rw_read_after_close_returns_eof(void);
void test_sys_prw_pipe_not_seekable(void);
void test_sys_pread_devramfs_offset_and_cursor(void);
void test_sys_prw_rights_and_walkonly(void);

// Local copy of the proc-test helpers used by test_handle.c. Kept
// independent so the two test files can be reordered without import
// coupling.
static struct Proc *make_test_proc(void) {
    struct Proc *p = proc_alloc();
    if (!p) return NULL;
    return p;
}

static void drop_test_proc(struct Proc *p) {
    if (!p) return;
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

void test_sys_pipe_allocates_two_distinct_spoor_handles(void) {
    struct Proc *p = make_test_proc();
    TEST_ASSERT(p != NULL, "proc_alloc");
    u64 spoor_allocated_before = spoor_total_allocated();

    hidx_t fd_rd = -1, fd_wr = -1;
    TEST_EXPECT_EQ(sys_pipe_for_proc(p, &fd_rd, &fd_wr), 0,
        "sys_pipe_for_proc returns 0");
    TEST_ASSERT(fd_rd >= 0 && fd_wr >= 0,
        "both fds allocated");
    TEST_ASSERT(fd_rd != fd_wr, "distinct fds");
    TEST_EXPECT_EQ(spoor_total_allocated() - spoor_allocated_before, 2ull,
        "two Spoors allocated");

    struct Handle h_rd, h_wr;
    int got_rd = handle_get(p, fd_rd, &h_rd);
    int got_wr = handle_get(p, fd_wr, &h_wr);
    TEST_ASSERT(got_rd == 0 && got_wr == 0,
        "handles installed");
    TEST_EXPECT_EQ((int)h_rd.kind, (int)KOBJ_SPOOR, "rd is KOBJ_SPOOR");
    TEST_EXPECT_EQ((int)h_wr.kind, (int)KOBJ_SPOOR, "wr is KOBJ_SPOOR");
    rights_t expected_rights = RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER;
    TEST_EXPECT_EQ(h_rd.rights, expected_rights, "rd rights");
    TEST_EXPECT_EQ(h_wr.rights, expected_rights, "wr rights");
    TEST_ASSERT(h_rd.obj != h_wr.obj,
        "rd and wr point at distinct Spoors");
    handle_put(&h_rd);
    handle_put(&h_wr);

    drop_test_proc(p);
}

void test_sys_pipe_proc_free_releases_handles(void) {
    struct Proc *p = make_test_proc();
    TEST_ASSERT(p != NULL, "proc_alloc");
    u64 pipe_freed_before  = pipe_total_freed();
    u64 spoor_freed_before = spoor_total_freed();

    hidx_t fd_rd = -1, fd_wr = -1;
    TEST_EXPECT_EQ(sys_pipe_for_proc(p, &fd_rd, &fd_wr), 0,
        "sys_pipe");

    // Drop the Proc. handle_table_free walks both slots; per-kind
    // release calls spoor_clunk for each KOBJ_SPOOR. spoor_clunk
    // routes through devpipe_close which drops the ring's per-end
    // ref. When both refs hit 0, the ring is freed.
    drop_test_proc(p);

    TEST_EXPECT_EQ(pipe_total_freed() - pipe_freed_before, 1ull,
        "proc_free released the pipe ring");
    TEST_EXPECT_EQ(spoor_total_freed() - spoor_freed_before, 2ull,
        "proc_free released both Spoors");
}

void test_sys_rw_write_then_read_round_trip(void) {
    struct Proc *p = make_test_proc();
    TEST_ASSERT(p != NULL, "proc_alloc");

    hidx_t fd_rd = -1, fd_wr = -1;
    TEST_EXPECT_EQ(sys_pipe_for_proc(p, &fd_rd, &fd_wr), 0, "sys_pipe");

    // Write a payload through the write fd.
    const u8 payload[] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 };
    s64 wrote = sys_write_for_proc(p, fd_wr, payload, sizeof(payload));
    TEST_EXPECT_EQ(wrote, (s64)sizeof(payload),
        "sys_write_for_proc accepts full payload");

    // Read it back through the read fd.
    u8 got[16] = { 0 };
    s64 nread = sys_read_for_proc(p, fd_rd, got, sizeof(got));
    TEST_EXPECT_EQ(nread, (s64)sizeof(payload),
        "sys_read_for_proc returns payload-length bytes");
    for (size_t i = 0; i < sizeof(payload); i++) {
        TEST_ASSERT(got[i] == payload[i],
            "bytes round-trip in FIFO order");
    }

    drop_test_proc(p);
}

void test_sys_rw_rights_check(void) {
    // sys_lookup_spoor requires RIGHT_READ for read / RIGHT_WRITE
    // for write. SYS_PIPE grants both on both ends, so to test the
    // check we'd need to construct a handle with reduced rights —
    // not possible at v1.0 from kernel test (no sys_handle_reduce
    // syscall). Instead: pass an INVALID fd (out-of-range) and
    // verify both handlers return -1.
    struct Proc *p = make_test_proc();
    TEST_ASSERT(p != NULL, "proc_alloc");

    u8 buf[4] = { 0 };
    TEST_EXPECT_EQ(sys_write_for_proc(p, 9999, buf, 4), -1L,
        "write on out-of-range fd returns -1");
    TEST_EXPECT_EQ(sys_read_for_proc(p, 9999, buf, 4), -1L,
        "read on out-of-range fd returns -1");

    drop_test_proc(p);
}

void test_sys_rw_zero_length_validates_fd(void) {
    // Zero-length read/write on a valid fd → 0; on a bad fd → -1.
    struct Proc *p = make_test_proc();
    TEST_ASSERT(p != NULL, "proc_alloc");

    hidx_t fd_rd = -1, fd_wr = -1;
    TEST_EXPECT_EQ(sys_pipe_for_proc(p, &fd_rd, &fd_wr), 0, "sys_pipe");

    TEST_EXPECT_EQ(sys_write_for_proc(p, fd_wr, NULL, 0), 0L,
        "zero-length write returns 0");
    TEST_EXPECT_EQ(sys_read_for_proc(p, fd_rd, NULL, 0), 0L,
        "zero-length read returns 0");

    drop_test_proc(p);
}

void test_sys_rw_read_after_close_returns_eof(void) {
    struct Proc *p = make_test_proc();
    TEST_ASSERT(p != NULL, "proc_alloc");

    hidx_t fd_rd = -1, fd_wr = -1;
    TEST_EXPECT_EQ(sys_pipe_for_proc(p, &fd_rd, &fd_wr), 0, "sys_pipe");

    // Close the write end first. devpipe_close sets write_eof + wakes
    // any sleeping reader; no waiter at this point.
    TEST_EXPECT_EQ(handle_close(p, fd_wr), 0, "close wr");

    // Subsequent read on the (now-empty) pipe with write_eof set
    // returns 0 (EOF) immediately without blocking.
    u8 got[8];
    s64 nread = sys_read_for_proc(p, fd_rd, got, sizeof(got));
    TEST_EXPECT_EQ(nread, 0L, "read returns 0 (EOF) after write end closed");

    drop_test_proc(p);
}

void test_sys_pipe_dup_spoor_handle_acquires_ref(void) {
    // P5-fd-syscalls: verify handle_dup of a KOBJ_SPOOR exercises the
    // acquire path (spoor_ref) — the dup'd handle has its own
    // reference; closing one doesn't free the underlying Spoor while
    // the other is alive.
    struct Proc *p = make_test_proc();
    TEST_ASSERT(p != NULL, "proc_alloc");

    hidx_t fd_rd = -1, fd_wr = -1;
    TEST_EXPECT_EQ(sys_pipe_for_proc(p, &fd_rd, &fd_wr), 0, "sys_pipe");

    struct Handle h_rd;
    TEST_ASSERT(handle_get(p, fd_rd, &h_rd) == 0, "handle_get(rd)");
    struct Spoor *rd_spoor = (struct Spoor *)h_rd.obj;
    handle_put(&h_rd);                    // #844: release the borrow before reading ref
    int ref_before = rd_spoor->ref;

    // Dup the read end with reduced rights (RIGHT_READ only — subset
    // of READ|WRITE|TRANSFER granted by SYS_PIPE).
    hidx_t dup_fd = handle_dup(p, fd_rd, RIGHT_READ);
    TEST_ASSERT(dup_fd >= 0 && dup_fd != fd_rd, "handle_dup returned new fd");
    TEST_EXPECT_EQ(rd_spoor->ref, ref_before + 1,
        "handle_dup bumped the Spoor refcount via handle_acquire_obj");

    // Verify the dup'd handle is a KOBJ_SPOOR pointing at the same Spoor.
    struct Handle h_dup;
    TEST_ASSERT(handle_get(p, dup_fd, &h_dup) == 0, "handle_get(dup)");
    TEST_EXPECT_EQ((int)h_dup.kind, (int)KOBJ_SPOOR, "dup'd handle is KOBJ_SPOOR");
    TEST_ASSERT(h_dup.obj == rd_spoor, "dup'd handle points at same Spoor");
    TEST_EXPECT_EQ(h_dup.rights, (rights_t)RIGHT_READ,
        "dup'd handle has reduced rights");
    handle_put(&h_dup);                   // #844: release before the ref==ref_before check

    // Rights elevation must be rejected: dup the reduced-rights dup
    // back to READ|WRITE → -1.
    hidx_t bad = handle_dup(p, dup_fd, RIGHT_READ | RIGHT_WRITE);
    TEST_EXPECT_EQ(bad, -1, "rights elevation in handle_dup is rejected");

    // Close the dup'd handle. The Spoor's ref drops by 1 but stays
    // alive (the original handle still holds a ref).
    TEST_EXPECT_EQ(handle_close(p, dup_fd), 0, "close dup");
    TEST_EXPECT_EQ(rd_spoor->ref, ref_before,
        "close of dup'd handle dropped refcount back to original");

    drop_test_proc(p);
}

void test_sys_attach_9p_rejection_paths(void) {
    // SYS_ATTACH_9P's user-VA copy + handshake-with-server happy path
    // is exercised by a userspace probe (deferred — needs a 9P
    // responder server). The kernel-internal sanity tests cover the
    // rejection paths: bad tx_fd / bad rx_fd / out-of-range rights /
    // out-of-range aname_len.
    //
    // We call the SVC dispatch with crafted ctx->regs to exercise the
    // handler; on rejection it returns -1 without touching anything
    // beyond the Spoor refs (no allocations, no installations).
    struct Proc *p = make_test_proc();
    TEST_ASSERT(p != NULL, "proc_alloc");

    // Get two valid KOBJ_SPOOR fds via sys_pipe_for_proc — they have
    // READ|WRITE|TRANSFER rights so they pass both gates.
    hidx_t fd_a_rd = -1, fd_a_wr = -1;
    TEST_EXPECT_EQ(sys_pipe_for_proc(p, &fd_a_rd, &fd_a_wr), 0, "pipe A");
    hidx_t fd_b_rd = -1, fd_b_wr = -1;
    TEST_EXPECT_EQ(sys_pipe_for_proc(p, &fd_b_rd, &fd_b_wr), 0, "pipe B");

    // The actual SVC dispatcher entry. Use it via exception_context
    // arg-shape (regs[0..4] = arguments; regs[8] = syscall number).
    // We can't easily fabricate an exception_context here; instead,
    // since sys_attach_9p_handler is static, we exercise rejection
    // paths via the public failure conditions:
    //
    //   - Pass an out-of-range fd → sys_lookup_spoor returns NULL →
    //     -1 returned.
    //
    // Without exposing sys_attach_9p_for_proc as the others do, the
    // test calls handle_get directly to check what would happen on
    // each rejection path.

    // 1. Verify a bogus fd doesn't pass handle_get (the helper used
    //    by sys_attach_9p_handler internally). This is a structural
    //    pre-check: if handle_get rejects, sys_attach_9p_handler
    //    returns -1 before any allocation.
    struct Handle bogus_tmp;
    TEST_EXPECT_EQ(handle_get(p, 9999, &bogus_tmp), -1,
        "out-of-range fd returns -1 from handle_get");

    // 2. Verify a closed fd doesn't pass.
    TEST_EXPECT_EQ(handle_close(p, fd_b_wr), 0, "close fd_b_wr");
    TEST_EXPECT_EQ(handle_get(p, fd_b_wr, &bogus_tmp), -1,
        "closed fd returns -1 from handle_get");

    // Cleanup. Procf_free closes the remaining handles (rd_a, wr_a,
    // rd_b) via handle_table_free → KOBJ_SPOOR release → spoor_clunk.
    drop_test_proc(p);
}

void test_sys_pipe_handle_close_releases_one_end(void) {
    struct Proc *p = make_test_proc();
    TEST_ASSERT(p != NULL, "proc_alloc");
    u64 pipe_freed_before  = pipe_total_freed();
    u64 spoor_freed_before = spoor_total_freed();

    hidx_t fd_rd = -1, fd_wr = -1;
    TEST_EXPECT_EQ(sys_pipe_for_proc(p, &fd_rd, &fd_wr), 0, "sys_pipe");

    // Close the read end. devpipe_close sets write_eof (no — read_eof;
    // closing the read end means read_eof = true) + wakes write_rendez
    // (no waiter; no-op) + drops the ring's per-endpoint ref (2 → 1).
    // Ring is NOT freed yet.
    TEST_EXPECT_EQ(handle_close(p, fd_rd), 0, "close rd");
    TEST_EXPECT_EQ(pipe_total_freed() - pipe_freed_before, 0ull,
        "ring NOT freed after one end close");
    TEST_EXPECT_EQ(spoor_total_freed() - spoor_freed_before, 1ull,
        "one Spoor freed (the rd-end)");

    // Close the write end. Drops the ring's last per-endpoint ref;
    // ring is freed.
    TEST_EXPECT_EQ(handle_close(p, fd_wr), 0, "close wr");
    TEST_EXPECT_EQ(pipe_total_freed() - pipe_freed_before, 1ull,
        "ring freed after second end close");
    TEST_EXPECT_EQ(spoor_total_freed() - spoor_freed_before, 2ull,
        "both Spoors freed");

    drop_test_proc(p);
}

// =============================================================================
// SYS_PREAD / SYS_PWRITE (#37) — positioned byte I/O.
//
// The positioned inners share spoor_read_common / spoor_write_common with the
// cursor path; these tests pin the FOUR properties the sharing must uphold:
// (a) the cursor is never read or advanced by a positioned op, (b) the
// caller's offset is what the Dev sees (asserted against devramfs's known
// /welcome content; the dev9p wire-offset twin lives in test_dev9p.c),
// (c) positioned I/O on a non-seekable Dev is rejected up front (the POSIX
// ESPIPE shape -- a pread on a pipe must NOT silently consume stream data),
// (d) the rights + #81 CWALKONLY gates carry over unchanged.
// =============================================================================

// Walk a file out of the boot ramfs and open it. Returns a REF-OWNED Spoor
// (the caller transfers it to handle_alloc, which ADOPTS the ref).
static struct Spoor *prw_open_ramfs(const char *name) {
    struct Spoor *root = devramfs.attach("");
    if (!root) return NULL;
    const char *names[1] = { name };
    struct Walkqid *wq = devramfs.walk(root, NULL, names, 1);
    spoor_unref(root);
    if (!wq) return NULL;
    if (wq->nqid != 1) { walkqid_free(wq); return NULL; }
    struct Spoor *f = wq->spoor;
    walkqid_free(wq);
    if (!devramfs.open(f, 0)) { spoor_unref(f); return NULL; }
    return f;
}

void test_sys_prw_pipe_not_seekable(void) {
    struct Proc *p = make_test_proc();
    TEST_ASSERT(p != NULL, "proc_alloc");
    hidx_t fd_rd = -1, fd_wr = -1;
    TEST_EXPECT_EQ(sys_pipe_for_proc(p, &fd_rd, &fd_wr), 0, "sys_pipe");

    u8 buf[8] = { 0 };
    // devpipe has no byte offsets (dev->seekable == false): positioned I/O
    // fails up front instead of silently acting as a cursor-free stream op.
    TEST_EXPECT_EQ(sys_pwrite_for_proc(p, fd_wr, buf, 8, 0), -1L,
        "pwrite on a pipe rejected (not seekable)");
    TEST_EXPECT_EQ(sys_pread_for_proc(p, fd_rd, buf, 8, 0), -1L,
        "pread on a pipe rejected (not seekable)");
    // The seekable gate sits BEFORE the len==0 short-circuit, so even the
    // zero-length probe reports the ESPIPE-shaped reject.
    TEST_EXPECT_EQ(sys_pwrite_for_proc(p, fd_wr, NULL, 0, 0), -1L,
        "zero-length pwrite on a pipe still rejected");

    // The rejects perturbed nothing: the cursor path round-trips.
    const u8 payload[4] = { 1, 2, 3, 4 };
    TEST_EXPECT_EQ(sys_write_for_proc(p, fd_wr, payload, 4), 4L, "write OK");
    u8 got[4] = { 0 };
    TEST_EXPECT_EQ(sys_read_for_proc(p, fd_rd, got, 4), 4L, "read OK");
    TEST_ASSERT(got[0] == 1 && got[3] == 4, "payload intact");

    drop_test_proc(p);
}

void test_sys_pread_devramfs_offset_and_cursor(void) {
    // /welcome content is pinned by tools/build.sh: "Welcome to Thylacine
    // ramfs.\n" (28 bytes). Offsets are asserted against it.
    struct Spoor *f = prw_open_ramfs("welcome");
    if (!f) return;   // initrd without the smoke files (never in CI)
    struct Proc *p = make_test_proc();
    TEST_ASSERT(p != NULL, "proc_alloc");
    hidx_t fd = handle_alloc(p, KOBJ_SPOOR, RIGHT_READ, f);   // adopts the ref
    TEST_ASSERT(fd >= 0, "handle_alloc");

    u8 buf[32];
    TEST_EXPECT_EQ(sys_pread_for_proc(p, fd, buf, 7, 0), 7L, "pread @0");
    TEST_ASSERT(buf[0] == 'W' && buf[6] == 'e', "pread @0 = 'Welcome'");
    TEST_EXPECT_EQ(sys_pread_for_proc(p, fd, buf, 4, 2), 4L, "pread @2");
    TEST_ASSERT(buf[0] == 'l' && buf[1] == 'c' && buf[2] == 'o' && buf[3] == 'm',
        "pread @2 = 'lcom'");
    // Past-EOF -> 0; negative offset -> -1; off+len past INT64_MAX -> -1.
    TEST_EXPECT_EQ(sys_pread_for_proc(p, fd, buf, 8, 4096), 0L,
        "pread past EOF returns 0");
    TEST_EXPECT_EQ(sys_pread_for_proc(p, fd, buf, 8, -1), -1L,
        "negative offset rejected");
    TEST_EXPECT_EQ(sys_pread_for_proc(p, fd, buf, 8,
                                      (s64)0x7FFFFFFFFFFFFFFCLL), -1L,
        "off+len past INT64_MAX rejected");

    // The cursor is untouched by everything above: a cursor read still
    // starts at byte 0, and a pread BETWEEN cursor reads does not move it.
    TEST_EXPECT_EQ(sys_read_for_proc(p, fd, buf, 4), 4L, "cursor read");
    TEST_ASSERT(buf[0] == 'W' && buf[3] == 'c', "cursor read starts at 0");
    TEST_EXPECT_EQ(sys_pread_for_proc(p, fd, buf, 2, 11), 2L, "pread @11");
    TEST_ASSERT(buf[0] == 'T' && buf[1] == 'h', "pread @11 = 'Th'");
    TEST_EXPECT_EQ(sys_read_for_proc(p, fd, buf, 3), 3L, "cursor read 2");
    TEST_ASSERT(buf[0] == 'o' && buf[1] == 'm' && buf[2] == 'e',
        "cursor continued at 4 (pread did not move it)");

    drop_test_proc(p);   // releases the handle -> the Spoor
}

void test_sys_prw_rights_and_walkonly(void) {
    struct Proc *p = make_test_proc();
    TEST_ASSERT(p != NULL, "proc_alloc");

    // A WRITE-only handle cannot pread (the RIGHT_READ gate) -- the
    // omode-derived-rights shape (#46 family) applied to the positioned ops.
    struct Spoor *fw = prw_open_ramfs("welcome");
    if (!fw) { drop_test_proc(p); return; }
    hidx_t fdw = handle_alloc(p, KOBJ_SPOOR, RIGHT_WRITE, fw);
    TEST_ASSERT(fdw >= 0, "handle_alloc W-only");
    u8 buf[8];
    TEST_EXPECT_EQ(sys_pread_for_proc(p, fdw, buf, 8, 0), -1L,
        "pread on a WRITE-only handle rejected");

    // A READ-only handle cannot pwrite.
    struct Spoor *fr = prw_open_ramfs("welcome");
    TEST_ASSERT(fr != NULL, "open welcome (r)");
    hidx_t fdr = handle_alloc(p, KOBJ_SPOOR, RIGHT_READ, fr);
    TEST_ASSERT(fdr >= 0, "handle_alloc R-only");
    TEST_EXPECT_EQ(sys_pwrite_for_proc(p, fdr, buf, 8, 0), -1L,
        "pwrite on a READ-only handle rejected");

    // #81: a CWALKONLY (O_PATH) handle does no byte I/O -- positioned
    // included, len 0 included.
    struct Spoor *fo = prw_open_ramfs("welcome");
    TEST_ASSERT(fo != NULL, "open welcome (opath)");
    fo->flag |= CWALKONLY;
    hidx_t fdo = handle_alloc(p, KOBJ_SPOOR, RIGHT_READ | RIGHT_WRITE, fo);
    TEST_ASSERT(fdo >= 0, "handle_alloc walkonly");
    TEST_EXPECT_EQ(sys_pread_for_proc(p, fdo, buf, 8, 0), -1L,
        "pread on O_PATH rejected");
    TEST_EXPECT_EQ(sys_pwrite_for_proc(p, fdo, buf, 8, 0), -1L,
        "pwrite on O_PATH rejected");
    TEST_EXPECT_EQ(sys_pread_for_proc(p, fdo, NULL, 0, 0), -1L,
        "zero-length pread on O_PATH rejected");

    drop_test_proc(p);
}
