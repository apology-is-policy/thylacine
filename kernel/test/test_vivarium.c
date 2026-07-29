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
//   vivarium.rejects_are_deliberate the FORWARDs, the T2s, and brk's ENOSYS, by name
//   vivarium.unknown_forwards       an unclassified number forwards, not ENOSYS
//   vivarium.fails_closed           NULL args/out -> ENOSYS, `out` untouched
//   vivarium.no_wide_alias          a >16-bit number cannot alias a real row
//   vivarium.openat_domain          the admitted flag set vs the rejects, by name
//   vivarium.openat_at_fdcwd        both AT_FDCWD encodings; a real dirfd forwards
//   vivarium.openat_build           SYS_OPEN's argument order
//   vivarium.stat_to_linux          88B -> 128B, incl. the I-13 no-leak property
//   vivarium.fstatat_domain         the AT_* domain; AT_FDCWD-only is structural
//
// The T2 tests (V-2b/V-2c) stay just as pure: neither decide function reads user
// memory by construction, and the stat conversion is data-in/data-out.

#include "test.h"

#include <thylacine/syscall.h>
#include <thylacine/types.h>
#include <thylacine/vivarium.h>

void test_vivarium_t1_renumbers(void);
void test_vivarium_rejects_are_deliberate(void);
void test_vivarium_unknown_forwards(void);
void test_vivarium_fails_closed(void);
void test_vivarium_no_wide_alias(void);
void test_vivarium_openat_domain(void);
void test_vivarium_openat_at_fdcwd(void);
void test_vivarium_openat_build(void);
void test_vivarium_stat_to_linux(void);
void test_vivarium_fstatat_domain(void);

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

    // openat + fstat are TIER-2 since V-2b: admissible, but not by renumber, so
    // vivarium_translate classifies them and leaves the work to the named
    // translator. `out` must stay untouched -- a T2 verdict is not a dispatch.
    out.nr = 0xDEADu;
    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_OPENAT, args, &out),
                   (int)VIV_TIER2, "openat is T2 (path len + O_* + AT_FDCWD)");
    TEST_EXPECT_EQ((u64)out.nr, (u64)0xDEADu, "a T2 verdict leaves out untouched");
    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_FSTAT, args, &out),
                   (int)VIV_TIER2, "fstat is T2 (88B t_stat -> 128B stat)");
    TEST_EXPECT_EQ((u64)out.nr, (u64)0xDEADu, "a T2 verdict leaves out untouched");
    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_NEWFSTATAT, args, &out),
                   (int)VIV_TIER2, "newfstatat is T2 (V-2c; SYS_STAT + the conversion)");
    TEST_EXPECT_EQ((u64)out.nr, (u64)0xDEADu, "a T2 verdict leaves out untouched");

    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_MMAP, args, &out),
                   (int)VIV_FORWARD, "mmap forwards (addr/prot/flags are policy)");

    // statx is the reason newfstatat is not the whole stat story: musl-aarch64
    // defines no __NR_fstatat, so its fstatat.c compiles the 79 path out and
    // issues 291 instead. Go and glibc do use 79. Pinned as a deliberate
    // FORWARD so promoting it later is a decision, not a drive-by.
    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_STATX, args, &out),
                   (int)VIV_FORWARD, "statx forwards (a mask + a 256B struct, not this shape)");

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

// -----------------------------------------------------------------------------
// TIER 2 (V-2b)
// -----------------------------------------------------------------------------

// AT_FDCWD sign-extended, the way a compiler that widens `int` -1 usually leaves
// x0. The bare-u32 form is exercised separately -- both must be recognised.
#define VIV_T_ATCWD ((u64)(s64)VIV_AT_FDCWD)

static void viv_expect_open(u64 dirfd, u64 flags, u32 want_omode,
                            const char *what) {
    u64 start_fd = 0xBADu;
    u32 omode    = 0xBADu;

    TEST_EXPECT_EQ((int)vivarium_openat_decide(dirfd, flags, &start_fd, &omode),
                   (int)VIV_TRANSLATED, what);
    TEST_EXPECT_EQ(start_fd, SYS_WALK_OPEN_FROM_ROOT, "AT_FDCWD -> FROM_ROOT");
    TEST_EXPECT_EQ((u64)omode, (u64)want_omode, what);

    // Whatever the map produces must be an omode SYS_OPEN will actually accept.
    // Asserting this for EVERY translated case (rather than eyeballing the
    // constants) is what makes a future flag admission safe to add.
    TEST_EXPECT_EQ((u64)(omode & ~SYS_WALK_OPEN_OMODE_VALID), (u64)0,
                   "the emitted omode is inside SYS_WALK_OPEN_OMODE_VALID");
}

