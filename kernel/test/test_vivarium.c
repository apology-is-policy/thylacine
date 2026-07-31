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
//   vivarium.mmap_domain            the anon-private domain; PROT_EXEC refused
//
// The T2 tests (V-2b/V-2c/V-2d) stay just as pure: no decide function reads user
// memory by construction, and the stat conversion is data-in/data-out.

#include "test.h"

#include <thylacine/syscall.h>
#include <thylacine/types.h>
#include <thylacine/notes.h>        // V-6b: the canonical note-name literals
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
void test_vivarium_mmap_domain(void);

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

    // mmap + munmap are TIER-2 since V-2d. V-2a rejected both on facts that
    // still hold; what changed is what FORWARD COSTS -- §4.1 defers V-3, so a
    // decline is now ENOSYS rather than "the supervisor serves it", and mmap is
    // on musl's critical path. The stated argument domain admits the shape musl
    // sends and declines the rest.
    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_MMAP, args, &out),
                   (int)VIV_TIER2, "mmap is T2 (V-2d; the anon-private domain)");
    TEST_EXPECT_EQ((u64)out.nr, (u64)0xDEADu, "a T2 verdict leaves out untouched");

    // statx is the reason newfstatat is not the whole stat story: musl-aarch64
    // defines no __NR_fstatat, so its fstatat.c compiles the 79 path out and
    // issues 291 instead. Go and glibc do use 79. Pinned as a deliberate
    // FORWARD so promoting it later is a decision, not a drive-by.
    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_STATX, args, &out),
                   (int)VIV_FORWARD, "statx forwards (a mask + a 256B struct, not this shape)");

    // The instructive one. munmap's arguments align 1:1 with SYS_BURROW_DETACH,
    // so it looks like a free RENUMBER -- and it is still not one: burrow_detach
    // refuses a partial detach while Linux permits one AND succeeds on an
    // unmapped range. It is T2, not T1, and this assert is the guard against
    // someone "simplifying" it into the renumber table.
    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_MUNMAP, args, &out),
                   (int)VIV_TIER2,
                   "munmap is T2, never T1 -- burrow_detach is exact-match only");
    TEST_EXPECT_EQ((u64)out.nr, (u64)0xDEADu, "a T2 verdict leaves out untouched");

    // mprotect is recorded rather than left to the default. It would reach
    // ENOSYS anyway via the fallthrough, but the file's standard is that a
    // number never considered and one considered-and-rejected are different
    // facts. Thylacine has NO prot-mutation syscall (I-12), and musl tolerates
    // exactly this (mallocng/malloc.c:92 checks `errno != ENOSYS`).
    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_MPROTECT, args, &out),
                   (int)VIV_ENOSYS, "mprotect is ENOSYS -- no prot-mutation syscall exists");

    // brk is the one honest ENOSYS: there is no break pointer to move at all, and
    // both musl and glibc fall back to mmap when brk reports unavailable.
    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_BRK, args, &out),
                   (int)VIV_ENOSYS, "brk is ENOSYS -- no counterpart, libc falls back");

    // V-5c. ppoll carries the whole poll family on aarch64 (no plain poll(2)),
    // so this is what musl's poll() becomes.
    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_PPOLL, args, &out),
                   (int)VIV_TIER2, "ppoll is T2 (V-5c; the ready-file fd swap)");
    TEST_EXPECT_EQ((u64)out.nr, (u64)0xDEADu, "a T2 verdict leaves out untouched");

    // pselect6 is the sibling shape -- three fd_sets rather than a pollfd array
    // -- and lands with its own reshape. Pinned as a deliberate FORWARD so
    // promoting it is a decision rather than a drive-by.
    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_PSELECT6, args, &out),
                   (int)VIV_FORWARD, "pselect6 forwards until its fd_set reshape lands");

    // THE COLLISION RE-CHECK, made executable. These are the first two rows
    // BELOW the highest native syscall number, so the "above the ceiling"
    // argument that discharged every earlier row does not apply -- 72 is
    // SYS_GETPID and 73 is SYS_GETUID. The safety argument does not rest on
    // there being no collision; it rests on a PHENO_LINUX Proc being unable to
    // reach a native number at all (every number it issues is translated, and
    // an unclassified one is ENOSYS). This assert pins the collision as a KNOWN
    // fact, so a future reader finds it stated rather than discovering it.
    TEST_ASSERT(VIV_LINUX_PSELECT6 == SYS_GETPID,
                "pselect6 collides with SYS_GETPID -- known, and reachable by neither");
    TEST_ASSERT(VIV_LINUX_PPOLL == SYS_GETUID,
                "ppoll collides with SYS_GETUID -- known, and reachable by neither");
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

// The mmap argument domain (V-2d). Each admitted argument and each decline is
// asserted BY NAME, because the domain is the whole safety argument for this
// row: a widening that admits a flag we cannot honour is precisely the failure
// mode the tier exists to prevent.
void test_vivarium_mmap_domain(void) {
    const u64 ok_flags = (u64)(VIV_MAP_PRIVATE | VIV_MAP_ANONYMOUS);
    const u64 rw       = (u64)(VIV_PROT_READ | VIV_PROT_WRITE);

    // The shape musl actually sends -- mallocng malloc.c:249/310 and
    // pthread_create.c:303, all of them (addr 0, RW, PRIVATE|ANON, fd -1, 0).
    TEST_EXPECT_EQ((int)vivarium_mmap_decide(0, rw, ok_flags, (u64)-1, 0),
                   (int)VIV_TRANSLATED, "anon-private RW is the admitted shape");

    // PROT_NONE is admitted, and it is the DOMINANT anonymous shape in musl --
    // the thread guard page (pthread_create.c:295) and mallocng's meta areas
    // (malloc.c:82). Declining it would mean malloc never initialises. It
    // yields a WRITABLE mapping: a stated fidelity degradation (VIVARIUM.md §9),
    // sanctioned by musl's own ENOSYS-tolerant mprotect.
    TEST_EXPECT_EQ((int)vivarium_mmap_decide(0, (u64)VIV_PROT_NONE, ok_flags,
                                             (u64)-1, 0),
                   (int)VIV_TRANSLATED, "PROT_NONE admitted (guard pages / meta areas)");
    TEST_EXPECT_EQ((int)vivarium_mmap_decide(0, (u64)VIV_PROT_READ, ok_flags,
                                             (u64)-1, 0),
                   (int)VIV_TRANSLATED, "PROT_READ admitted (yields RW -- degraded, §9)");

    // PROT_EXEC is the hard line: an executable anonymous mapping is CAP_JIT /
    // I-42 territory and W^X (I-12) forbids the RW-and-X region the naive
    // translation would produce. This is the single most important decline here.
    TEST_EXPECT_EQ((int)vivarium_mmap_decide(0, rw | (u64)VIV_PROT_EXEC, ok_flags,
                                             (u64)-1, 0),
                   (int)VIV_FORWARD, "PROT_EXEC declines -- I-12 W^X / I-42 CAP_JIT");
    TEST_EXPECT_EQ((int)vivarium_mmap_decide(0, (u64)VIV_PROT_EXEC, ok_flags,
                                             (u64)-1, 0),
                   (int)VIV_FORWARD, "PROT_EXEC alone declines too");

    // The allow-list is two bits, NOT "everything except PROT_EXEC" -- these
    // three would all have slipped through a deny-list.
    TEST_EXPECT_EQ((int)vivarium_mmap_decide(0, rw | (u64)VIV_PROT_BTI, ok_flags,
                                             (u64)-1, 0),
                   (int)VIV_FORWARD, "PROT_BTI declines (aarch64; not honourable)");
    TEST_EXPECT_EQ((int)vivarium_mmap_decide(0, rw | (u64)VIV_PROT_MTE, ok_flags,
                                             (u64)-1, 0),
                   (int)VIV_FORWARD, "PROT_MTE declines (aarch64; real semantics)");
    TEST_EXPECT_EQ((int)vivarium_mmap_decide(0, rw | (u64)VIV_PROT_GROWSDOWN,
                                             ok_flags, (u64)-1, 0),
                   (int)VIV_FORWARD, "PROT_GROWSDOWN declines (mapping growth)");

    // MAP_FIXED / MAP_FIXED_NOREPLACE are where `addr` stops being a hint and
    // becomes a REQUIREMENT -- and the target picks the address, so they cannot
    // be honoured. Both spellings.
    TEST_EXPECT_EQ((int)vivarium_mmap_decide(0x40000000ull, rw,
                                             ok_flags | (u64)VIV_MAP_FIXED,
                                             (u64)-1, 0),
                   (int)VIV_FORWARD, "MAP_FIXED declines -- addr is a requirement");
    TEST_EXPECT_EQ((int)vivarium_mmap_decide(0x40000000ull, rw,
                                             ok_flags | (u64)VIV_MAP_FIXED_NOREPLACE,
                                             (u64)-1, 0),
                   (int)VIV_FORWARD, "MAP_FIXED_NOREPLACE declines too");

    // ... but a non-NULL addr WITHOUT MAP_FIXED is admitted and ignored: Linux
    // specifies it as a hint the kernel may disregard, and the caller reads the
    // real address from the return. Ignoring it is conforming, not a compromise.
    TEST_EXPECT_EQ((int)vivarium_mmap_decide(0x40000000ull, rw, ok_flags,
                                             (u64)-1, 0),
                   (int)VIV_TRANSLATED, "a bare addr hint is admitted and ignored");

    // MAP_SHARED has no anonymous counterpart to share.
    TEST_EXPECT_EQ((int)vivarium_mmap_decide(0, rw,
                                             (u64)(VIV_MAP_SHARED | VIV_MAP_ANONYMOUS),
                                             (u64)-1, 0),
                   (int)VIV_FORWARD, "MAP_SHARED declines");

    // Exact flags equality, not a mask test. MAP_STACK and MAP_NORESERVE are
    // arguably honourable, but MEASURED musl sends neither -- admitting a flag
    // no caller sends would be speculation dressed as generosity.
    TEST_EXPECT_EQ((int)vivarium_mmap_decide(0, rw, ok_flags | (u64)VIV_MAP_STACK,
                                             (u64)-1, 0),
                   (int)VIV_FORWARD, "MAP_STACK declines (unmeasured, so unadmitted)");
    TEST_EXPECT_EQ((int)vivarium_mmap_decide(0, rw, ok_flags | (u64)VIV_MAP_NORESERVE,
                                             (u64)-1, 0),
                   (int)VIV_FORWARD, "MAP_NORESERVE declines (unmeasured)");

    // File-backed mmap is out: the row's whole target is anonymous memory.
    TEST_EXPECT_EQ((int)vivarium_mmap_decide(0, rw, ok_flags, 3, 0),
                   (int)VIV_FORWARD, "a real fd declines -- no file-backed mapping");
    TEST_EXPECT_EQ((int)vivarium_mmap_decide(0, rw, ok_flags, (u64)-1, 4096),
                   (int)VIV_FORWARD, "a nonzero offset declines");

    // `fd` is an int: a caller may leave x4 sign-extended or merely
    // zero-extended, and BOTH spellings of -1 must be recognised -- the same
    // trap AT_FDCWD carries in vivarium_openat_decide.
    TEST_EXPECT_EQ((int)vivarium_mmap_decide(0, rw, ok_flags, 0xFFFFFFFFull, 0),
                   (int)VIV_TRANSLATED, "zero-extended -1 fd is recognised");

    // The high halves of prot/flags/fd are NOT significant (all `int` in the
    // Linux ABI), so garbage there must not flip a verdict.
    TEST_EXPECT_EQ((int)vivarium_mmap_decide(0, 0xDEADBEEF00000000ull | rw,
                                             0xCAFE000000000000ull | ok_flags,
                                             (u64)-1, 0),
                   (int)VIV_TRANSLATED, "the high half of prot/flags is unread");

    // No length judgement here: `len` is a SEMANTIC question the shell answers
    // (EINVAL for 0, ENOMEM for too-large), so it is not a decide parameter at
    // all -- this asserts the signature has not grown one by accident.
}

