// VIVARIUM V-2 — tests for the Linux syscall translation table.
//
// `vivarium_translate` is PURE, so these need no Proc, no handle table, no user
// memory, no boot state — a synthetic argument vector is the whole fixture. That
// property is the reason the table was built before the syscall-entry branch
// (V-1b): the piece carrying the actual content is provable in isolation, while
// the branch it will eventually plug into is five lines.
//
// Coverage:
//   vivarium.t1_renumbers          every T1 row renumbers + carries args verbatim
//   vivarium.rejects_are_deliberate the four FORWARDs + brk's ENOSYS, by name
//   vivarium.unknown_forwards       an unclassified number forwards, not ENOSYS
//   vivarium.fails_closed           NULL args/out -> ENOSYS, `out` untouched
//   vivarium.no_wide_alias          a >16-bit number cannot alias a real row

#include "test.h"

#include <thylacine/syscall.h>
#include <thylacine/types.h>
#include <thylacine/vivarium.h>

void test_vivarium_t1_renumbers(void);
void test_vivarium_rejects_are_deliberate(void);
void test_vivarium_unknown_forwards(void);
void test_vivarium_fails_closed(void);
void test_vivarium_no_wide_alias(void);

// A recognisable argument vector: every word distinct and non-zero, so a
// dropped, duplicated, or reordered word is visible rather than accidentally
// correct (0 would pass a memset bug; equal values would pass a swap bug).
static void viv_fill_args(u64 *a) {
    for (u32 i = 0; i < VIV_NARGS; i++) a[i] = 0x1000u + i;
}

static void viv_expect_renumber(u64 linux_nr, u64 want_thyla, const char *what) {
    u64 args[VIV_NARGS];
    struct viv_call out;
    viv_fill_args(args);

    // Poison `out` so "translated" cannot be confused with "left alone".
    out.nr = 0xDEADu;
    for (u32 i = 0; i < VIV_NARGS; i++) out.args[i] = 0xBADu;

    TEST_EXPECT_EQ((int)vivarium_translate(linux_nr, args, &out),
                   (int)VIV_TRANSLATED, what);
    TEST_EXPECT_EQ((u64)out.nr, want_thyla, what);

    // The args must carry across VERBATIM. A T1 row's whole claim is that no
    // mapping is needed, so any mutation here is the claim being false.
    for (u32 i = 0; i < VIV_NARGS; i++)
        TEST_EXPECT_EQ(out.args[i], (u64)(0x1000u + i),
                       "T1 carries every argument word unchanged");
}

void test_vivarium_t1_renumbers(void) {
    viv_expect_renumber(VIV_LINUX_READ,       SYS_READ,       "read -> SYS_READ");
    viv_expect_renumber(VIV_LINUX_WRITE,      SYS_WRITE,      "write -> SYS_WRITE");
    viv_expect_renumber(VIV_LINUX_CLOSE,      SYS_CLOSE,      "close -> SYS_CLOSE");
    viv_expect_renumber(VIV_LINUX_LSEEK,      SYS_LSEEK,      "lseek -> SYS_LSEEK");
    viv_expect_renumber(VIV_LINUX_EXIT_GROUP, SYS_EXIT_GROUP, "exit_group -> SYS_EXIT_GROUP");

    // The lseek row's equivalence rests on the two enumerations coinciding. Pin
    // it here rather than only in a comment: if T_SEEK_* ever moves, this fails
    // and the row must drop to T2 (a flag mapping) instead of silently
    // translating a Linux SEEK_END into some other Thylacine whence.
    TEST_EXPECT_EQ((int)T_SEEK_SET, 0, "T_SEEK_SET == Linux SEEK_SET");
    TEST_EXPECT_EQ((int)T_SEEK_CUR, 1, "T_SEEK_CUR == Linux SEEK_CUR");
    TEST_EXPECT_EQ((int)T_SEEK_END, 2, "T_SEEK_END == Linux SEEK_END");
}