static void viv_expect_open_forwards(u64 flags, const char *what) {
    u64 start_fd = 0xBADu;
    u32 omode    = 0xBADu;

    TEST_EXPECT_EQ((int)vivarium_openat_decide(VIV_T_ATCWD, flags, &start_fd, &omode),
                   (int)VIV_FORWARD, what);
    // A declined call must leave the outputs alone: a caller that forwards but
    // reads them anyway must not find a plausible-looking omode waiting.
    TEST_EXPECT_EQ(start_fd, (u64)0xBADu, "a forwarded openat leaves start_fd alone");
    TEST_EXPECT_EQ((u64)omode, (u64)0xBADu, "a forwarded openat leaves omode alone");
}

void test_vivarium_openat_domain(void) {
    // The three access modes, and O_TRUNC composing with each.
    viv_expect_open(VIV_T_ATCWD, VIV_O_RDONLY, 0u, "O_RDONLY -> OREAD");
    viv_expect_open(VIV_T_ATCWD, VIV_O_WRONLY, 1u, "O_WRONLY -> OWRITE");
    viv_expect_open(VIV_T_ATCWD, VIV_O_RDWR,   2u, "O_RDWR -> ORDWR");
    viv_expect_open(VIV_T_ATCWD, VIV_O_WRONLY | VIV_O_TRUNC, 1u | 0x10u,
                    "O_WRONLY|O_TRUNC -> OWRITE|OTRUNC");

    // The three no-op admissions. Each is admitted because Thylacine already
    // provides what the flag asks for unconditionally (see vivarium.c), so the
    // resulting omode must be IDENTICAL to the flag's absence.
    viv_expect_open(VIV_T_ATCWD, VIV_O_RDONLY | VIV_O_CLOEXEC, 0u,
                    "O_CLOEXEC is a no-op (no fd crosses spawn implicitly)");
    viv_expect_open(VIV_T_ATCWD, VIV_O_RDWR | VIV_O_NOCTTY, 2u,
                    "O_NOCTTY is a no-op (ct acquisition is explicit)");
    viv_expect_open(VIV_T_ATCWD, VIV_O_RDONLY | VIV_O_LARGEFILE, 0u,
                    "O_LARGEFILE is a no-op (all offsets are 64-bit)");

    // O_PATH dominates: the access bits and O_TRUNC are ignored on BOTH sides,
    // so the emitted omode is the bare OPATH rather than OPATH|whatever.
    viv_expect_open(VIV_T_ATCWD, VIV_O_PATH, SYS_WALK_OPEN_OPATH,
                    "O_PATH -> OPATH");
    viv_expect_open(VIV_T_ATCWD, VIV_O_PATH | VIV_O_RDWR | VIV_O_TRUNC,
                    SYS_WALK_OPEN_OPATH,
                    "O_PATH ignores access bits + O_TRUNC, as Linux does");

    // The rejects. Each of these, if silently ignored, is a WRONG ANSWER rather
    // than a harmless no-op -- that asymmetry is the whole admission rule, so
    // each is pinned by name.
    viv_expect_open_forwards(VIV_O_WRONLY | VIV_O_CREAT,
                             "O_CREAT forwards (SYS_OPEN cannot create)");
    viv_expect_open_forwards(VIV_O_RDONLY | VIV_O_DIRECTORY,
                             "O_DIRECTORY forwards (no is-a-dir check to honour)");
    viv_expect_open_forwards(VIV_O_WRONLY | VIV_O_APPEND,
                             "O_APPEND forwards (no append mode in omode)");
    viv_expect_open_forwards(VIV_O_RDONLY | VIV_O_NOFOLLOW,
                             "O_NOFOLLOW forwards (correct only while symlinks are absent)");
    viv_expect_open_forwards(VIV_O_RDONLY | VIV_O_NONBLOCK,
                             "O_NONBLOCK forwards");
    viv_expect_open_forwards(VIV_O_WRONLY | VIV_O_EXCL, "O_EXCL forwards");

    // (flags & O_ACCMODE) == 3 is EINVAL on Linux. We forward rather than mint
    // the error ourselves -- section 4 forbids the table inventing error semantics.
    viv_expect_open_forwards(VIV_O_ACCMODE,
                             "accmode 3 forwards (Linux EINVAL; not ours to mint)");

    // Fail toward the supervisor on a bad call site, never toward a dispatch.
    TEST_EXPECT_EQ((int)vivarium_openat_decide(VIV_T_ATCWD, 0, NULL, NULL),
                   (int)VIV_FORWARD, "NULL outputs -> FORWARD, never TRANSLATED");
}