// -----------------------------------------------------------------------------
// vivarium.signal_map — the signal<->note decode (V-6, §6.22).
//
// Asserts each mapped signal by NAME rather than sweeping a range, so a future
// edit that silently re-points SIGPIPE at the wrong note fails here. The
// UNMAPPED set is asserted just as explicitly: those are decisions (no note
// carries them), not gaps, and collapsing them into "default" would lose that.
// -----------------------------------------------------------------------------
void test_vivarium_signal_map(void) {
    // Every mapped row, checked individually.
    TEST_ASSERT(viv_signal_note(VIV_SIGINT)   == VIV_SIGNOTE_INTERRUPT,
                "SIGINT -> interrupt");
    TEST_ASSERT(viv_signal_note(VIV_SIGTERM)  == VIV_SIGNOTE_NONE,
                "SIGTERM has NO note: V-6b evicted it from `interrupt` so a "
                "per-signal disposition is representable at all");
    TEST_ASSERT(viv_signal_note(VIV_SIGKILL)  == VIV_SIGNOTE_KILL,
                "SIGKILL -> kill");
    TEST_ASSERT(viv_signal_note(VIV_SIGPIPE)  == VIV_SIGNOTE_PIPE,
                "SIGPIPE -> pipe");
    TEST_ASSERT(viv_signal_note(VIV_SIGCHLD)  == VIV_SIGNOTE_CHILD_EXIT,
                "SIGCHLD -> child_exit");
    TEST_ASSERT(viv_signal_note(VIV_SIGSEGV)  == VIV_SIGNOTE_SNARE_SEGV,
                "SIGSEGV -> snare:segv");
    TEST_ASSERT(viv_signal_note(VIV_SIGBUS)   == VIV_SIGNOTE_SNARE_BUS,
                "SIGBUS -> snare:bus");
    TEST_ASSERT(viv_signal_note(VIV_SIGILL)   == VIV_SIGNOTE_SNARE_ILL,
                "SIGILL -> snare:ill");
    TEST_ASSERT(viv_signal_note(VIV_SIGFPE)   == VIV_SIGNOTE_SNARE_FPE,
                "SIGFPE -> snare:fpe");
    TEST_ASSERT(viv_signal_note(VIV_SIGHUP)   == VIV_SIGNOTE_TTY_HUP,
                "SIGHUP -> tty:hup");
    TEST_ASSERT(viv_signal_note(VIV_SIGQUIT)  == VIV_SIGNOTE_TTY_QUIT,
                "SIGQUIT -> tty:quit");
    TEST_ASSERT(viv_signal_note(VIV_SIGWINCH) == VIV_SIGNOTE_TTY_WINCH,
                "SIGWINCH -> tty:winch");
    TEST_ASSERT(viv_signal_note(VIV_SIGTSTP)  == VIV_SIGNOTE_TTY_SUSP,
                "SIGTSTP -> tty:susp");
    TEST_ASSERT(viv_signal_note(VIV_SIGCONT)  == VIV_SIGNOTE_TTY_CONT,
                "SIGCONT -> tty:cont");

    // The UNMAPPED set, asserted by name. Each is a decision with a reason
    // recorded at the mapper's default arm; a future "fix" that invents a
    // delivery for one of these must come here and say why.
    TEST_ASSERT(viv_signal_note(14) == VIV_SIGNOTE_NONE, "SIGALRM: no timer note");
    TEST_ASSERT(viv_signal_note(10) == VIV_SIGNOTE_NONE, "SIGUSR1: no note");
    TEST_ASSERT(viv_signal_note(12) == VIV_SIGNOTE_NONE, "SIGUSR2: no note");
    TEST_ASSERT(viv_signal_note(VIV_SIGABRT) == VIV_SIGNOTE_NONE,
                "SIGABRT: reachable only via raise()");
    TEST_ASSERT(viv_signal_note(VIV_SIGSTOP) == VIV_SIGNOTE_NONE,
                "SIGSTOP: uncatchable, no note");
    TEST_ASSERT(viv_signal_note(34) == VIV_SIGNOTE_NONE,
                "realtime range needs queued siginfo (Tier 2)");

    // Out of range both ways -- 0 is not a signal, 65 is past _NSIG-1.
    TEST_ASSERT(viv_signal_note(0)  == VIV_SIGNOTE_NONE, "0 is not a signal");
    TEST_ASSERT(viv_signal_note(65) == VIV_SIGNOTE_NONE, "65 is past the range");
}

// -----------------------------------------------------------------------------
// vivarium.sigaction_domain — the rt_sigaction argument domain (V-6, §6.22).
// -----------------------------------------------------------------------------
void test_vivarium_sigaction_domain(void) {
    const u64 H = 0x400000;   // a plausible handler VA
    const u64 R = VIV_SA_RESTORER;

    // V-6b pinned this as VIV_FORWARD ("a real handler declines until V-6c can
    // deliver to it") and said the assertion would INVERT when the frame landed.
    // It has: the Tier-1 frame exists, so a handler with a restorer installs.
    TEST_ASSERT(vivarium_sigaction_decide(VIV_SIGINT, H, R, 8) == VIV_TRANSLATED,
                "V-6c: a real handler with SA_RESTORER now installs");

    // SIG_DFL / SIG_IGN need no trampoline -- nothing returns from them. This
    // is the load-bearing case: signal(SIGPIPE, SIG_IGN) is the single most
    // common signal call in real programs and it must work with no handler
    // machinery at all.
    TEST_ASSERT(vivarium_sigaction_decide(VIV_SIGPIPE, VIV_SIG_IGN, 0, 8)
                    == VIV_TRANSLATED,
                "SIG_IGN admitted without SA_RESTORER");
    TEST_ASSERT(vivarium_sigaction_decide(VIV_SIGPIPE, VIV_SIG_DFL, 0, 8)
                    == VIV_TRANSLATED,
                "SIG_DFL admitted without SA_RESTORER");

    // THE ARGUMENT DOMAIN: a real handler WITHOUT a restorer declines. We will
    // not synthesise one -- the only alternative is an executable vDSO page,
    // and the vDSO is deliberately RO+XN (I-12/I-13).
    TEST_ASSERT(vivarium_sigaction_decide(VIV_SIGINT, H, 0, 8) == VIV_FORWARD,
                "handler without SA_RESTORER declines");

    // Uncatchable by POSIX; Linux answers EINVAL for both. Recording a
    // disposition for SIGKILL would be a stored lie -- I-19's N-4 makes the
    // `kill` note non-catchable on the Thylacine side too.
    TEST_ASSERT(vivarium_sigaction_decide(VIV_SIGKILL, H, R, 8) == VIV_FORWARD,
                "SIGKILL declines");
    TEST_ASSERT(vivarium_sigaction_decide(VIV_SIGSTOP, H, R, 8) == VIV_FORWARD,
                "SIGSTOP declines");

    // SIG_ERR is POSIX-invalid. Without this check the recorded handler is -1
    // and a later delivery jumps there (the pouch layer's F11 audit close).
    TEST_ASSERT(vivarium_sigaction_decide(VIV_SIGINT, VIV_SIG_ERR, R, 8)
                    == VIV_FORWARD,
                "SIG_ERR declines");

    // No note carries it -> a disposition we could record but never act on.
    TEST_ASSERT(vivarium_sigaction_decide(14, H, R, 8) == VIV_FORWARD,
                "SIGALRM declines: no note carries it");
    TEST_ASSERT(vivarium_sigaction_decide(VIV_SIGTERM, VIV_SIG_IGN, 0, 8)
                    == VIV_FORWARD,
                "SIGTERM declines: V-6b left it with no note of its own");

    // UNDELIVERABLE notes take SIG_DFL and nothing else. An EL0 fault runs
    // proc_fault_terminate -> exits() without ever calling notes_post, so there
    // is no queue entry to catch or ignore -- but SIG_DFL is exactly what
    // already happens, so admitting it stores no lie.
    TEST_ASSERT(vivarium_sigaction_decide(VIV_SIGSEGV, VIV_SIG_DFL, 0, 8)
                    == VIV_TRANSLATED,
                "SIGSEGV + SIG_DFL admitted: terminate is what already happens");
    TEST_ASSERT(vivarium_sigaction_decide(VIV_SIGSEGV, VIV_SIG_IGN, 0, 8)
                    == VIV_FORWARD,
                "SIGSEGV + SIG_IGN declines: nothing to ignore, the fault "
                "terminates before any queue is consulted");
    TEST_ASSERT(vivarium_sigaction_decide(VIV_SIGBUS, H, R, 8) == VIV_FORWARD,
                "a SIGBUS handler declines: it could never be called");

    // SIGCHLD + SIG_IGN is Linux's AUTO-REAP, not "ignore". Thylacine reaps
    // only through wait_pid, so honouring the surface meaning would leave a
    // guest with zombies it believes are gone.
    TEST_ASSERT(vivarium_sigaction_decide(VIV_SIGCHLD, VIV_SIG_IGN, 0, 8)
                    == VIV_FORWARD,
                "SIGCHLD + SIG_IGN declines: it means auto-reap on Linux");
    TEST_ASSERT(vivarium_sigaction_decide(VIV_SIGCHLD, VIV_SIG_DFL, 0, 8)
                    == VIV_TRANSLATED,
                "SIGCHLD + SIG_DFL is still fine");

    // Range and sigsetsize.
    TEST_ASSERT(vivarium_sigaction_decide(0,  H, R, 8) == VIV_FORWARD, "sig 0");
    TEST_ASSERT(vivarium_sigaction_decide(65, H, R, 8) == VIV_FORWARD, "sig 65");
    TEST_ASSERT(vivarium_sigaction_decide(VIV_SIGINT, H, R, 16) == VIV_FORWARD,
                "sigsetsize must be 8");
}