// The rejections are DECISIONS, not gaps, so they are asserted by name. If a
// future change promotes one to a table row, this test fails and forces the
// promotion to be deliberate rather than incidental.
void test_vivarium_rejects_are_deliberate(void) {
    u64 args[VIV_NARGS];
    struct viv_call out;
    viv_fill_args(args);

    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_OPENAT, args, &out),
                   (int)VIV_FORWARD, "openat forwards (path len + O_* + AT_FDCWD)");
    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_FSTAT, args, &out),
                   (int)VIV_FORWARD, "fstat forwards (88B t_stat != 128B stat)");
    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_MMAP, args, &out),
                   (int)VIV_FORWARD, "mmap forwards (addr/prot/flags are policy)");

    // The instructive one. munmap's arguments align 1:1 with SYS_BURROW_DETACH,
    // so it looks like a free renumber; it is excluded because burrow_detach
    // refuses a partial detach while Linux permits one, i.e. the translation is
    // not TOTAL. This assert is the guard against someone "fixing" the omission.
    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_MUNMAP, args, &out),
                   (int)VIV_FORWARD,
                   "munmap forwards -- args align but burrow_detach is exact-match only");

    // brk is the one honest ENOSYS: there is no break pointer to move at all, and
    // both musl and glibc fall back to mmap when brk reports unavailable.
    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_BRK, args, &out),
                   (int)VIV_ENOSYS, "brk is ENOSYS -- no counterpart, libc falls back");
}

void test_vivarium_unknown_forwards(void) {
    u64 args[VIV_NARGS];
    struct viv_call out;
    viv_fill_args(args);

    // An unclassified call must FORWARD, not ENOSYS. Claiming a syscall does not
    // exist when we merely have not reached it is a lie the guest cannot tell
    // apart from a real one -- it would make a libc take a permanent fallback
    // path for a call the supervisor could have served.
    TEST_EXPECT_EQ((int)vivarium_translate(178 /* gettid */, args, &out),
                   (int)VIV_FORWARD, "an unclassified number forwards, never ENOSYS");
    TEST_EXPECT_EQ((int)vivarium_translate(0, args, &out),
                   (int)VIV_FORWARD, "Linux 0 (io_setup) is unclassified -> forward");
}

void test_vivarium_fails_closed(void) {
    u64 args[VIV_NARGS];
    struct viv_call out;
    viv_fill_args(args);

    out.nr = 0xDEADu;

    // A caller that ignores the verdict must never receive a dispatchable
    // number. NULL in either direction is ENOSYS, and `out` is left untouched.
    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_READ, NULL, &out),
                   (int)VIV_ENOSYS, "NULL args -> ENOSYS");
    TEST_EXPECT_EQ((u64)out.nr, (u64)0xDEADu, "out untouched on the NULL-args path");

    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_READ, args, NULL),
                   (int)VIV_ENOSYS, "NULL out -> ENOSYS");
}

void test_vivarium_no_wide_alias(void) {
    u64 args[VIV_NARGS];
    struct viv_call out;
    viv_fill_args(args);

    // The rows are u16. A 64-bit number whose low 16 bits match a real row must
    // NOT translate -- otherwise x8 = 0x1_0040 would dispatch SYS_WRITE. The
    // guard runs before any narrowing comparison.
    out.nr = 0xDEADu;
    TEST_EXPECT_EQ((int)vivarium_translate(0x10000u + VIV_LINUX_WRITE, args, &out),
                   (int)VIV_FORWARD, "a >16-bit number cannot alias a table row");
    TEST_EXPECT_EQ((u64)out.nr, (u64)0xDEADu, "out untouched on the wide-number path");

    TEST_EXPECT_EQ((int)vivarium_translate(~(u64)0, args, &out),
                   (int)VIV_FORWARD, "UINT64_MAX forwards, never translates");
}