void test_vivarium_openat_at_fdcwd(void) {
    // AT_FDCWD is `int` -100. Depending on how the caller widened it, x0 holds
    // either the sign-extended or the merely zero-extended value. BOTH mean
    // AT_FDCWD; recognising only one would work on some toolchains and silently
    // forward every open() on others.
    viv_expect_open((u64)(s64)-100, VIV_O_RDONLY, 0u,
                    "AT_FDCWD sign-extended is recognised");
    viv_expect_open((u64)0xFFFFFF9Cu, VIV_O_RDONLY, 0u,
                    "AT_FDCWD zero-extended is recognised");

    // A real dirfd is out of domain, permanently rather than pending: a Linux
    // dirfd is a NORMALLY-OPENED handle, and 9P forbids Twalk from an opened fid
    // (syscall.h:2370), so it is not a usable SYS_OPEN start_fd -- and telling it
    // apart from an O_PATH one means reading handle state this function may not
    // touch. (V-2b filed this as "revisit once the path is measured"; V-2c found
    // that would not have helped.)
    u64 start_fd = 0xBADu;
    u32 omode    = 0xBADu;
    TEST_EXPECT_EQ((int)vivarium_openat_decide(3, VIV_O_RDONLY, &start_fd, &omode),
                   (int)VIV_FORWARD, "a real dirfd forwards (handle state, not a gap)");

    // Only the LOW 32 BITS are significant -- `dirfd` is an `int`. A high-half
    // value that is not AT_FDCWD in its low word must not be mistaken for one.
    TEST_EXPECT_EQ((int)vivarium_openat_decide(0x1234567800000003ull, VIV_O_RDONLY,
                                               &start_fd, &omode),
                   (int)VIV_FORWARD, "the high half of dirfd is not consulted");
}

void test_vivarium_openat_build(void) {
    struct viv_call out;

    out.nr = 0xDEADu;
    for (u32 i = 0; i < VIV_NARGS; i++) out.args[i] = 0xBADu;

    vivarium_openat_build(SYS_WALK_OPEN_FROM_ROOT, 0x4000, 11, 2u, &out);

    // SYS_OPEN's argument order (syscall.h:1337-1342) stated in exactly one
    // place, and checked here so a reshuffle cannot pass silently.
    TEST_EXPECT_EQ(out.nr,      (u64)SYS_OPEN,             "build -> SYS_OPEN");
    TEST_EXPECT_EQ(out.args[0], SYS_WALK_OPEN_FROM_ROOT,   "arg0 = start_fd");
    TEST_EXPECT_EQ(out.args[1], (u64)0x4000,               "arg1 = path_va");
    TEST_EXPECT_EQ(out.args[2], (u64)11,                   "arg2 = path_len");
    TEST_EXPECT_EQ(out.args[3], (u64)2,                    "arg3 = omode");

    // The unused words are ZEROED, not left as the caller's poison: SYS_OPEN
    // ignores them today, but handing a dispatcher a vector with stale words in
    // it is how a later 5-argument reader picks up garbage.
    TEST_EXPECT_EQ(out.args[4], (u64)0, "unused arg4 zeroed, not left poisoned");
    TEST_EXPECT_EQ(out.args[5], (u64)0, "unused arg5 zeroed, not left poisoned");
}