// -----------------------------------------------------------------------------
// vivarium.sigset_to_notemask — the mask decode (V-6, §6.22).
// -----------------------------------------------------------------------------
void test_vivarium_sigset_to_notemask(void) {
    // The real NOTE_BIT_* numbering, passed in so vivarium.c stays free of
    // kernel headers. These values are notes.h's.
    const struct viv_notebit_map m = {
        .interrupt = 0, .kill = 1, .pipe = 2, .child_exit = 3,
        .snare = 4, .tty = 5,
    };

    // One signal -> one bit.
    TEST_ASSERT(viv_sigset_to_notemask(1ULL << (VIV_SIGPIPE - 1), &m) == (1ULL << 2),
                "SIGPIPE -> NOTE_BIT_PIPE");
    TEST_ASSERT(viv_sigset_to_notemask(1ULL << (VIV_SIGCHLD - 1), &m) == (1ULL << 3),
                "SIGCHLD -> NOTE_BIT_CHILD_EXIT");

    // SIGINT now owns `interrupt` alone, and SIGTERM contributes NOTHING --
    // the V-6b eviction. Before it, this union produced the same single bit for
    // a different reason (a collapse); asserting the union AND the SIGTERM-only
    // case separately is what tells the two apart.
    u64 both = (1ULL << (VIV_SIGINT - 1)) | (1ULL << (VIV_SIGTERM - 1));
    TEST_ASSERT(viv_sigset_to_notemask(both, &m) == (1ULL << 0),
                "SIGINT|SIGTERM -> the interrupt bit (from SIGINT alone)");
    TEST_ASSERT(viv_sigset_to_notemask(1ULL << (VIV_SIGTERM - 1), &m) == 0,
                "SIGTERM alone contributes no bit: it has no note");

    // The whole snare family folds onto ONE bit, because notes.h has a single
    // NOTE_BIT_SNARE covering the fault family.
    u64 faults = (1ULL << (VIV_SIGSEGV - 1)) | (1ULL << (VIV_SIGBUS - 1)) |
                 (1ULL << (VIV_SIGILL - 1))  | (1ULL << (VIV_SIGFPE - 1));
    TEST_ASSERT(viv_sigset_to_notemask(faults, &m) == (1ULL << 4),
                "the snare family folds onto one bit");

    // SIGKILL is DROPPED, not translated. This is what makes musl's
    // __block_all_sigs (which sets every bit, SIGKILL included) translatable
    // at all -- and it agrees with both sides: POSIX says SIGKILL is unmaskable
    // and I-19's N-4 says the `kill` note bypasses the mask.
    TEST_ASSERT(viv_sigset_to_notemask(1ULL << (VIV_SIGKILL - 1), &m) == 0,
                "SIGKILL is never maskable");

    // Unmapped signals are dropped rather than declining the whole call.
    TEST_ASSERT(viv_sigset_to_notemask(1ULL << 13 /* SIGALRM */, &m) == 0,
                "SIGALRM drops: nothing can deliver it either");

    // The wide mask musl actually sends: every bit set. Everything mappable
    // appears, SIGKILL does not.
    u64 all = viv_sigset_to_notemask(~0ULL, &m);
    TEST_ASSERT((all & (1ULL << 1)) == 0, "block-all still excludes kill");
    TEST_ASSERT(all == ((1ULL << 0) | (1ULL << 2) | (1ULL << 3) |
                        (1ULL << 4) | (1ULL << 5)),
                "block-all covers exactly the five maskable notes");

    // Fail closed on a NULL map rather than dereferencing it.
    TEST_ASSERT(viv_sigset_to_notemask(~0ULL, NULL) == 0, "NULL map -> 0");

    // The `how`/sigsetsize domain.
    TEST_ASSERT(vivarium_sigprocmask_decide(VIV_SIG_BLOCK, 8) == VIV_TRANSLATED,
                "SIG_BLOCK admitted");
    TEST_ASSERT(vivarium_sigprocmask_decide(VIV_SIG_SETMASK, 8) == VIV_TRANSLATED,
                "SIG_SETMASK admitted");
    TEST_ASSERT(vivarium_sigprocmask_decide(3, 8) == VIV_FORWARD, "bad how");
    TEST_ASSERT(vivarium_sigprocmask_decide(VIV_SIG_BLOCK, 16) == VIV_FORWARD,
                "sigsetsize must be 8");
}

// -----------------------------------------------------------------------------
// vivarium.signal_exclusivity — the domain rule that makes a per-signal
// disposition representable at all (V-6b, §6.22).
// -----------------------------------------------------------------------------
//
// A disposition is per-SIGNAL; a note carries no signal identity. So a signal
// may only carry one if it is the ONLY signal mapped to its note -- otherwise
// honouring the request silences a second signal the guest said nothing about.
// This is also what makes the REVERSE direction (note -> signal) well-defined,
// which is what the notes_post discard needs.
void test_vivarium_signal_exclusivity(void) {
    // Every mapped signal owns its note outright. V-6b made this universal by
    // evicting SIGTERM from `interrupt`; before that, SIGINT failed here -- and
    // SIGINT is precisely the signal a shell ignores.
    TEST_ASSERT(viv_signal_owns_note_exclusively(VIV_SIGINT),
                "SIGINT owns `interrupt` alone (the V-6b eviction)");
    TEST_ASSERT(viv_signal_owns_note_exclusively(VIV_SIGPIPE),  "SIGPIPE");
    TEST_ASSERT(viv_signal_owns_note_exclusively(VIV_SIGCHLD),  "SIGCHLD");
    TEST_ASSERT(viv_signal_owns_note_exclusively(VIV_SIGKILL),  "SIGKILL");
    TEST_ASSERT(viv_signal_owns_note_exclusively(VIV_SIGHUP),   "SIGHUP");
    TEST_ASSERT(viv_signal_owns_note_exclusively(VIV_SIGQUIT),  "SIGQUIT");
    TEST_ASSERT(viv_signal_owns_note_exclusively(VIV_SIGWINCH), "SIGWINCH");
    TEST_ASSERT(viv_signal_owns_note_exclusively(VIV_SIGTSTP),  "SIGTSTP");
    TEST_ASSERT(viv_signal_owns_note_exclusively(VIV_SIGCONT),  "SIGCONT");

    // The tty family shares ONE MASK BIT but five distinct NAMES, so each still
    // owns its own note. That distinction is the whole reason the reverse map
    // takes a name rather than a bit.
    TEST_ASSERT(viv_signal_note(VIV_SIGHUP) != viv_signal_note(VIV_SIGQUIT),
                "tty:hup and tty:quit are different notes despite one mask bit");

    // Each fault signal owns its own snare note too -- they are separate notes
    // that merely share NOTE_BIT_SNARE.
    TEST_ASSERT(viv_signal_owns_note_exclusively(VIV_SIGSEGV), "SIGSEGV");
    TEST_ASSERT(viv_signal_owns_note_exclusively(VIV_SIGBUS),  "SIGBUS");

    // An unmapped signal owns nothing -- there is no note to own.
    TEST_ASSERT(!viv_signal_owns_note_exclusively(VIV_SIGTERM),
                "SIGTERM owns nothing: no note");
    TEST_ASSERT(!viv_signal_owns_note_exclusively(14), "SIGALRM owns nothing");
    TEST_ASSERT(!viv_signal_owns_note_exclusively(0),  "0 is not a signal");
    TEST_ASSERT(!viv_signal_owns_note_exclusively(65), "65 is past the range");
}

// -----------------------------------------------------------------------------
// vivarium.signote_deliverable — which notes can actually reach a queue.
// -----------------------------------------------------------------------------
//
// MEASURED against notes.c, not assumed: g_known_notes holds interrupt / kill /
// pipe / child_exit / tty:*, and NOT snare:*. proc_fault_terminate calls
// exits(name) directly, so a fault never reaches notes_post at all.
void test_vivarium_signote_deliverable(void) {
    TEST_ASSERT(viv_signote_is_deliverable(VIV_SIGNOTE_INTERRUPT),  "interrupt");
    TEST_ASSERT(viv_signote_is_deliverable(VIV_SIGNOTE_KILL),       "kill");
    TEST_ASSERT(viv_signote_is_deliverable(VIV_SIGNOTE_PIPE),       "pipe");
    TEST_ASSERT(viv_signote_is_deliverable(VIV_SIGNOTE_CHILD_EXIT), "child_exit");
    TEST_ASSERT(viv_signote_is_deliverable(VIV_SIGNOTE_TTY_HUP),    "tty:hup");
    TEST_ASSERT(viv_signote_is_deliverable(VIV_SIGNOTE_TTY_QUIT),   "tty:quit");
    TEST_ASSERT(viv_signote_is_deliverable(VIV_SIGNOTE_TTY_WINCH),  "tty:winch");
    TEST_ASSERT(viv_signote_is_deliverable(VIV_SIGNOTE_TTY_SUSP),   "tty:susp");
    TEST_ASSERT(viv_signote_is_deliverable(VIV_SIGNOTE_TTY_CONT),   "tty:cont");

    // The snare family is NOT in g_known_notes. A SIGSEGV handler could never
    // be called, so the domain refuses to store one.
    TEST_ASSERT(!viv_signote_is_deliverable(VIV_SIGNOTE_SNARE_SEGV), "snare:segv");
    TEST_ASSERT(!viv_signote_is_deliverable(VIV_SIGNOTE_SNARE_BUS),  "snare:bus");
    TEST_ASSERT(!viv_signote_is_deliverable(VIV_SIGNOTE_SNARE_ILL),  "snare:ill");
    TEST_ASSERT(!viv_signote_is_deliverable(VIV_SIGNOTE_SNARE_FPE),  "snare:fpe");
    TEST_ASSERT(!viv_signote_is_deliverable(VIV_SIGNOTE_NONE),       "NONE");
}