void test_vivarium_stat_to_linux(void) {
    struct t_stat in;
    struct viv_linux_stat out;

    // The kernel links no memset, so byte-zero by hand (dev9p.c:701's idiom).
    for (u64 i = 0; i < (u64)sizeof(in); i++)  ((u8 *)&in)[i]  = 0;
    // Poison the OUTPUT so "zeroed by the conversion" cannot be confused with
    // "happened to already be zero" -- that is the I-13 leak check below.
    for (u64 i = 0; i < (u64)sizeof(out); i++) ((u8 *)&out)[i] = 0xA5;

    // Distinct, recognisable values so a swapped pair is visible.
    in.size      = 0x1111;
    in.qid_path  = 0x2222;
    in.atime_sec = 0x3333;
    in.mtime_sec = 0x4444;
    in.ctime_sec = 0x5555;
    in.mode      = 0100644u;
    in.nlink     = 7;
    in.blksize   = 4096;
    in.blocks    = 9;
    in.uid       = 1001;
    in.gid       = 1002;
    in.devno     = 42;

    vivarium_stat_to_linux(&in, &out);

    // (devno, qid.path) IS Thylacine's file identity (#100) and is already the
    // pair userspace maps onto (st_dev, st_ino) -- pouch patch 0010 does exactly
    // this. The correspondence is inherited, not invented here.
    TEST_EXPECT_EQ(out.st_dev, (u64)42,     "st_dev <- t_stat.devno (#100)");
    TEST_EXPECT_EQ(out.st_ino, (u64)0x2222, "st_ino <- t_stat.qid_path");

    TEST_EXPECT_EQ((u64)out.st_mode,  (u64)0100644u, "st_mode carries");
    TEST_EXPECT_EQ((u64)out.st_nlink, (u64)7,        "st_nlink carries");
    TEST_EXPECT_EQ((u64)out.st_uid,   (u64)1001,     "st_uid carries");
    TEST_EXPECT_EQ((u64)out.st_gid,   (u64)1002,     "st_gid carries");

    TEST_EXPECT_EQ((u64)out.st_size,    (u64)0x1111, "st_size carries");
    TEST_EXPECT_EQ((u64)out.st_blksize, (u64)4096,   "st_blksize carries");
    TEST_EXPECT_EQ((u64)out.st_blocks,  (u64)9,      "st_blocks carries");

    TEST_EXPECT_EQ((u64)out.st_atime_sec, (u64)0x3333, "st_atim.tv_sec carries");
    TEST_EXPECT_EQ((u64)out.st_mtime_sec, (u64)0x4444, "st_mtim.tv_sec carries");
    TEST_EXPECT_EQ((u64)out.st_ctime_sec, (u64)0x5555, "st_ctim.tv_sec carries");

    // t_stat has no sub-second resolution, so the nsec words are an honest zero
    // -- and, being zero over POISON, they prove the conversion writes them.
    TEST_EXPECT_EQ(out.st_atime_nsec, (u64)0, "atim.tv_nsec zeroed (no sub-second source)");
    TEST_EXPECT_EQ(out.st_mtime_nsec, (u64)0, "mtim.tv_nsec zeroed");
    TEST_EXPECT_EQ(out.st_ctime_nsec, (u64)0, "ctim.tv_nsec zeroed");

    // I-13: this buffer is copied to a guest, so EVERY reserved word must be
    // written. Over 0xA5 poison, a surviving byte is a kernel-stack leak.
    TEST_EXPECT_EQ(out.st_rdev,          (u64)0, "st_rdev zeroed (no dev_t to fabricate)");
    TEST_EXPECT_EQ(out.__pad1,           (u64)0, "__pad1 zeroed -- no kernel stack leaks");
    TEST_EXPECT_EQ((u64)out.__pad2,      (u64)0, "__pad2 zeroed");
    TEST_EXPECT_EQ((u64)out.__unused4,   (u64)0, "__unused4 zeroed");
    TEST_EXPECT_EQ((u64)out.__unused5,   (u64)0, "__unused5 zeroed");

    // Belt-and-braces on the same property: not one byte of the 128 may still
    // hold poison. This catches a field ADDED later and left unassigned, which
    // the per-field asserts above would not.
    int poisoned = 0;
    for (u64 i = 0; i < (u64)sizeof(out); i++)
        if (((const u8 *)&out)[i] == 0xA5) poisoned++;
    TEST_EXPECT_EQ((u64)poisoned, (u64)0, "no byte of the 128 survives unwritten");

    // Fail closed on a bad call site: NULL must be a no-op, not a fault.
    vivarium_stat_to_linux(NULL, &out);
    vivarium_stat_to_linux(&in, NULL);
    TEST_EXPECT_EQ(out.st_ino, (u64)0x2222, "NULL in/out is a no-op, not a scribble");
}