// -----------------------------------------------------------------------------
// vivarium.sigtab — the disposition table + the note-name reverse map (V-6b).
// -----------------------------------------------------------------------------
void test_vivarium_sigtab(void) {
    // The reverse map ROUND-TRIPS against the forward one for every deliverable
    // signal. Driving it from the forward map means a future edit to either
    // direction alone fails here rather than drifting silently.
    struct { u64 sig; const char *name; } rt[] = {
        { VIV_SIGINT,   NOTE_NAME_INTERRUPT  },
        { VIV_SIGKILL,  NOTE_NAME_KILL       },
        { VIV_SIGPIPE,  NOTE_NAME_PIPE       },
        { VIV_SIGCHLD,  NOTE_NAME_CHILD_EXIT },
        { VIV_SIGHUP,   NOTE_NAME_TTY_HUP    },
        { VIV_SIGQUIT,  NOTE_NAME_TTY_QUIT   },
        { VIV_SIGWINCH, NOTE_NAME_TTY_WINCH  },
        { VIV_SIGTSTP,  NOTE_NAME_TTY_SUSP   },
        { VIV_SIGCONT,  NOTE_NAME_TTY_CONT   },
    };
    for (u32 i = 0; i < sizeof(rt) / sizeof(rt[0]); i++) {
        TEST_ASSERT(viv_signote_from_note_name(rt[i].name)
                        == viv_signal_note(rt[i].sig),
                    "note name round-trips to the signal's own note");
    }

    // Anything not in the supported set decodes to NONE -- never to a signal.
    TEST_ASSERT(viv_signote_from_note_name("snare:segv") == VIV_SIGNOTE_NONE,
                "snare:* is not postable, so it decodes to nothing");
    TEST_ASSERT(viv_signote_from_note_name("nonesuch") == VIV_SIGNOTE_NONE,
                "an unknown name is never treated as a signal");
    TEST_ASSERT(viv_signote_from_note_name(NULL) == VIV_SIGNOTE_NONE, "NULL");

    // The table itself. A Proc with no table ignores nothing -- which is why a
    // NULL-safe read matters: it is the state every native Proc is in forever.
    struct viv_ksigaction ign = { .handler = VIV_SIG_IGN };
    struct viv_ksigaction dfl = { .handler = VIV_SIG_DFL };
    struct viv_ksigaction hnd = { .handler = 0x400000, .flags = VIV_SA_RESTORER,
                                  .restorer = 0x400100, .mask = 0 };
    struct viv_ksigaction got;

    TEST_ASSERT(!viv_sigtab_note_ignored(NULL, VIV_SIGNOTE_PIPE),
                "no table -> nothing ignored");
    TEST_ASSERT(!viv_sigtab_set(NULL, VIV_SIGNOTE_PIPE, &ign),
                "setting through a NULL table fails rather than faulting");
    TEST_ASSERT(!viv_sigtab_note_handler(NULL, VIV_SIGNOTE_PIPE, &got),
                "no table -> no handler");

    struct viv_sigtab tab;
    for (u32 i = 0; i < (u32)sizeof(tab); i++) ((u8 *)&tab)[i] = 0;

    // A ZEROED table reads as all-SIG_DFL. That is what lets the lazy
    // allocation in viv_sigtab_of skip an initialiser -- kzalloc IS the init.
    TEST_ASSERT(!viv_sigtab_note_ignored(&tab, VIV_SIGNOTE_PIPE),
                "fresh table ignores nothing");
    TEST_ASSERT(!viv_sigtab_note_handler(&tab, VIV_SIGNOTE_PIPE, &got),
                "fresh table has no handler -- zero IS SIG_DFL");

    TEST_ASSERT(viv_sigtab_set(&tab, VIV_SIGNOTE_PIPE, &ign), "set IGN");
    TEST_ASSERT(viv_sigtab_note_ignored(&tab, VIV_SIGNOTE_PIPE), "pipe ignored");
    TEST_ASSERT(!viv_sigtab_note_handler(&tab, VIV_SIGNOTE_PIPE, &got),
                "SIG_IGN is not a handler -- delivery must never jump to 1");

    // Entries are INDEPENDENT: ignoring one note must not ignore its neighbours.
    // With the tty family sharing a mask bit but not a note, this is the
    // property that lets a guest ignore SIGWINCH while SIGHUP still kills it.
    TEST_ASSERT(!viv_sigtab_note_ignored(&tab, VIV_SIGNOTE_INTERRUPT),
                "ignoring pipe did not touch interrupt");
    TEST_ASSERT(viv_sigtab_set(&tab, VIV_SIGNOTE_TTY_WINCH, &ign), "winch");
    TEST_ASSERT(viv_sigtab_note_ignored(&tab, VIV_SIGNOTE_TTY_WINCH), "winch set");
    TEST_ASSERT(!viv_sigtab_note_ignored(&tab, VIV_SIGNOTE_TTY_HUP),
                "tty:hup stays deliverable though it shares a MASK bit");

    // V-6c: a REAL handler round-trips whole. The restorer is the part that
    // matters most -- it is the guest's own trampoline and the only way back
    // out of the handler.
    TEST_ASSERT(viv_sigtab_set(&tab, VIV_SIGNOTE_INTERRUPT, &hnd), "set handler");
    TEST_ASSERT(viv_sigtab_note_handler(&tab, VIV_SIGNOTE_INTERRUPT, &got),
                "handler reported");
    TEST_ASSERT(got.handler == 0x400000 && got.restorer == 0x400100 &&
                got.flags == VIV_SA_RESTORER,
                "the whole k_sigaction survives, restorer included");
    TEST_ASSERT(!viv_sigtab_note_ignored(&tab, VIV_SIGNOTE_INTERRUPT),
                "a handler is not 'ignored'");

    // Clearing is a real operation, not just a set -- SIG_DFL after SIG_IGN.
    TEST_ASSERT(viv_sigtab_set(&tab, VIV_SIGNOTE_PIPE, &dfl), "clear");
    TEST_ASSERT(!viv_sigtab_note_ignored(&tab, VIV_SIGNOTE_PIPE), "pipe restored");

    // Out of range is refused rather than writing past the array.
    TEST_ASSERT(!viv_sigtab_set(&tab, (enum viv_signote)VIV_SIGNOTE_COUNT, &ign),
                "out-of-range note refused");
    TEST_ASSERT(!viv_sigtab_note_ignored(&tab, (enum viv_signote)VIV_SIGNOTE_COUNT),
                "out-of-range read refused");
    TEST_ASSERT(!viv_sigtab_note_handler(&tab, (enum viv_signote)VIV_SIGNOTE_COUNT,
                                         &got),
                "out-of-range handler read refused");
    TEST_ASSERT(!viv_sigtab_note_handler(&tab, VIV_SIGNOTE_INTERRUPT, NULL),
                "a NULL out-pointer is refused rather than written through");
}

// -----------------------------------------------------------------------------
// vivarium.notemask_to_sigset — reporting a mask back honestly (V-6b).
// -----------------------------------------------------------------------------
void test_vivarium_notemask_to_sigset(void) {
    const struct viv_notebit_map m = {
        .interrupt = 0, .kill = 1, .pipe = 2, .child_exit = 3,
        .snare = 4, .tty = 5,
    };

    TEST_ASSERT(viv_notemask_to_sigset(1ULL << 2, &m)
                    == (1ULL << (VIV_SIGPIPE - 1)),
                "the pipe bit reports exactly SIGPIPE");

    // THE HONEST OVER-REPORT. One tty bit blocks five signals, so reading the
    // mask back names all five. Showing the guest the tidy answer it asked for
    // while blocking wider is the lie this avoids.
    u64 tty = viv_notemask_to_sigset(1ULL << 5, &m);
    TEST_ASSERT(tty & (1ULL << (VIV_SIGWINCH - 1)), "SIGWINCH reported");
    TEST_ASSERT(tty & (1ULL << (VIV_SIGHUP - 1)),
                "SIGHUP reported too -- blocking SIGWINCH really does block it");
    TEST_ASSERT(tty & (1ULL << (VIV_SIGQUIT - 1)), "SIGQUIT reported");
    TEST_ASSERT(tty & (1ULL << (VIV_SIGTSTP - 1)), "SIGTSTP reported");
    TEST_ASSERT(tty & (1ULL << (VIV_SIGCONT - 1)), "SIGCONT reported");
    TEST_ASSERT((tty & (1ULL << (VIV_SIGPIPE - 1))) == 0,
                "and nothing outside the family");

    // SIGKILL is never blocked, so it is never reported blocked -- the mirror
    // of the forward direction dropping it.
    TEST_ASSERT((viv_notemask_to_sigset(~0ULL, &m) & (1ULL << (VIV_SIGKILL - 1)))
                    == 0,
                "block-everything still never reports SIGKILL");

    // A signal with no note is never reported blocked: nothing can deliver it,
    // so "blocked" would describe a state that does not exist.
    TEST_ASSERT((viv_notemask_to_sigset(~0ULL, &m) & (1ULL << (VIV_SIGTERM - 1)))
                    == 0,
                "SIGTERM is never reported blocked -- it has no note");

    // Round-trip on the shape musl sends: block-all, read back, and every
    // signal that CAN be blocked appears.
    u64 notes = viv_sigset_to_notemask(~0ULL, &m);
    u64 back  = viv_notemask_to_sigset(notes, &m);
    TEST_ASSERT(back & (1ULL << (VIV_SIGINT - 1)),  "SIGINT survives the round trip");
    TEST_ASSERT(back & (1ULL << (VIV_SIGCHLD - 1)), "SIGCHLD survives");
    TEST_ASSERT(back & (1ULL << (VIV_SIGSEGV - 1)), "SIGSEGV survives");

    TEST_ASSERT(viv_notemask_to_sigset(~0ULL, NULL) == 0, "NULL map -> 0");
}