// -----------------------------------------------------------------------------
// TIER 2 (V-2c) -- newfstatat
// -----------------------------------------------------------------------------

static void viv_expect_statat(u64 dirfd, u64 flags, const char *what) {
    TEST_EXPECT_EQ((int)vivarium_fstatat_decide(dirfd, flags),
                   (int)VIV_TRANSLATED, what);
}

static void viv_expect_statat_forwards(u64 flags, const char *what) {
    TEST_EXPECT_EQ((int)vivarium_fstatat_decide(VIV_T_ATCWD, flags),
                   (int)VIV_FORWARD, what);
}

void test_vivarium_fstatat_domain(void) {
    // Plain stat() -- flags 0. This is the row that carries the value: on
    // aarch64, stat() compiles to newfstatat, not to a stat(2) of its own.
    viv_expect_statat(VIV_T_ATCWD, 0, "flags 0 (plain stat) translates");

    // The one no-op admission. A Thylacine namespace is composed explicitly, so
    // nothing mounts as a side effect of traversal -- the flag asks for what we
    // do unconditionally, and by construction of the model rather than by a
    // feature being unbuilt.
    viv_expect_statat(VIV_T_ATCWD, VIV_AT_NO_AUTOMOUNT,
                      "AT_NO_AUTOMOUNT is a no-op (nothing mounts on traversal)");

    // The costly reject, pinned by name because it is the one a future reader
    // will be tempted to admit: SYS_STAT's contract literally says "stat ==
    // lstat" at v1.0. That equivalence holds only because SYMLINKS ARE ABSENT,
    // so admitting it would silently return the target's metadata the day they
    // land -- the O_NOFOLLOW trap, on the stat surface.
    viv_expect_statat_forwards(VIV_AT_SYMLINK_NOFOLLOW,
                               "AT_SYMLINK_NOFOLLOW forwards (lstat; correct only while symlinks are absent)");

    // Serving this would mean synthesising a "." the caller never passed.
    viv_expect_statat_forwards(VIV_AT_EMPTY_PATH,
                               "AT_EMPTY_PATH forwards (SYS_STAT needs a real path)");

    // Not valid on fstatat at all -- Linux answers EINVAL. Forwarded rather than
    // rejected here: minting errors is not the table's job (openat's accmode-3
    // decision, applied consistently).
    viv_expect_statat_forwards(VIV_AT_REMOVEDIR,
                               "AT_REMOVEDIR forwards (unlinkat's flag; Linux EINVAL)");
    viv_expect_statat_forwards(VIV_AT_SYMLINK_FOLLOW,
                               "AT_SYMLINK_FOLLOW forwards (linkat's flag; Linux EINVAL)");

    // An unadmitted bit forwards even in combination with an admitted one --
    // the check is a whole-word mask, not a scan for known flags.
    viv_expect_statat_forwards(VIV_AT_NO_AUTOMOUNT | VIV_AT_SYMLINK_NOFOLLOW,
                               "a rejected bit forwards even beside an admitted one");
    viv_expect_statat_forwards(0x80000000u, "an unknown high bit forwards");

    // Both AT_FDCWD encodings, for the openat sign-extension reason.
    viv_expect_statat((u64)(s64)-100, 0, "AT_FDCWD sign-extended is recognised");
    viv_expect_statat((u64)0xFFFFFF9Cu, 0, "AT_FDCWD zero-extended is recognised");

    // A real dirfd forwards -- and here that is STRUCTURAL, not a v1.0 limit:
    // SYS_STAT takes (path, len, out) and has no base argument at all, so there
    // is nowhere for a dirfd to go. Contrast openat, which at least HAS a
    // start_fd it could carry.
    TEST_EXPECT_EQ((int)vivarium_fstatat_decide(3, 0), (int)VIV_FORWARD,
                   "a real dirfd forwards (SYS_STAT has no base argument)");

    // Only the low 32 bits of dirfd are significant.
    TEST_EXPECT_EQ((int)vivarium_fstatat_decide(0x1234567800000003ull, 0),
                   (int)VIV_FORWARD, "the high half of dirfd is not consulted");
}