// -----------------------------------------------------------------------------
// vivarium.signote_reverse — note -> signal, and the Linux default table (V-6c).
//
// The reverse direction is what delivery uses to fill si_signo and x0, so it is
// the point where the V-6b exclusivity rule stops being tidiness and becomes
// load-bearing: without it there would be no single answer to give.
// -----------------------------------------------------------------------------
void test_vivarium_signote_reverse(void);
void test_vivarium_signote_reverse(void) {
    // Every signal that owns its note exclusively must round-trip through both
    // directions. Driven from viv_signal_note itself so a future row cannot be
    // added to one direction and forgotten in the other.
    for (u64 sig = 1; sig <= VIV_NSIG; sig++) {
        enum viv_signote n = viv_signal_note(sig);
        if (n == VIV_SIGNOTE_NONE) continue;
        if (!viv_signal_owns_note_exclusively(sig)) continue;
        TEST_ASSERT(viv_signote_to_signal(n) == sig,
                    "an exclusively-owned note maps back to its own signal");
    }

    TEST_ASSERT(viv_signote_to_signal(VIV_SIGNOTE_NONE) == 0,
                "NONE maps to no signal");
    TEST_ASSERT(viv_signote_to_signal(VIV_SIGNOTE_INTERRUPT) == VIV_SIGINT,
                "interrupt is SIGINT's alone since V-6b evicted SIGTERM");

    // The Linux default-action table. Only these three do nothing by default;
    // getting the set wrong either drops a signal that should kill (too many)
    // or lets a queue fill with notes nothing consumes (too few).
    TEST_ASSERT(viv_signote_default_is_ignore(VIV_SIGNOTE_CHILD_EXIT), "SIGCHLD");
    TEST_ASSERT(viv_signote_default_is_ignore(VIV_SIGNOTE_TTY_WINCH), "SIGWINCH");
    TEST_ASSERT(viv_signote_default_is_ignore(VIV_SIGNOTE_TTY_CONT), "SIGCONT");

    TEST_ASSERT(!viv_signote_default_is_ignore(VIV_SIGNOTE_INTERRUPT), "SIGINT kills");
    TEST_ASSERT(!viv_signote_default_is_ignore(VIV_SIGNOTE_TTY_HUP), "SIGHUP kills");
    TEST_ASSERT(!viv_signote_default_is_ignore(VIV_SIGNOTE_TTY_QUIT), "SIGQUIT kills");
    TEST_ASSERT(!viv_signote_default_is_ignore(VIV_SIGNOTE_PIPE), "SIGPIPE kills");
    TEST_ASSERT(!viv_signote_default_is_ignore(VIV_SIGNOTE_KILL), "SIGKILL kills");
    TEST_ASSERT(!viv_signote_default_is_ignore(VIV_SIGNOTE_NONE), "NONE");

    // SIGTSTP's default is STOP, not ignore. Reporting "ignore" would silently
    // discard a job-control signal; the kernel NDFLT-stop arm is task #15.
    TEST_ASSERT(!viv_signote_default_is_ignore(VIV_SIGNOTE_TTY_SUSP),
                "SIGTSTP defaults to STOP, which is not 'ignore'");
}

// -----------------------------------------------------------------------------
// vivarium.sigframe — the Tier-1 frame the guest reads (V-6c).
// -----------------------------------------------------------------------------
void test_vivarium_sigframe(void);
void test_vivarium_sigframe(void) {
    // The layout constants ARE the ABI. These are the numbers the aarch64
    // target compiler produces for musl's own declarations; the host compiler
    // gives 2328/2496 instead, because macOS `long double` is 8 bytes where
    // aarch64's is 16. Measuring on the wrong target is how that goes wrong.
    TEST_ASSERT(VIV_SIGINFO_SIZE == 128, "siginfo_t 128");
    TEST_ASSERT(VIV_UCONTEXT_SIZE == 4560, "ucontext_t 4560");
    TEST_ASSERT(VIV_SIGFRAME_SIZE == 4688, "rt_sigframe 4688");
    TEST_ASSERT((VIV_SIGFRAME_SIZE % 16u) == 0,
                "the frame size must be a multiple of 16 or the handler's sp "
                "loses its AAPCS64 alignment");
    TEST_ASSERT((VIV_SIGFRAME_TOTAL % 16u) == 0, "total 16-aligned");

    // Poison the buffer first: the builder must overwrite EVERY byte, pads
    // included, or a kernel stack frame reaches the guest (I-13).
    struct viv_sigframe_head f;
    for (u32 i = 0; i < (u32)sizeof(f); i++) ((u8 *)&f)[i] = 0xA5;

    u64 regs[31];
    for (u32 i = 0; i < 31; i++) regs[i] = 0x1000 + i;

    vivarium_build_sigframe(&f, VIV_SIGINT, 0x5ULL, regs,
                            0x7ff00000ull, 0x400abcull, 0x60000000ull);

    TEST_ASSERT(f.info.si_signo == (s32)VIV_SIGINT, "si_signo");
    TEST_ASSERT(f.info.si_errno == 0, "si_errno");
    TEST_ASSERT(f.info.si_code == VIV_SI_KERNEL, "si_code = SI_KERNEL");

    // The union must be ZERO, not poison: SI_KERNEL says "no further fields",
    // and leaking 0xA5 there would be a kernel-memory disclosure that a guest
    // could read as si_pid.
    for (u32 i = 0; i < (u32)sizeof(f.info.__pad1); i++)
        TEST_ASSERT(f.info.__pad1[i] == 0, "siginfo union zeroed, not poisoned");
    TEST_ASSERT(f.info.__pad0 == 0, "siginfo pad zeroed");

    TEST_ASSERT(f.uc.uc_flags == 0 && f.uc.uc_link == 0, "ucontext header");
    TEST_ASSERT(f.uc.ss_flags == 2 && f.uc.ss_sp == 0 && f.uc.ss_size == 0,
                "SS_DISABLE: sigaltstack is an ENOSYS row, so there is never "
                "an alternate stack to describe");
    TEST_ASSERT(f.uc.uc_sigmask[0] == 0x5ULL, "the saved mask is reported");
    for (u32 i = 1; i < 16; i++)
        TEST_ASSERT(f.uc.uc_sigmask[i] == 0, "sigset words 1..15 zeroed");
    for (u32 i = 0; i < (u32)sizeof(f.uc.__pad_mctx); i++)
        TEST_ASSERT(f.uc.__pad_mctx[i] == 0, "the mcontext alignment pad zeroed");

    // The interrupted context, which is the whole reason the frame is worth
    // writing: a crash reporter reading uc_mcontext.pc gets the real PC.
    for (u32 i = 0; i < 31; i++)
        TEST_ASSERT(f.uc.uc_mcontext.regs[i] == 0x1000 + i, "regs mirrored");
    TEST_ASSERT(f.uc.uc_mcontext.sp == 0x7ff00000ull, "sp");
    TEST_ASSERT(f.uc.uc_mcontext.pc == 0x400abcull, "pc");
    TEST_ASSERT(f.uc.uc_mcontext.pstate == 0x60000000ull, "pstate");
    TEST_ASSERT(f.uc.uc_mcontext.fault_address == 0,
                "no deliverable note is fault-generated -- snare:* never "
                "reaches a queue, so there is no faulting address to report");

    // The _aarch64_ctx chain terminator. A guest walking uc_mcontext.__reserved
    // must stop at once rather than following whatever was on the stack.
    TEST_ASSERT(f.uc.uc_mcontext.end_magic == 0 && f.uc.uc_mcontext.end_size == 0,
                "the record chain is terminated immediately -- no FPSIMD record "
                "is claimed, because note delivery does not save V regs (#96)");

    // NULL is refused rather than dereferenced.
    vivarium_build_sigframe(NULL, VIV_SIGINT, 0, regs, 0, 0, 0);

    // A NULL regs pointer leaves the register block zero rather than faulting.
    struct viv_sigframe_head g;
    vivarium_build_sigframe(&g, VIV_SIGPIPE, 0, NULL, 1, 2, 3);
    TEST_ASSERT(g.uc.uc_mcontext.regs[0] == 0 && g.uc.uc_mcontext.regs[30] == 0,
                "NULL regs -> zeroed register block, no fault");
    TEST_ASSERT(g.info.si_signo == (s32)VIV_SIGPIPE, "second build is clean");
}

// =============================================================================
// SOCKETS (V-5). The pure half: the argument domain, the address parse, the
// command builder, the connection-number parse, and the table.
// =============================================================================

void test_vivarium_socket_domain(void);
void test_vivarium_socket_domain(void) {
    enum viv_net_proto proto = (enum viv_net_proto)0xff;
    s32 err = 0;

    // The two admitted shapes.
    TEST_ASSERT(vivarium_socket_decide(2, 1, 0, &proto, &err) && proto == VIV_NET_TCP,
                "AF_INET + SOCK_STREAM -> tcp");
    TEST_ASSERT(vivarium_socket_decide(2, 2, 0, &proto, &err) && proto == VIV_NET_UDP,
                "AF_INET + SOCK_DGRAM -> udp");
    // The family default protocol number is the same call.
    TEST_ASSERT(vivarium_socket_decide(2, 1, 6, &proto, &err) && proto == VIV_NET_TCP,
                "IPPROTO_TCP is the STREAM default, not a different socket");
    TEST_ASSERT(vivarium_socket_decide(2, 2, 17, &proto, &err) && proto == VIV_NET_UDP,
                "IPPROTO_UDP is the DGRAM default");

    // AF_INET6 declines with the errno a guest can act on -- NOT EINVAL, which
    // would make it retry the same address.
    TEST_ASSERT(!vivarium_socket_decide(10, 1, 0, &proto, &err), "AF_INET6 declines");
    TEST_ASSERT(err == T_E_AFNOSUPPORT, "AF_INET6 -> EAFNOSUPPORT, not EINVAL");
    TEST_ASSERT(!vivarium_socket_decide(1, 1, 0, &proto, &err), "AF_UNIX declines here");
    TEST_ASSERT(err == T_E_AFNOSUPPORT, "AF_UNIX -> EAFNOSUPPORT (the /srv path is pouch's)");

    // The type-word flags are REFUSED, not masked off. This is the leg that
    // matters: silently dropping SOCK_NONBLOCK gives the guest a blocking
    // socket where it expected EAGAIN.
    TEST_ASSERT(!vivarium_socket_decide(2, 1 | 04000, 0, &proto, &err),
                "SOCK_NONBLOCK is refused, never ignored");
    TEST_ASSERT(err == T_E_INVAL, "SOCK_NONBLOCK -> EINVAL");
    TEST_ASSERT(!vivarium_socket_decide(2, 1 | 02000000, 0, &proto, &err),
                "SOCK_CLOEXEC is refused, never ignored");

    // No /net analogue.
    TEST_ASSERT(!vivarium_socket_decide(2, 5, 0, &proto, &err), "SOCK_SEQPACKET declines");
    TEST_ASSERT(err == T_E_PROTONOSUPPORT, "SOCK_SEQPACKET -> EPROTONOSUPPORT");
    TEST_ASSERT(!vivarium_socket_decide(2, 3, 0, &proto, &err), "SOCK_RAW declines");
    // A protocol number that contradicts the type.
    TEST_ASSERT(!vivarium_socket_decide(2, 1, 17, &proto, &err),
                "SOCK_STREAM + IPPROTO_UDP is not a socket netd can serve");

    // Fail closed on NULL outputs.
    TEST_ASSERT(!vivarium_socket_decide(2, 1, 0, NULL, &err), "NULL proto -> false");
    TEST_ASSERT(!vivarium_socket_decide(2, 1, 0, &proto, NULL), "NULL err -> false");

    TEST_ASSERT(vivarium_net_proto_dir(VIV_NET_TCP)[0] == 't', "tcp dir");
    TEST_ASSERT(vivarium_net_proto_dir(VIV_NET_UDP)[0] == 'u', "udp dir");
}

void test_vivarium_sockaddr(void);
void test_vivarium_sockaddr(void) {
    u8  ip[4];
    u16 port = 0;

    // AF_INET 10.0.2.2:80. family little-endian, port NETWORK order (hi,lo).
    u8 sa[16] = { 2, 0, 0, 80, 10, 0, 2, 2 };
    TEST_ASSERT(vivarium_sockaddr_in_parse(sa, sizeof(sa), ip, &port), "parses");
    TEST_ASSERT(ip[0] == 10 && ip[1] == 0 && ip[2] == 2 && ip[3] == 2, "octets in order");
    TEST_ASSERT(port == 80, "port is network order (hi byte first)");

    // A port above 255 exercises both bytes -- 0x1F90 == 8080.
    sa[2] = 0x1F; sa[3] = 0x90;
    TEST_ASSERT(vivarium_sockaddr_in_parse(sa, sizeof(sa), ip, &port) && port == 8080,
                "two-byte port assembles correctly");

    // Refusals: wrong family, short buffer, port 0.
    sa[0] = 10;
    TEST_ASSERT(!vivarium_sockaddr_in_parse(sa, sizeof(sa), ip, &port), "AF_INET6 refused");
    sa[0] = 2;
    TEST_ASSERT(!vivarium_sockaddr_in_parse(sa, 7, ip, &port), "short sockaddr refused");
    sa[2] = 0; sa[3] = 0;
    TEST_ASSERT(!vivarium_sockaddr_in_parse(sa, sizeof(sa), ip, &port),
                "port 0 refused -- netd's dial parser rejects it");
    TEST_ASSERT(!vivarium_sockaddr_in_parse(NULL, 16, ip, &port), "NULL refused");
}

void test_vivarium_net_cmd(void);
void test_vivarium_net_cmd(void) {
    char buf[48];
    u8   ip[4] = { 10, 0, 2, 2 };

    u32 n = vivarium_net_cmd_ipport(buf, sizeof(buf), "connect", ip, 80);
    // Compare against the literal rather than a hand-counted length -- a
    // hardcoded count is a second thing to get wrong, and it would be checking
    // my arithmetic rather than the builder.
    const char *want = "connect 10.0.2.2!80";
    u32 wl = 0; while (want[wl]) wl++;
    TEST_ASSERT(n == wl, "length matches the literal");
    for (u32 i = 0; i < wl; i++) TEST_ASSERT(buf[i] == want[i], "byte matches");

    // A 0.0.0.0 address still renders every octet.
    u8 z[4] = { 0, 0, 0, 0 };
    u32 zn = vivarium_net_cmd_ipport(buf, sizeof(buf), "connect", z, 1);
    TEST_ASSERT(zn == 17 && buf[8] == '0' && buf[9] == '.', "zero octets render as 0");

    // Overflow returns 0 rather than a truncated command -- a short write to
    // netd's ctl would be a DIFFERENT dial, which is the whole hazard.
    TEST_ASSERT(vivarium_net_cmd_ipport(buf, 8, "connect", ip, 80) == 0,
                "no room -> 0, never a truncated verb");
    TEST_ASSERT(vivarium_net_cmd_ipport(NULL, 48, "connect", ip, 80) == 0, "NULL -> 0");
}

void test_vivarium_conn_n(void);
void test_vivarium_conn_n(void) {
    u32 n = 0xffffffffu;
    TEST_ASSERT(vivarium_parse_conn_n("0", 1, &n) && n == 0, "connection 0 is valid");
    TEST_ASSERT(vivarium_parse_conn_n("7", 1, &n) && n == 7, "single digit");
    TEST_ASSERT(vivarium_parse_conn_n("123", 3, &n) && n == 123, "multi digit");
    // netd may pad the line; stop at the first terminator rather than failing.
    TEST_ASSERT(vivarium_parse_conn_n("42\n", 3, &n) && n == 42, "newline terminates");
    TEST_ASSERT(vivarium_parse_conn_n("42 ", 3, &n) && n == 42, "space terminates");
    TEST_ASSERT(vivarium_parse_conn_n("9\0abc", 5, &n) && n == 9, "NUL terminates");

    // Refusals -- each would otherwise yield connection 0 and dial a stranger.
    TEST_ASSERT(!vivarium_parse_conn_n("", 0, &n), "empty refused");
    TEST_ASSERT(!vivarium_parse_conn_n("\n", 1, &n), "terminator-only refused");
    TEST_ASSERT(!vivarium_parse_conn_n("x", 1, &n), "non-decimal refused");
    TEST_ASSERT(!vivarium_parse_conn_n("1x", 2, &n), "trailing garbage refused");
    TEST_ASSERT(!vivarium_parse_conn_n("99999999999", 11, &n), "u32 overflow refused");
    TEST_ASSERT(!vivarium_parse_conn_n(NULL, 1, &n), "NULL refused");
}

void test_vivarium_socktab(void);
void test_vivarium_socktab(void) {
    static struct viv_socktab tab;
    for (u32 i = 0; i < VIV_SOCK_MAX; i++) {
        tab.s[i].fd = -1; tab.s[i].state = VIV_SOCK_FREE;
        tab.s[i].proto = 0; tab.s[i].n = 0;
    }

    TEST_ASSERT(viv_socktab_find(&tab, 0) == NULL, "fd 0 does not match a free entry");
    TEST_ASSERT(viv_socktab_find(NULL, 3) == NULL, "NULL table is safe");

    struct viv_sock *a = viv_socktab_claim(&tab, 0, VIV_NET_TCP, 5);
    TEST_ASSERT(a != NULL && a->fd == 0 && a->n == 5, "fd 0 is a claimable socket");
    TEST_ASSERT(a->state == VIV_SOCK_FRESH, "a new socket is FRESH, not CONNECTED");
    TEST_ASSERT(viv_socktab_find(&tab, 0) == a, "found by fd");
    TEST_ASSERT(viv_socktab_find(&tab, 1) == NULL, "a different fd does not match");

    struct viv_sock *b = viv_socktab_claim(&tab, 9, VIV_NET_UDP, 2);
    TEST_ASSERT(b != NULL && b != a && b->proto == VIV_NET_UDP, "second, distinct");

    // Drop clears the WHOLE entry: a later claim must not inherit stale state.
    viv_socktab_drop(&tab, 0);
    TEST_ASSERT(viv_socktab_find(&tab, 0) == NULL, "dropped");
    TEST_ASSERT(viv_socktab_find(&tab, 9) == b, "the sibling survives the drop");
    viv_socktab_drop(&tab, 0);              // idempotent
    viv_socktab_drop(&tab, 12345);          // an fd that was never a socket
    viv_socktab_drop(NULL, 9);              // NULL-safe

    struct viv_sock *c = viv_socktab_claim(&tab, 0, VIV_NET_TCP, 77);
    TEST_ASSERT(c != NULL && c->n == 77 && c->state == VIV_SOCK_FRESH,
                "a reused slot carries no stale (proto, N) -- the close-hook "
                "bug this table exists to prevent would show up here");

    // Exhaustion is EMFILE-shaped (NULL), not a wrap or an overwrite.
    u32 claimed = 2;
    for (s32 fd = 100; claimed < VIV_SOCK_MAX; fd++) {
        if (!viv_socktab_claim(&tab, fd, VIV_NET_TCP, 1)) break;
        claimed++;
    }
    TEST_ASSERT(claimed == VIV_SOCK_MAX, "the table fills to exactly VIV_SOCK_MAX");
    TEST_ASSERT(viv_socktab_claim(&tab, 9999, VIV_NET_TCP, 1) == NULL,
                "a full table refuses rather than overwriting");
}

// The close hook's regression (V-5). The hook lives in viv_linux_dispatch,
// which needs a phenotyped Proc at EL0, so this drives the TABLE operation the
// hook performs and pins the property the hook exists to guarantee: after a
// drop, the index is reusable and carries NOTHING from its previous tenant.
//
// Without the hook, close() frees the fd INDEX while the entry survives, the
// next fd-creating syscall gets that index back, and a later connect() finds a
// stale entry -- writing a dial verb to a STRANGER'S connection. That is the
// failure this asserts is impossible once drop has run.
void test_vivarium_socktab_close_hook(void);
void test_vivarium_socktab_close_hook(void) {
    static struct viv_socktab tab;
    for (u32 i = 0; i < VIV_SOCK_MAX; i++) {
        tab.s[i].fd = -1; tab.s[i].state = VIV_SOCK_FREE;
        tab.s[i].proto = 0; tab.s[i].n = 0;
    }

    // fd 4 is a TCP socket on connection 11, and gets connected.
    struct viv_sock *s = viv_socktab_claim(&tab, 4, VIV_NET_TCP, 11);
    TEST_ASSERT(s != NULL, "claim fd 4");
    s->state = VIV_SOCK_CONNECTED;

    // close(4) -- the hook.
    viv_socktab_drop(&tab, 4);

    // The kernel hands index 4 back to an unrelated open(). A socket call on it
    // must now say "not a socket", NOT resolve to connection 11.
    TEST_ASSERT(viv_socktab_find(&tab, 4) == NULL,
                "a recycled fd index resolves to NO socket -- if this finds an "
                "entry, connect() on an unrelated file would dial connection 11");

    // And when fd 4 legitimately becomes a socket again, it is a FRESH one.
    struct viv_sock *s2 = viv_socktab_claim(&tab, 4, VIV_NET_UDP, 3);
    TEST_ASSERT(s2 != NULL, "reclaim fd 4");
    TEST_ASSERT(s2->state == VIV_SOCK_FRESH, "the new socket is FRESH, not CONNECTED");
    TEST_ASSERT(s2->proto == VIV_NET_UDP && s2->n == 3,
                "the new socket carries its OWN (proto, N) -- no bleed from the "
                "previous tenant of this index");
}

// V-5b: the bind fields are part of the recycled-slot contract too. A slot that
// kept the previous socket's port would let listen() announce a port THIS
// socket never asked for -- the close-hook bug wearing a different hat.
void test_vivarium_socktab_bind_fields(void);
void test_vivarium_socktab_bind_fields(void) {
    static struct viv_socktab tab;
    for (u32 i = 0; i < VIV_SOCK_MAX; i++) {
        tab.s[i].fd = -1; tab.s[i].state = VIV_SOCK_FREE;
        tab.s[i].proto = 0; tab.s[i].n = 0;
        tab.s[i].bound_addr = 0; tab.s[i].bound_port = 0;
    }

    TEST_ASSERT(viv_socktab_has_room(&tab), "an empty table has room");
    TEST_ASSERT(!viv_socktab_has_room(NULL), "NULL has no room (and does not fault)");

    struct viv_sock *s = viv_socktab_claim(&tab, 7, VIV_NET_TCP, 3);
    TEST_ASSERT(s != NULL, "claim fd 7");
    TEST_ASSERT(s->bound_addr == 0 && s->bound_port == 0,
                "a fresh socket carries NO bind");

    s->bound_addr = 0x7F000001u;   // 127.0.0.1
    s->bound_port = 7789;
    s->state      = VIV_SOCK_LISTENING;

    viv_socktab_drop(&tab, 7);

    struct viv_sock *s2 = viv_socktab_claim(&tab, 7, VIV_NET_TCP, 4);
    TEST_ASSERT(s2 != NULL, "reclaim fd 7");
    TEST_ASSERT(s2->bound_port == 0 && s2->bound_addr == 0,
                "the recycled slot carries NO stale bind -- if it did, this "
                "socket's listen() would announce 127.0.0.1!7789");
    TEST_ASSERT(s2->state == VIV_SOCK_FRESH, "and it is FRESH, not LISTENING");

    // has_room is the accept()-before-blocking check: it must go false exactly
    // when a claim would fail, or accept blocks, takes a real peer, and then
    // has to hang up on it.
    u32 claimed = 1;
    for (s32 fd = 200; claimed < VIV_SOCK_MAX; fd++) {
        if (!viv_socktab_claim(&tab, fd, VIV_NET_TCP, 1)) break;
        claimed++;
    }
    TEST_ASSERT(claimed == VIV_SOCK_MAX, "filled to VIV_SOCK_MAX");
    TEST_ASSERT(!viv_socktab_has_room(&tab), "a full table reports no room");
    TEST_ASSERT(viv_socktab_claim(&tab, 9999, VIV_NET_TCP, 1) == NULL,
                "and a claim on it does fail -- has_room agrees with claim");
}

// V-5b: the listen() decision table. Every arm is a REFUSAL a guest can
// provoke, so each gets its own POSIX code rather than a shared EINVAL.
void test_vivarium_listen_decide(void);
void test_vivarium_listen_decide(void) {
    s32 err = -1;

    TEST_ASSERT(vivarium_listen_decide(VIV_NET_TCP, VIV_SOCK_FRESH, 80, &err),
                "a bound fresh TCP socket may listen");
    TEST_ASSERT(err == 0, "and reports no error");

    // UDP has no listen file at all -- netd's walk rejects `listen` outside
    // /net/tcp, so this must never reach a walk.
    err = -1;
    TEST_ASSERT(!vivarium_listen_decide(VIV_NET_UDP, VIV_SOCK_FRESH, 80, &err),
                "a UDP socket may not listen");
    TEST_ASSERT(err == T_E_OPNOTSUPP, "and says EOPNOTSUPP, the POSIX code");

    // Port 0 means Linux would auto-bind an ephemeral port. netd's announce
    // parser rejects port 0, and inventing one would be a translation the
    // guest did not ask for -- so this DECLINES.
    err = -1;
    TEST_ASSERT(!vivarium_listen_decide(VIV_NET_TCP, VIV_SOCK_FRESH, 0, &err),
                "an unbound (port 0) socket may not listen");
    TEST_ASSERT(err == T_E_OPNOTSUPP, "declined, not mis-announced on port 0");

    err = -1;
    TEST_ASSERT(!vivarium_listen_decide(VIV_NET_TCP, VIV_SOCK_CONNECTED, 80, &err),
                "a connected socket may not listen");
    TEST_ASSERT(err == T_E_INVAL, "EINVAL, per POSIX");

    // A repeat listen() is a POSIX success, not an error: it may only adjust a
    // backlog netd owns. false + err 0 is the "already done" signal.
    err = -1;
    TEST_ASSERT(!vivarium_listen_decide(VIV_NET_TCP, VIV_SOCK_LISTENING, 80, &err),
                "a listening socket does not re-announce");
    TEST_ASSERT(err == 0, "but reports SUCCESS -- a repeat listen() is legal");

    TEST_ASSERT(!vivarium_listen_decide(VIV_NET_TCP, VIV_SOCK_FRESH, 80, NULL),
                "NULL out_err is refused, not dereferenced");
}

// V-5c: the ppoll timeout conversion. The rounding direction is the whole test
// -- truncating a sub-millisecond wait to 0 turns a poll into a spin, and a
// spin is the failure a poll loop would never notice and never stop paying for.
void test_vivarium_timespec_to_ms(void);
void test_vivarium_timespec_to_ms(void) {
    s32 ms = -99, err = -1;

    TEST_ASSERT(vivarium_timespec_to_ms(0, 0, &ms, &err), "zero timespec converts");
    TEST_ASSERT(ms == 0, "and is 0 ms -- a genuine non-blocking poll");

    TEST_ASSERT(vivarium_timespec_to_ms(2, 500000000, &ms, &err), "2.5s converts");
    TEST_ASSERT(ms == 2500, "to 2500 ms");

    // ROUNDS UP. 1 nanosecond is not zero, and 0 means "do not wait".
    TEST_ASSERT(vivarium_timespec_to_ms(0, 1, &ms, &err), "1ns converts");
    TEST_ASSERT(ms == 1, "UP to 1 ms -- never down to 0, which would spin");

    TEST_ASSERT(vivarium_timespec_to_ms(0, 999999, &ms, &err), "999999ns converts");
    TEST_ASSERT(ms == 1, "up to 1 ms");
    TEST_ASSERT(vivarium_timespec_to_ms(0, 1000000, &ms, &err), "exactly 1ms converts");
    TEST_ASSERT(ms == 1, "to 1 ms exactly -- the round-up adds nothing here");

    // Saturation, not overflow. The multiply is guarded BEFORE it happens.
    TEST_ASSERT(vivarium_timespec_to_ms(0x7FFFFFFFFFFF, 0, &ms, &err),
                "an absurd timeout converts rather than overflowing");
    TEST_ASSERT(ms == 0x7FFFFFFF, "saturated at INT32_MAX ms");

    // Linux's own validation.
    err = -1;
    TEST_ASSERT(!vivarium_timespec_to_ms(-1, 0, &ms, &err), "negative sec refused");
    TEST_ASSERT(err == T_E_INVAL, "as EINVAL");
    err = -1;
    TEST_ASSERT(!vivarium_timespec_to_ms(0, -1, &ms, &err), "negative nsec refused");
    TEST_ASSERT(err == T_E_INVAL, "as EINVAL");
    err = -1;
    TEST_ASSERT(!vivarium_timespec_to_ms(0, 1000000000, &ms, &err),
                "nsec >= 1e9 refused -- that is a malformed timespec, not 1s");
    TEST_ASSERT(err == T_E_INVAL, "as EINVAL");

    TEST_ASSERT(!vivarium_timespec_to_ms(0, 0, NULL, &err), "NULL out_ms refused");
    TEST_ASSERT(!vivarium_timespec_to_ms(0, 0, &ms, NULL), "NULL out_err refused");
}

// V-5c: the ppoll argument domain. Two of the three refusals are ENOSYS rather
// than EINVAL, and that distinction is the point -- the arguments are valid
// Linux, and it is this kernel that cannot serve them.
void test_vivarium_ppoll_decide(void);
void test_vivarium_ppoll_decide(void) {
    s32 err = -1;

    TEST_ASSERT(vivarium_ppoll_decide(1, 0, &err), "one fd, no sigmask, is served");
    TEST_ASSERT(err == 0, "with no error");
    TEST_ASSERT(vivarium_ppoll_decide(POLL_MAX_NFDS, 0, &err),
                "exactly POLL_MAX_NFDS fds is served -- the bound is inclusive");

    // ppoll's whole reason to exist over poll() is the ATOMIC mask swap. There
    // is no way to do that here, and doing it approximately would re-open the
    // race the caller chose ppoll to close.
    err = -1;
    TEST_ASSERT(!vivarium_ppoll_decide(1, 0x40000000, &err), "a sigmask is refused");
    TEST_ASSERT(err == T_E_NOSYS,
                "as ENOSYS -- the shape is unserved, the argument is not wrong");

    // Linux reads nfds == 0 as a timed sleep. There is no native sleep syscall
    // to route it to, and SYS_POLL rejects nfds == 0 outright.
    err = -1;
    TEST_ASSERT(!vivarium_ppoll_decide(0, 0, &err), "nfds == 0 is refused");
    TEST_ASSERT(err == T_E_NOSYS, "as ENOSYS -- it is a sleep, not a poll");

    // This one IS a bad value: the native cap is a real bound a caller must
    // respect, so it gets the POSIX code for a bad argument.
    err = -1;
    TEST_ASSERT(!vivarium_ppoll_decide(POLL_MAX_NFDS + 1, 0, &err),
                "over the native cap is refused");
    TEST_ASSERT(err == T_E_INVAL, "as EINVAL -- a real out-of-range argument");

    // Checked in the order a caller can act on: someone using ppoll FOR its
    // signal semantics must learn that is the unserved part.
    err = -1;
    TEST_ASSERT(!vivarium_ppoll_decide(0, 0x40000000, &err),
                "both wrong at once is refused");
    TEST_ASSERT(err == T_E_NOSYS, "reporting the sigmask, which is checked first");

    TEST_ASSERT(!vivarium_ppoll_decide(1, 0, NULL), "NULL out_err is refused");
}

// V-5b: the announce builder. The wildcard/concrete split is load-bearing --
// netd migrates an EXPLICIT 127.x listener onto its loopback stack while a `*`
// listener stays on the NIC, so these two reach different listeners.
void test_vivarium_announce_cmd(void);
void test_vivarium_announce_cmd(void) {
    char buf[48];

    const u8 any[4] = {0, 0, 0, 0};
    u32 n = vivarium_net_cmd_announce(buf, sizeof(buf), any, 7789);
    TEST_ASSERT(n == 15, "`announce *!7789` is 15 bytes: 8 verb + 1 space + 2 + 4 port");
    TEST_ASSERT(buf[0]=='a' && buf[9]=='*' && buf[10]=='!' && buf[11]=='7',
                "INADDR_ANY renders as the Plan 9 wildcard `announce *!7789`");

    const u8 lo[4] = {127, 0, 0, 1};
    n = vivarium_net_cmd_announce(buf, sizeof(buf), lo, 7789);
    TEST_ASSERT(n == 23, "announce 127.0.0.1!7789 is 23 bytes");
    TEST_ASSERT(buf[9]=='1' && buf[10]=='2' && buf[11]=='7' && buf[12]=='.',
                "a concrete address keeps its dotted quad -- rendering it as `*` "
                "would move the listener off netd's loopback stack");

    // Overflow is a refusal, never a truncated verb: a short write here would
    // announce a DIFFERENT port than the guest asked for.
    TEST_ASSERT(vivarium_net_cmd_announce(buf, 5, lo, 7789) == 0,
                "a buffer too small refuses");
    TEST_ASSERT(vivarium_net_cmd_announce(buf, 12, any, 7789) == 0,
                "including for the wildcard form");
    TEST_ASSERT(vivarium_net_cmd_announce(NULL, sizeof(buf), lo, 80) == 0,
                "NULL refused");
}

// V-5b: the endpoint parser -- the inverse of the ipport builder, reading
// netd's `remote` file for accept()'s peer address. A garbled endpoint must
// never become a plausible-looking address.
void test_vivarium_parse_ipport(void);
void test_vivarium_parse_ipport(void) {
    u8  ip[4];
    u16 port = 0;

    TEST_ASSERT(vivarium_parse_ipport("127.0.0.1!7789", 14, ip, &port), "parses");
    TEST_ASSERT(ip[0]==127 && ip[1]==0 && ip[2]==0 && ip[3]==1, "octets");
    TEST_ASSERT(port == 7789, "port");

    TEST_ASSERT(vivarium_parse_ipport("10.0.2.15!80\n", 13, ip, &port),
                "a trailing newline is line padding, not garbage");
    TEST_ASSERT(ip[0]==10 && ip[3]==15 && port == 80, "and parses correctly");

    TEST_ASSERT(vivarium_parse_ipport("0.0.0.0!0", 9, ip, &port),
                "the all-zero endpoint is well-formed");
    TEST_ASSERT(port == 0, "port 0 is legal HERE -- unlike a dial, this is a report");

    // Round-trip against the builder they must stay inverse to.
    char cmd[48];
    const u8 src[4] = {192, 168, 1, 200};
    u32 cn = vivarium_net_cmd_ipport(cmd, sizeof(cmd), "connect", src, 65535);
    TEST_ASSERT(cn > 8, "built");
    TEST_ASSERT(vivarium_parse_ipport(cmd + 8, cn - 8, ip, &port),
                "the builder's payload parses back");
    TEST_ASSERT(ip[0]==192 && ip[1]==168 && ip[2]==1 && ip[3]==200 && port==65535,
                "round-trip is exact");

    TEST_ASSERT(!vivarium_parse_ipport("127.0.0!80", 10, ip, &port), "3 octets refused");
    TEST_ASSERT(!vivarium_parse_ipport("127.0.0.1.2!80", 14, ip, &port), "5 octets refused");
    TEST_ASSERT(!vivarium_parse_ipport("256.0.0.1!80", 12, ip, &port), "a >255 octet refused");
    TEST_ASSERT(!vivarium_parse_ipport("127.0.0.1!70000", 15, ip, &port), "a >65535 port refused");
    TEST_ASSERT(!vivarium_parse_ipport("127.0.0.1", 9, ip, &port), "a missing port refused");
    TEST_ASSERT(!vivarium_parse_ipport("127.0.0.1!", 10, ip, &port), "an empty port refused");
    TEST_ASSERT(!vivarium_parse_ipport("127.0.0.1!80x", 13, ip, &port), "trailing garbage refused");
    TEST_ASSERT(!vivarium_parse_ipport("", 0, ip, &port), "empty refused");
    TEST_ASSERT(!vivarium_parse_ipport(NULL, 4, ip, &port), "NULL refused");
    TEST_ASSERT(!vivarium_parse_ipport("1234.0.0.1!80", 13, ip, &port),
                "a 4-digit octet refused before it can overflow");
}

// V-5b: the peer-address builder. accept() writes this into guest memory, so
// the byte layout is a Linux ABI, not an internal choice.
void test_vivarium_sockaddr_build(void);
void test_vivarium_sockaddr_build(void) {
    u8 sa[16];
    for (u32 i = 0; i < 16; i++) sa[i] = 0xAA;

    const u8 ip[4] = {10, 0, 2, 15};
    TEST_ASSERT(vivarium_sockaddr_in_build(sa, sizeof(sa), ip, 7789) == 16,
                "sockaddr_in is 16 bytes");
    TEST_ASSERT(sa[0] == 2 && sa[1] == 0, "family AF_INET, little-endian");
    TEST_ASSERT(sa[2] == (7789 >> 8) && sa[3] == (7789 & 0xFF),
                "port in NETWORK order");
    TEST_ASSERT(sa[4]==10 && sa[5]==0 && sa[6]==2 && sa[7]==15, "address octets");
    for (u32 i = 8; i < 16; i++)
        TEST_ASSERT(sa[i] == 0, "sin_zero is zeroed -- no stack bytes leak to EL0");

    TEST_ASSERT(vivarium_sockaddr_in_build(sa, 15, ip, 80) == 0, "a short buffer refuses");
    TEST_ASSERT(vivarium_sockaddr_in_build(NULL, 16, ip, 80) == 0, "NULL refused");

    // The parse is its inverse for the address half.
    u8  back[4];
    u16 bport = 0;
    TEST_ASSERT(vivarium_sockaddr_in_build(sa, sizeof(sa), ip, 7789) == 16, "rebuild");
    TEST_ASSERT(vivarium_sockaddr_in_parse(sa, 16, back, &bport), "parses back");
    TEST_ASSERT(back[0]==10 && back[3]==15 && bport==7789, "round-trip is exact");
}

// V-5b: bind()'s parse accepts what connect()'s refuses. 0.0.0.0:0 is an
// ordinary bind and a malformed dial, so the two callers need different
// strictness over the same bytes.
void test_vivarium_sockaddr_parse_any(void);
void test_vivarium_sockaddr_parse_any(void) {
    u8  sa[16] = {0};
    u8  ip[4];
    u16 port = 0xFFFF;

    sa[0] = 2; sa[1] = 0;                       // AF_INET
    sa[2] = 0; sa[3] = 0;                       // port 0
    sa[4] = 0; sa[5] = 0; sa[6] = 0; sa[7] = 0; // 0.0.0.0

    TEST_ASSERT(vivarium_sockaddr_in_parse_any(sa, 16, ip, &port),
                "bind(0.0.0.0:0) parses -- it is INADDR_ANY, not malformed");
    TEST_ASSERT(port == 0, "and reports port 0");
    TEST_ASSERT(!vivarium_sockaddr_in_parse(sa, 16, ip, &port),
                "while the STRICT parse refuses it -- a dial to port 0 is "
                "meaningless and netd rejects it");

    sa[2] = 0x1E; sa[3] = 0x61;                 // port 7777, network order
    TEST_ASSERT(vivarium_sockaddr_in_parse_any(sa, 16, ip, &port), "a real port parses");
    TEST_ASSERT(port == 7777, "network-order port decoded");
    TEST_ASSERT(vivarium_sockaddr_in_parse(sa, 16, ip, &port), "and the strict parse agrees");

    sa[0] = 10;                                  // AF_INET6
    TEST_ASSERT(!vivarium_sockaddr_in_parse_any(sa, 16, ip, &port),
                "AF_INET6 refused by BOTH -- v6 has no /net representation");
    sa[0] = 2;
    TEST_ASSERT(!vivarium_sockaddr_in_parse_any(sa, 7, ip, &port), "a short address refused");
    TEST_ASSERT(!vivarium_sockaddr_in_parse_any(NULL, 16, ip, &port), "NULL refused");
}
