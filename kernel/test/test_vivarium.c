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
//   vivarium.pselect6_decide        the domain + the PROC_HANDLE_MAX nfds clamp
//   vivarium.fdset_bytes            FDS_BYTES -- Linux's 8-byte-granular copy
//   vivarium.fdset_to_pollfds       3 fd_sets -> pollfds; exceptfds DECLINES
//   vivarium.pollfds_to_fdset       the asymmetric reverse map; a count of BITS
//   vivarium.fd_freeing_rows        close is the ONLY served fd-freeing call
//   vivarium.clone_domain           the exact vfork flags word; every widening
//
// The T2 tests (V-2b/V-2c/V-2d) stay just as pure: no decide function reads user
// memory by construction, and the stat conversion is data-in/data-out.

#include "test.h"

#include <thylacine/syscall.h>
#include <thylacine/types.h>
#include <thylacine/handle.h>       // V-5c-2: PROC_HANDLE_MAX, asserted BY NAME
#include <thylacine/notes.h>        // V-6b: the canonical note-name literals
#include <thylacine/proc.h>         // L-6b: WAIT_*, so the collision is asserted
                                    //       against the REAL constants, not copies
#include <thylacine/vivarium.h>

#include "../../mm/slub.h"          // #127: a real sigtab, so proc_free frees it

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
void test_vivarium_clone_domain(void);
void test_vivarium_wait4_domain(void);

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

    // pipe2 is T2 since #155. Asserted BY NAME next to its siblings for the
    // reason this whole function exists -- and with one extra job here: pipe2 is
    // an fd-CREATING row, which needs no socktab work at all, whereas the
    // fd-FREEING set two tests over must pay a drop of the entry keyed on the
    // number it frees. These two facts are easy to conflate. This assert and
    // that one, read together, say which side of the line each number is on.
    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_PIPE2, args, &out),
                   (int)VIV_TIER2, "pipe2 is T2 (#155; flags domain + an int[2] copy-out)");
    TEST_EXPECT_EQ((u64)out.nr, (u64)0xDEADu, "a T2 verdict leaves out untouched");

    // dup3 is T2 since #157 -- the first fd-FREEING row to be served, and the
    // reason the rule two tests over is now "pay the drop in the arm your
    // refusal structure demands" rather than "extend the close hook".
    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_DUP3, args, &out),
                   (int)VIV_TIER2, "dup3 is T2 (#157; aarch64 has no dup2 number)");
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

    // pselect6 is the sibling shape -- three fd_sets rather than a pollfd array.
    // V-5c-1 pinned it as a deliberate FORWARD "so promoting it is a decision
    // rather than a drive-by"; V-5c-2 is that decision, and this line changing
    // is what made it one.
    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_PSELECT6, args, &out),
                   (int)VIV_TIER2, "pselect6 is T2 (V-5c-2; the fd_set reshape)");

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

static void viv_expect_open_cx(u64 dirfd, u64 flags, u32 want_omode,
                               bool want_cloexec, const char *what) {
    u64  start_fd = 0xBADu;
    u32  omode    = 0xBADu;
    bool cloexec  = true;   // poison: every case below must OVERWRITE this

    TEST_EXPECT_EQ((int)vivarium_openat_decide(dirfd, flags, &start_fd, &omode,
                                               &cloexec),
                   (int)VIV_TRANSLATED, what);
    TEST_EXPECT_EQ(start_fd, SYS_WALK_OPEN_FROM_ROOT, "AT_FDCWD -> FROM_ROOT");
    TEST_EXPECT_EQ((u64)omode, (u64)want_omode, what);

    // #151: O_CLOEXEC's effect is here, NOT in the omode -- it names a property
    // of the resulting descriptor. Asserting it on every translated case is what
    // keeps a future flag admission from quietly turning the bit on or off.
    TEST_EXPECT_EQ((u64)(cloexec ? 1 : 0), (u64)(want_cloexec ? 1 : 0), what);

    // Whatever the map produces must be an omode SYS_OPEN will actually accept.
    // Asserting this for EVERY translated case (rather than eyeballing the
    // constants) is what makes a future flag admission safe to add.
    TEST_EXPECT_EQ((u64)(omode & ~SYS_WALK_OPEN_OMODE_VALID), (u64)0,
                   "the emitted omode is inside SYS_WALK_OPEN_OMODE_VALID");
}

static void viv_expect_open(u64 dirfd, u64 flags, u32 want_omode,
                            const char *what) {
    viv_expect_open_cx(dirfd, flags, want_omode, /*want_cloexec=*/false, what);
}

static void viv_expect_open_forwards(u64 flags, const char *what) {
    u64  start_fd = 0xBADu;
    u32  omode    = 0xBADu;
    bool cloexec  = true;

    TEST_EXPECT_EQ((int)vivarium_openat_decide(VIV_T_ATCWD, flags, &start_fd,
                                               &omode, &cloexec),
                   (int)VIV_FORWARD, what);
    // A declined call must leave the outputs alone: a caller that forwards but
    // reads them anyway must not find a plausible-looking omode waiting.
    TEST_EXPECT_EQ(start_fd, (u64)0xBADu, "a forwarded openat leaves start_fd alone");
    TEST_EXPECT_EQ((u64)omode, (u64)0xBADu, "a forwarded openat leaves omode alone");
    TEST_EXPECT_EQ((u64)(cloexec ? 1 : 0), (u64)1,
                   "a forwarded openat leaves cloexec alone");
}

void test_vivarium_openat_domain(void) {
    // The three access modes, and O_TRUNC composing with each.
    viv_expect_open(VIV_T_ATCWD, VIV_O_RDONLY, 0u, "O_RDONLY -> OREAD");
    viv_expect_open(VIV_T_ATCWD, VIV_O_WRONLY, 1u, "O_WRONLY -> OWRITE");
    viv_expect_open(VIV_T_ATCWD, VIV_O_RDWR,   2u, "O_RDWR -> ORDWR");
    viv_expect_open(VIV_T_ATCWD, VIV_O_WRONLY | VIV_O_TRUNC, 1u | 0x10u,
                    "O_WRONLY|O_TRUNC -> OWRITE|OTRUNC");

    // The two remaining no-op admissions. Each is admitted because Thylacine
    // already provides what the flag asks for unconditionally (see vivarium.c),
    // so the resulting omode must be IDENTICAL to the flag's absence.
    viv_expect_open(VIV_T_ATCWD, VIV_O_RDWR | VIV_O_NOCTTY, 2u,
                    "O_NOCTTY is a no-op (ct acquisition is explicit)");
    viv_expect_open(VIV_T_ATCWD, VIV_O_RDONLY | VIV_O_LARGEFILE, 0u,
                    "O_LARGEFILE is a no-op (all offsets are 64-bit)");

    // #151: O_CLOEXEC is NO LONGER a no-op. It was one, on a rationale LINEAGE
    // voided (execve now preserves the handle table), so this case moved from
    // "the omode is unchanged" -- which is still true, and still asserted -- to
    // "and the descriptor flag comes out set".
    viv_expect_open_cx(VIV_T_ATCWD, VIV_O_RDONLY | VIV_O_CLOEXEC, 0u, true,
                       "O_CLOEXEC sets the descriptor flag, not the omode");
    viv_expect_open_cx(VIV_T_ATCWD, VIV_O_RDONLY, 0u, false,
                       "no O_CLOEXEC leaves the descriptor flag clear");

    // O_PATH dominates: the access bits and O_TRUNC are ignored on BOTH sides,
    // so the emitted omode is the bare OPATH rather than OPATH|whatever.
    viv_expect_open(VIV_T_ATCWD, VIV_O_PATH, SYS_WALK_OPEN_OPATH,
                    "O_PATH -> OPATH");
    viv_expect_open(VIV_T_ATCWD, VIV_O_PATH | VIV_O_RDWR | VIV_O_TRUNC,
                    SYS_WALK_OPEN_OPATH,
                    "O_PATH ignores access bits + O_TRUNC, as Linux does");
    // O_PATH does NOT swallow O_CLOEXEC: Linux honours it on an O_PATH open (it
    // is one of the three flags O_PATH does not ignore), and an O_PATH open
    // produces a descriptor like any other.
    viv_expect_open_cx(VIV_T_ATCWD, VIV_O_PATH | VIV_O_CLOEXEC,
                       SYS_WALK_OPEN_OPATH, true,
                       "O_PATH|O_CLOEXEC keeps the descriptor flag");

    // The rejects. Each of these, if silently ignored, is a WRONG ANSWER rather
    // than a harmless no-op -- that asymmetry is the whole admission rule, so
    // each is pinned by name.
    viv_expect_open_forwards(VIV_O_WRONLY | VIV_O_CREAT,
                             "O_CREAT forwards (SYS_OPEN cannot create)");
    viv_expect_open_forwards(VIV_O_RDONLY | VIV_O_DIRECTORY,
                             "O_DIRECTORY forwards (no is-a-dir check to honour)");
    viv_expect_open_forwards(VIV_O_WRONLY | VIV_O_APPEND,
                             "O_APPEND forwards (no append mode in omode)");
    // D-1: the V-2b reject INVERTED on the day its own rationale predicted --
    // symlinks landed, so O_NOFOLLOW now TRANSLATES to the resolver's
    // no-follow omode bit (real semantics: ELOOP on a final link; with O_PATH
    // the handle IS the link).
    viv_expect_open(VIV_T_ATCWD, VIV_O_RDONLY | VIV_O_NOFOLLOW,
                    0u | SYS_WALK_OPEN_NOFOLLOW,
                    "O_NOFOLLOW translates to the no-follow omode bit (D-1)");
    viv_expect_open(VIV_T_ATCWD, VIV_O_PATH | VIV_O_NOFOLLOW,
                    SYS_WALK_OPEN_OPATH | SYS_WALK_OPEN_NOFOLLOW,
                    "O_PATH|O_NOFOLLOW keeps the flag (the lstat-fd idiom; "
                    "one of the three flags O_PATH does not ignore)");
    viv_expect_open_forwards(VIV_O_RDONLY | VIV_O_NONBLOCK,
                             "O_NONBLOCK forwards");
    viv_expect_open_forwards(VIV_O_WRONLY | VIV_O_EXCL, "O_EXCL forwards");

    // (flags & O_ACCMODE) == 3 is EINVAL on Linux. We forward rather than mint
    // the error ourselves -- section 4 forbids the table inventing error semantics.
    viv_expect_open_forwards(VIV_O_ACCMODE,
                             "accmode 3 forwards (Linux EINVAL; not ours to mint)");

    // Fail toward the supervisor on a bad call site, never toward a dispatch.
    // Each output is nulled in turn, not just all three at once: a guard that
    // checked only the first two would pass an all-NULL test and then write
    // through a NULL cloexec_out for a caller that supplied the other two.
    u64  s = 0; u32 o = 0; bool cx = false;
    TEST_EXPECT_EQ((int)vivarium_openat_decide(VIV_T_ATCWD, 0, NULL, NULL, NULL),
                   (int)VIV_FORWARD, "NULL outputs -> FORWARD, never TRANSLATED");
    TEST_EXPECT_EQ((int)vivarium_openat_decide(VIV_T_ATCWD, 0, NULL, &o, &cx),
                   (int)VIV_FORWARD, "NULL start_fd_out alone -> FORWARD");
    TEST_EXPECT_EQ((int)vivarium_openat_decide(VIV_T_ATCWD, 0, &s, NULL, &cx),
                   (int)VIV_FORWARD, "NULL omode_out alone -> FORWARD");
    TEST_EXPECT_EQ((int)vivarium_openat_decide(VIV_T_ATCWD, 0, &s, &o, NULL),
                   (int)VIV_FORWARD, "NULL cloexec_out alone -> FORWARD");
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
    u64  start_fd = 0xBADu;
    u32  omode    = 0xBADu;
    bool cloexec  = false;
    TEST_EXPECT_EQ((int)vivarium_openat_decide(3, VIV_O_RDONLY, &start_fd, &omode,
                                               &cloexec),
                   (int)VIV_FORWARD, "a real dirfd forwards (handle state, not a gap)");

    // Only the LOW 32 BITS are significant -- `dirfd` is an `int`. A high-half
    // value that is not AT_FDCWD in its low word must not be mistaken for one.
    TEST_EXPECT_EQ((int)vivarium_openat_decide(0x1234567800000003ull, VIV_O_RDONLY,
                                               &start_fd, &omode, &cloexec),
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
    u32 sf = 0xFFu;   // poison: a translate must WRITE it (0 or the lstat mark)
    TEST_EXPECT_EQ((int)vivarium_fstatat_decide(dirfd, flags, &sf),
                   (int)VIV_TRANSLATED, what);
}

static void viv_expect_statat_forwards(u64 flags, const char *what) {
    u32 sf = 0;
    TEST_EXPECT_EQ((int)vivarium_fstatat_decide(VIV_T_ATCWD, flags, &sf),
                   (int)VIV_FORWARD, what);
}

void test_vivarium_fstatat_domain(void) {
    // Plain stat() -- flags 0. This is the row that carries the value: on
    // aarch64, stat() compiles to newfstatat, not to a stat(2) of its own.
    viv_expect_statat(VIV_T_ATCWD, 0, "flags 0 (plain stat) translates");
    {
        u32 sf = 0xFFu;
        TEST_EXPECT_EQ((int)vivarium_fstatat_decide(VIV_T_ATCWD, 0, &sf),
                       (int)VIV_TRANSLATED, "plain stat translates (sf probe)");
        TEST_EXPECT_EQ((u64)sf, (u64)0, "plain stat follows (sf == 0)");
    }

    // The one no-op admission. A Thylacine namespace is composed explicitly, so
    // nothing mounts as a side effect of traversal -- the flag asks for what we
    // do unconditionally, and by construction of the model rather than by a
    // feature being unbuilt.
    viv_expect_statat(VIV_T_ATCWD, VIV_AT_NO_AUTOMOUNT,
                      "AT_NO_AUTOMOUNT is a no-op (nothing mounts on traversal)");

    // D-1: the V-2c reject INVERTED, on the day its own comment predicted
    // ("the day symlinks land there is nothing ... that would fail" -- this
    // leg is the something). lstat now TRANSLATES, with the out-param marking
    // the no-follow shape for sys_stat_for_proc.
    {
        u32 sf = 0;
        TEST_EXPECT_EQ((int)vivarium_fstatat_decide(VIV_T_ATCWD,
                                                    VIV_AT_SYMLINK_NOFOLLOW, &sf),
                       (int)VIV_TRANSLATED,
                       "AT_SYMLINK_NOFOLLOW translates (lstat; D-1)");
        TEST_EXPECT_EQ((u64)(sf != 0), (u64)1, "lstat marks the no-follow shape");
    }
    {
        // Both admitted bits together translate (the pre-D-1 leg asserted the
        // combination FORWARDED; the mask grew, the whole-word check stands).
        u32 sf = 0;
        TEST_EXPECT_EQ((int)vivarium_fstatat_decide(
                           VIV_T_ATCWD,
                           VIV_AT_NO_AUTOMOUNT | VIV_AT_SYMLINK_NOFOLLOW, &sf),
                       (int)VIV_TRANSLATED,
                       "NO_AUTOMOUNT + SYMLINK_NOFOLLOW both admitted");
        TEST_EXPECT_EQ((u64)(sf != 0), (u64)1, "the combination keeps the lstat mark");
    }

    // Fail toward the decline on a bad call site (the openat shape).
    TEST_EXPECT_EQ((int)vivarium_fstatat_decide(VIV_T_ATCWD, 0, NULL),
                   (int)VIV_FORWARD, "NULL out-param forwards, never dispatches");

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
    viv_expect_statat_forwards(VIV_AT_EMPTY_PATH | VIV_AT_SYMLINK_NOFOLLOW,
                               "a rejected bit forwards even beside an admitted one");
    viv_expect_statat_forwards(0x80000000u, "an unknown high bit forwards");

    // Both AT_FDCWD encodings, for the openat sign-extension reason.
    viv_expect_statat((u64)(s64)-100, 0, "AT_FDCWD sign-extended is recognised");
    viv_expect_statat((u64)0xFFFFFF9Cu, 0, "AT_FDCWD zero-extended is recognised");

    // A real dirfd forwards -- and here that is STRUCTURAL, not a v1.0 limit:
    // SYS_STAT takes (path, len, out) and has no base argument at all, so there
    // is nowhere for a dirfd to go. Contrast openat, which at least HAS a
    // start_fd it could carry.
    {
        u32 sf = 0;
        TEST_EXPECT_EQ((int)vivarium_fstatat_decide(3, 0, &sf), (int)VIV_FORWARD,
                       "a real dirfd forwards (SYS_STAT has no base argument)");

        // Only the low 32 bits of dirfd are significant.
        TEST_EXPECT_EQ((int)vivarium_fstatat_decide(0x1234567800000003ull, 0, &sf),
                       (int)VIV_FORWARD, "the high half of dirfd is not consulted");
    }
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

// The FILE mmap argument domain (DISTRO D-3). Measured off musl's map_library,
// so the admitted case is asserted as THE CALL STOCK LDSO MAKES rather than as a
// shape we thought reasonable -- and each decline is named, because this arm
// hands a guest a mapping of a FILE and a widening here is an I-36 question, not
// a fidelity one.
void test_vivarium_mmap_file_domain(void) {
    const u64 priv = (u64)VIV_MAP_PRIVATE;
    const u64 rx   = (u64)(VIV_PROT_READ | VIV_PROT_EXEC);
    const u64 fd   = 3;

    // THE MEASURED CALL. dynlink.c:809 `mmap(addr_min, map_len, prot,
    // MAP_PRIVATE, fd, off_start)`, read against the shipped Alpine libc whose
    // lowest PT_LOAD is R+X at file offset 0: this exact tuple is what opening
    // /lib/ld-musl-aarch64.so.1 produces.
    TEST_EXPECT_EQ((int)vivarium_mmap_file_decide(rx, priv, fd, 0),
                   (int)VIV_TRANSLATED, "R+X private fd-backed is the measured shape");

    // R-only: the same call against a library whose lowest PT_LOAD is R-only
    // (a -z separate-code layout). Not hypothetical -- it is the OTHER shape the
    // same line emits, decided by the linker rather than by the caller.
    TEST_EXPECT_EQ((int)vivarium_mmap_file_decide((u64)VIV_PROT_READ, priv, fd, 0),
                   (int)VIV_TRANSLATED, "R-only private fd-backed admitted too");

    // PROT_WRITE is THE refusal that keeps I-36 intact: this arm's Burrow is
    // shared and has no write-back path, so a writable file mapping would either
    // lose the writes or leak them into every other Proc sharing the Image.
    TEST_EXPECT_EQ((int)vivarium_mmap_file_decide(
                       (u64)(VIV_PROT_READ | VIV_PROT_WRITE), priv, fd, 0),
                   (int)VIV_FORWARD, "PROT_WRITE declines -- I-36 has no write-back");
    TEST_EXPECT_EQ((int)vivarium_mmap_file_decide(
                       (u64)(VIV_PROT_READ | VIV_PROT_WRITE | VIV_PROT_EXEC),
                       priv, fd, 0),
                   (int)VIV_FORWARD, "W+X declines (I-12 too, two independent gates)");

    // PROT_NONE declines HERE while the anon arm admits it -- the asymmetry is
    // deliberate and is asserted so a future "make the arms consistent" tidy-up
    // has to argue with a test. There is no degradation available: serving a
    // PROT_NONE file map readably would hand over bytes the caller declined.
    TEST_EXPECT_EQ((int)vivarium_mmap_file_decide((u64)VIV_PROT_NONE, priv, fd, 0),
                   (int)VIV_FORWARD, "PROT_NONE declines on the FILE arm");
    TEST_EXPECT_EQ((int)vivarium_mmap_file_decide((u64)VIV_PROT_EXEC, priv, fd, 0),
                   (int)VIV_FORWARD, "X-without-R declines (no readable page to exec)");

    // An allow-list, so the aarch64 extras fall out without being enumerated.
    TEST_EXPECT_EQ((int)vivarium_mmap_file_decide(rx | (u64)VIV_PROT_BTI, priv, fd, 0),
                   (int)VIV_FORWARD, "PROT_BTI declines");
    TEST_EXPECT_EQ((int)vivarium_mmap_file_decide(rx | (u64)VIV_PROT_MTE, priv, fd, 0),
                   (int)VIV_FORWARD, "PROT_MTE declines");

    // MAP_SHARED is the write-back semantics arriving by a second door, and
    // MAP_FIXED is the caller-chosen address D-3a does not serve (D-3b does,
    // through its own arm). Exact equality refuses both without naming them.
    TEST_EXPECT_EQ((int)vivarium_mmap_file_decide(rx, (u64)VIV_MAP_SHARED, fd, 0),
                   (int)VIV_FORWARD, "MAP_SHARED declines -- write-back by another name");
    TEST_EXPECT_EQ((int)vivarium_mmap_file_decide(
                       rx, priv | (u64)VIV_MAP_FIXED, fd, 0),
                   (int)VIV_FORWARD, "MAP_FIXED declines on this arm");
    TEST_EXPECT_EQ((int)vivarium_mmap_file_decide(
                       rx, priv | (u64)VIV_MAP_FIXED_NOREPLACE, fd, 0),
                   (int)VIV_FORWARD, "MAP_FIXED_NOREPLACE declines");
    TEST_EXPECT_EQ((int)vivarium_mmap_file_decide(
                       rx, priv | (u64)VIV_MAP_ANONYMOUS, fd, 0),
                   (int)VIV_FORWARD, "MAP_ANONYMOUS declines -- that is the other arm");

    // BOTH spellings of -1, the AT_FDCWD trap again: a caller may leave x4
    // sign-extended or merely zero-extended, and neither is a real descriptor.
    TEST_EXPECT_EQ((int)vivarium_mmap_file_decide(rx, priv, (u64)-1, 0),
                   (int)VIV_FORWARD, "sign-extended -1 fd declines");
    TEST_EXPECT_EQ((int)vivarium_mmap_file_decide(rx, priv, 0xFFFFFFFFull, 0),
                   (int)VIV_FORWARD, "zero-extended -1 fd declines");

    // A page-aligned offset is STRUCTURE, not fidelity: the FILE fault arm reads
    // each page at file_offset + page-floored burrow offset, an identity that
    // only holds when the Burrow's offset 0 is the mapping's start.
    TEST_EXPECT_EQ((int)vivarium_mmap_file_decide(rx, priv, fd, 0x1000),
                   (int)VIV_TRANSLATED, "a page-aligned non-zero offset is admitted");
    TEST_EXPECT_EQ((int)vivarium_mmap_file_decide(rx, priv, fd, 1),
                   (int)VIV_FORWARD, "a misaligned offset declines");
    TEST_EXPECT_EQ((int)vivarium_mmap_file_decide(rx, priv, fd, 0xFFF),
                   (int)VIV_FORWARD, "one byte short of a page declines");

    // prot/flags/fd are `int` in the Linux ABI, so their high halves are noise.
    TEST_EXPECT_EQ((int)vivarium_mmap_file_decide(0xDEADBEEF00000000ull | rx,
                                                  0xCAFE000000000000ull | priv,
                                                  0x1234567800000003ull, 0),
                   (int)VIV_TRANSLATED, "the high half of prot/flags/fd is unread");
}

// The two MAP_FIXED argument domains (DISTRO D-3b). Measured off map_library's
// overlay calls, so each admitted case is asserted as THE CALL STOCK LDSO MAKES.
void test_vivarium_mmap_fixed_domain(void) {
    const u64 pf   = (u64)(VIV_MAP_PRIVATE | VIV_MAP_FIXED);
    const u64 pfa  = (u64)(VIV_MAP_PRIVATE | VIV_MAP_FIXED | VIV_MAP_ANONYMOUS);
    const u64 rw   = (u64)(VIV_PROT_READ | VIV_PROT_WRITE);
    const u64 rx   = (u64)(VIV_PROT_READ | VIV_PROT_EXEC);
    const u64 addr = 0x40001000ull;                  // a real page
    const u64 fd   = 3;

    // ---- arm 2 (dynlink.c:842) -----------------------------------------------

    // THE MEASURED CALL. All 18 ELFs in the stock Alpine rootfs are `R-X` then
    // `RW-`, so the ONE arm-2 request any of them makes is this: the writable
    // data segment, page-aligned file offset, MAP_PRIVATE|MAP_FIXED.
    TEST_EXPECT_EQ((int)vivarium_mmap_fixed_file_decide(addr, rw, pf, fd, 0x1000),
                   (int)VIV_TRANSLATED, "RW private fixed fd-backed is the measured shape");

    // R+X and R-only arm-2 requests. NO PRODUCER on this rootfs -- they need a
    // `-z separate-code` four-segment layout. Admitted because the ELF format
    // permits them and a Debian/Fedora toolchain emits them; asserted HERE
    // because the in-guest gate cannot reach them, so this is their only cover.
    TEST_EXPECT_EQ((int)vivarium_mmap_fixed_file_decide(addr, rx, pf, fd, 0),
                   (int)VIV_TRANSLATED, "R+X fixed fd-backed admitted (separate-code layout)");
    TEST_EXPECT_EQ((int)vivarium_mmap_fixed_file_decide(addr, (u64)VIV_PROT_READ,
                                                        pf, fd, 0),
                   (int)VIV_TRANSLATED, "R-only fixed fd-backed admitted");

    // W+X declines at the domain edge. vma_alloc would reject it too, but I-12
    // is worth failing closed on before anything has to argue about it.
    TEST_EXPECT_EQ((int)vivarium_mmap_fixed_file_decide(
                       addr, (u64)(VIV_PROT_READ | VIV_PROT_WRITE | VIV_PROT_EXEC),
                       pf, fd, 0),
                   (int)VIV_FORWARD, "W+X declines -- I-12 fails closed here too");

    // PROT_NONE is a pure reservation; there is no honourable service for it.
    TEST_EXPECT_EQ((int)vivarium_mmap_fixed_file_decide(addr, 0, pf, fd, 0),
                   (int)VIV_FORWARD, "PROT_NONE declines");

    // addr is a REQUIREMENT under MAP_FIXED, not the hint it is on the non-fixed
    // arms. Zero and misaligned are separate mistakes and both must decline.
    TEST_EXPECT_EQ((int)vivarium_mmap_fixed_file_decide(0, rw, pf, fd, 0),
                   (int)VIV_FORWARD, "a fixed map at NULL declines");
    TEST_EXPECT_EQ((int)vivarium_mmap_fixed_file_decide(addr + 1, rw, pf, fd, 0),
                   (int)VIV_FORWARD, "a misaligned fixed addr declines");

    // MAP_SHARED is the write-back semantics this whole arc refuses, arriving by
    // another door. Exact flag equality is what excludes it.
    TEST_EXPECT_EQ((int)vivarium_mmap_fixed_file_decide(
                       addr, rw, (u64)(VIV_MAP_SHARED | VIV_MAP_FIXED), fd, 0),
                   (int)VIV_FORWARD, "MAP_SHARED declines");
    TEST_EXPECT_EQ((int)vivarium_mmap_fixed_file_decide(
                       addr, rw, pf | (u64)VIV_MAP_ANONYMOUS, fd, 0),
                   (int)VIV_FORWARD, "MAP_ANONYMOUS is the OTHER arm, not this one");

    TEST_EXPECT_EQ((int)vivarium_mmap_fixed_file_decide(addr, rw, pf, (u64)-1, 0),
                   (int)VIV_FORWARD, "fd == -1 declines on the fd-backed arm");
    TEST_EXPECT_EQ((int)vivarium_mmap_fixed_file_decide(addr, rw, pf, fd, 1),
                   (int)VIV_FORWARD, "a misaligned offset declines");

    // ---- arm 3 (dynlink.c:851) -----------------------------------------------

    // THE MEASURED CALL: the bss tail, gated in musl on memsz > filesz && PF_W,
    // so its prot always carries R|W and its offset is always 0.
    TEST_EXPECT_EQ((int)vivarium_mmap_fixed_anon_decide(addr, rw, pfa, (u64)-1, 0),
                   (int)VIV_TRANSLATED, "RW private fixed anon is the measured shape");

    // PROT_NONE declines HERE, and that DIVERGES from the non-fixed anon arm,
    // which degrades it to writable. A fixed PROT_NONE over an existing mapping
    // is a guard; answering it with a writable page is a hole, not a degradation.
    TEST_EXPECT_EQ((int)vivarium_mmap_decide(addr, 0, (u64)(VIV_MAP_PRIVATE |
                                                            VIV_MAP_ANONYMOUS),
                                             (u64)-1, 0),
                   (int)VIV_TRANSLATED, "the NON-fixed anon arm does admit PROT_NONE");
    TEST_EXPECT_EQ((int)vivarium_mmap_fixed_anon_decide(addr, 0, pfa, (u64)-1, 0),
                   (int)VIV_FORWARD, "but the FIXED anon arm refuses it -- a guard");

    TEST_EXPECT_EQ((int)vivarium_mmap_fixed_anon_decide(addr, rx, pfa, (u64)-1, 0),
                   (int)VIV_FORWARD, "PROT_EXEC declines on the anon arm (I-42/CAP_JIT)");
    TEST_EXPECT_EQ((int)vivarium_mmap_fixed_anon_decide(addr, rw, pfa, 3, 0),
                   (int)VIV_FORWARD, "a real fd declines on the anonymous arm");
    TEST_EXPECT_EQ((int)vivarium_mmap_fixed_anon_decide(addr, rw, pfa, (u64)-1, 0x1000),
                   (int)VIV_FORWARD, "a nonzero offset declines on the anonymous arm");
    TEST_EXPECT_EQ((int)vivarium_mmap_fixed_anon_decide(0, rw, pfa, (u64)-1, 0),
                   (int)VIV_FORWARD, "a fixed anon map at NULL declines");
}

// The FOUR mmap arms are pairwise DISJOINT. Every decider's comment claims it;
// without this they would only agree with each other. The dispatch site relies
// on it to make its try-order free rather than load-bearing -- if some tuple
// were admitted by two arms, the order would silently decide which semantics a
// guest receives, and D-3b's arms differ in whether the guest gets shared
// demand-paged file bytes or a private eager copy.
void test_vivarium_mmap_arms_disjoint(void) {
    // Sweep the cross product of every prot/flag/fd/offset value either arm
    // reasons about. Cheap and exhaustive over the interesting space -- far more
    // convincing than a handful of hand-picked tuples, and it is the shape that
    // would catch a FUTURE widening of either arm colliding with the other.
    const u64 prots[] = {
        0, VIV_PROT_READ, VIV_PROT_WRITE, VIV_PROT_EXEC,
        VIV_PROT_READ | VIV_PROT_WRITE, VIV_PROT_READ | VIV_PROT_EXEC,
        VIV_PROT_WRITE | VIV_PROT_EXEC,
        VIV_PROT_READ | VIV_PROT_WRITE | VIV_PROT_EXEC,
        VIV_PROT_BTI, VIV_PROT_MTE,
    };
    const u64 flags[] = {
        0, VIV_MAP_SHARED, VIV_MAP_PRIVATE, VIV_MAP_FIXED, VIV_MAP_ANONYMOUS,
        VIV_MAP_PRIVATE | VIV_MAP_ANONYMOUS,
        VIV_MAP_PRIVATE | VIV_MAP_FIXED,
        VIV_MAP_PRIVATE | VIV_MAP_ANONYMOUS | VIV_MAP_FIXED,
        VIV_MAP_PRIVATE | VIV_MAP_FIXED_NOREPLACE,
    };
    const u64 fds[]  = { 0, 3, (u64)-1, 0xFFFFFFFFull };
    const u64 offs[] = { 0, 1, 0x1000 };
    // The FIXED arms judge `addr`, so it joins the sweep: 0 and a misaligned
    // value must reach the two decliners, and a real page must reach admission.
    const u64 addrs[] = { 0, 0x1000, 0x1001, 0x40000000ull };

    int adm_file = 0, adm_anon = 0, adm_fixed_file = 0, adm_fixed_anon = 0;
    for (unsigned ai = 0; ai < sizeof(addrs) / sizeof(addrs[0]); ai++)
    for (unsigned pi = 0; pi < sizeof(prots) / sizeof(prots[0]); pi++)
    for (unsigned fi = 0; fi < sizeof(flags) / sizeof(flags[0]); fi++)
    for (unsigned di = 0; di < sizeof(fds)   / sizeof(fds[0]);   di++)
    for (unsigned oi = 0; oi < sizeof(offs)  / sizeof(offs[0]);  oi++) {
        u64 a = addrs[ai], pr = prots[pi], fl = flags[fi];
        u64 fd = fds[di],  off = offs[oi];
        TEST_ASSERT(vivarium_mmap_arms_disjoint(a, pr, fl, fd, off),
                    "no tuple may be admitted by more than ONE mmap arm");
        if (vivarium_mmap_file_decide(pr, fl, fd, off) == VIV_TRANSLATED)
            adm_file++;
        if (vivarium_mmap_decide(a, pr, fl, fd, off) == VIV_TRANSLATED)
            adm_anon++;
        if (vivarium_mmap_fixed_file_decide(a, pr, fl, fd, off) == VIV_TRANSLATED)
            adm_fixed_file++;
        if (vivarium_mmap_fixed_anon_decide(a, pr, fl, fd, off) == VIV_TRANSLATED)
            adm_fixed_anon++;
    }

    // Disjointness is satisfiable by admitting NOTHING, so the sweep proves
    // nothing until EVERY arm is shown to admit something inside it. These are
    // the checks that keep the loop above from passing vacuously -- and they are
    // per-arm rather than a total, because a total is satisfied by one arm
    // admitting everything while another admits nothing at all.
    TEST_ASSERT(adm_file       > 0, "the sweep must reach the FILE arm's domain");
    TEST_ASSERT(adm_anon       > 0, "the sweep must reach the anon arm's domain");
    TEST_ASSERT(adm_fixed_file > 0, "the sweep must reach the FIXED FILE domain");
    TEST_ASSERT(adm_fixed_anon > 0, "the sweep must reach the FIXED anon domain");
}

// The clone argument domain (LINEAGE L-3d). This row hands a guest a second
// PROCESS, so every decline is asserted by NAME and by REASON -- three distinct
// classes of widening are being refused here and a mask test would admit all of
// them at once.
void test_vivarium_clone_domain(void) {
    // A plausible stack: nonzero, 16-aligned, in the user half. The decide does
    // not check alignment or range (SYS_RFORK's own gate does, and re-stating
    // it here would be a second copy of a rule that must not drift), so this is
    // just "a stack a caller might pass".
    const u64 sp = 0x0000004000010000ull;

    // POISONED between uses. `share_mem` is only meaningful on VIV_TRANSLATED,
    // so seeding it with the WRONG value before each admitted call is what makes
    // the assertions below real rather than a reading of a leftover.
    bool sm;

    // The shape musl's posix_spawn sends. posix_spawn.c:198 --
    //   __clone(child, stack+sizeof stack, CLONE_VM|CLONE_VFORK|SIGCHLD, &args)
    const u64 ok = (u64)(VIV_CLONE_VM | VIV_CLONE_VFORK | VIV_CLONE_SIGCHLD);
    sm = false;
    TEST_EXPECT_EQ((int)vivarium_clone_decide(ok, sp, &sm),
                   (int)VIV_TRANSLATED, "CLONE_VM|CLONE_VFORK|SIGCHLD is the admitted shape");
    TEST_ASSERT(sm, "the vfork shape SHARES the address space (RFMEM)");

    // THE FORK SHAPE (L-6a). musl's fork() -> _Fork() emits exactly
    // clone(SIGCHLD, 0), and L-4/L-5 built the private copy-on-write address
    // space its child needs. Two separate claims, because a widening that got
    // the second one wrong would hand a fork child a SHARED address space and
    // suspend its parent -- and the verdict alone cannot tell those apart.
    const u64 forkw = (u64)VIV_CLONE_FLAGS_FORK;
    sm = true;
    TEST_EXPECT_EQ((int)vivarium_clone_decide(forkw, 0, &sm),
                   (int)VIV_TRANSLATED, "clone(SIGCHLD, 0) is fork() -- admitted since L-6a");
    TEST_ASSERT(!sm, "the fork shape COPIES the address space (RFPROC alone)");

    // The `stack` rule INVERTS between the shapes, so both arms are pinned:
    // zero is REQUIRED to decline under RFMEM (below) and REQUIRED to be served
    // here, because under RFPROC alone zero means INHERIT and that is what
    // fork() means. A single shared rule would have to be wrong for one of them.
    sm = true;
    TEST_EXPECT_EQ((int)vivarium_clone_decide(forkw, sp, &sm),
                   (int)VIV_TRANSLATED, "fork with an explicit stack is still fork");
    TEST_ASSERT(!sm, "an explicit stack does not make it a vfork");

    // THE DECISION THIS CHUNK HAD TO MAKE, and the reason it is not the same as
    // L-3c-2's. A caller that sets CLONE_VM and CLEARS CLONE_VFORK has said "do
    // not suspend me" in the only vocabulary Linux gives it. Serving it anyway
    // -- which the kernel primitive would happily do, since the suspend is keyed
    // on RFMEM -- turns a working program into a deadlock the moment the child
    // neither execs nor exits promptly. So it declines, and the guest gets an
    // answer it can act on instead of a hang with our name on it.
    TEST_EXPECT_EQ((int)vivarium_clone_decide(
                       (u64)(VIV_CLONE_VM | VIV_CLONE_SIGCHLD), sp, &sm),
                   (int)VIV_FORWARD,
                   "CLONE_VM without CLONE_VFORK declines -- never a suspend unasked");

    // The thread set. A genuinely concurrent child has a correct target
    // already -- SYS_THREAD_SPAWN -- and it is not this row.
    TEST_EXPECT_EQ((int)vivarium_clone_decide(
                       ok | (u64)VIV_CLONE_THREAD, sp, &sm),
                   (int)VIV_FORWARD, "CLONE_THREAD declines -- that is SYS_THREAD_SPAWN");
    TEST_EXPECT_EQ((int)vivarium_clone_decide(
                       ok | (u64)VIV_CLONE_FILES, sp, &sm),
                   (int)VIV_FORWARD, "CLONE_FILES declines -- the table is COPIED, not shared");

    // The fork word is EXACT too -- L-6a widened the domain by one word, not
    // into a mask. CLONE_FILES on top of it would share the handle table, which
    // is a different Plan 9 flag (RFFDG) and still refused.
    TEST_EXPECT_EQ((int)vivarium_clone_decide(
                       forkw | (u64)VIV_CLONE_FILES, 0, &sm),
                   (int)VIV_FORWARD, "fork|CLONE_FILES declines -- the fork word is exact");
    TEST_EXPECT_EQ((int)vivarium_clone_decide(
                       forkw | (u64)VIV_CLONE_SETTLS, 0, &sm),
                   (int)VIV_FORWARD, "fork|CLONE_SETTLS declines -- x3 is not read either");

    // THE GARBAGE-REGISTER GUARD, asserted from the domain side. Each of these
    // three bits makes one of x2/x3/x4 MEANINGFUL, and the shell's safety
    // argument is that it never reads them: it passes a literal 0 for
    // child_tls and ignores parent_tid/child_tid entirely. Admitting any of
    // these would silently make that a lie -- musl's __clone leaves all three
    // registers holding whatever posix_spawn's caller happened to have there.
    TEST_EXPECT_EQ((int)vivarium_clone_decide(ok | (u64)VIV_CLONE_SETTLS, sp, &sm),
                   (int)VIV_FORWARD, "CLONE_SETTLS declines -- x3 is not read");
    TEST_EXPECT_EQ((int)vivarium_clone_decide(ok | (u64)VIV_CLONE_PARENT_SETTID, sp, &sm),
                   (int)VIV_FORWARD, "CLONE_PARENT_SETTID declines -- x2 is not read");
    TEST_EXPECT_EQ((int)vivarium_clone_decide(ok | (u64)VIV_CLONE_CHILD_SETTID, sp, &sm),
                   (int)VIV_FORWARD, "CLONE_CHILD_SETTID declines -- x4 is not read");
    TEST_EXPECT_EQ((int)vivarium_clone_decide(ok | (u64)VIV_CLONE_CHILD_CLEARTID, sp, &sm),
                   (int)VIV_FORWARD, "CLONE_CHILD_CLEARTID declines -- x4 is not read");

    // The exit signal is the LOW BYTE, not a flag, and only SIGCHLD is
    // admitted: `exits()` posts `child_exit` unconditionally (I-19), so any
    // other request -- including 0, "no signal", which is what a detached child
    // asks for -- would get a note it did not ask for.
    TEST_EXPECT_EQ((int)vivarium_clone_decide(
                       (u64)(VIV_CLONE_VM | VIV_CLONE_VFORK), sp, &sm),
                   (int)VIV_FORWARD, "exit signal 0 declines -- child_exit posts regardless");
    TEST_EXPECT_EQ((int)vivarium_clone_decide(
                       (u64)(VIV_CLONE_VM | VIV_CLONE_VFORK | 15u), sp, &sm),
                   (int)VIV_FORWARD, "a non-SIGCHLD exit signal declines");
    // The same rule on the fork word. clone(0, 0) is a detached child asking
    // for no exit signal at all, and `exits()` posts child_exit regardless.
    TEST_EXPECT_EQ((int)vivarium_clone_decide(0, 0, &sm),
                   (int)VIV_FORWARD, "a bare clone(0) declines -- no exit signal is unservable");

    // A ZERO stack is Linux's `vfork()` proper -- "share the parent's stack",
    // which is safe there only because CLONE_VFORK suspends the parent.
    // SYS_RFORK refuses a zero child_sp by contract, so this declines one layer
    // above rather than weakening a landed kernel gate. LINEAGE.md §9's fourth
    // question, second half.
    TEST_EXPECT_EQ((int)vivarium_clone_decide(ok, 0, &sm),
                   (int)VIV_FORWARD, "stack==0 declines -- vfork() proper is out of scope");

    // THE HIGH HALF IS READ, unlike every other decide in this file -- and that
    // asymmetry is the point. mmap/openat narrow to 32 bits because their Linux
    // parameters ARE `int`; clone's is an `unsigned long`, so narrowing would be
    // an assumption about Linux's own source rather than about its ABI. Under
    // that uncertainty the stricter reading wins, and it costs nothing: musl's
    // clone.s zero-extends (`uxtw x0,w2`), so the high half is always 0 from the
    // real consumer. Reverting to a `(u32)` cast fails HERE.
    TEST_EXPECT_EQ((int)vivarium_clone_decide(0xDEADBEEF00000000ull | ok, sp, &sm),
                   (int)VIV_FORWARD,
                   "a high-half bit declines -- clone's flags word is 64-bit");
    TEST_EXPECT_EQ((int)vivarium_clone_decide(0xDEADBEEF00000000ull | forkw, 0, &sm),
                   (int)VIV_FORWARD,
                   "the fork word is full-width too -- a high-half bit declines");

    // FAIL CLOSED on a NULL out-param. The shell cannot then read an
    // uninitialised `share_mem` and choose RFMEM by accident.
    TEST_EXPECT_EQ((int)vivarium_clone_decide(ok, sp, NULL),
                   (int)VIV_FORWARD, "a NULL share_mem_out declines");

    // clone is a TIER-2 row -- it needs the exception frame, so it can never be
    // a renumber. Pinned here so a future edit cannot demote it to T1, which
    // would copy all six argument words verbatim into SYS_RFORK and hand the
    // child musl's garbage x2 as its TPIDR_EL0. execve is TIER2 for the same
    // structural reason (L-6a): it rewrites the frame rather than filling it.
    {
        u64 args[VIV_NARGS];
        struct viv_call out;
        viv_fill_args(args);
        TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_CLONE, args, &out),
                       (int)VIV_TIER2, "clone is TIER2 -- never a renumber");
        TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_EXECVE, args, &out),
                       (int)VIV_TIER2, "execve is TIER2 -- never a renumber");
    }
}

// -----------------------------------------------------------------------------
// vivarium.wait4_domain — the option-word map (LINEAGE L-6b, §5.5).
//
// The row that lets a guest reap what L-6a let it create. Its risk is entirely
// NUMERIC: Linux's option bits and Thylacine's WAIT_* bits agree on two values
// and disagree on the third, with Linux's WEXITED sitting exactly on
// WAIT_CONTINUED. So the assertions below are about VALUES, not just verdicts.
// -----------------------------------------------------------------------------
void test_vivarium_wait4_domain(void) {
    // POISONED before every admitted call, in the clone_domain discipline: the
    // fields are only meaningful on VIV_TRANSLATED, so seeding them WRONG is
    // what makes the reads below real rather than a leftover.
    struct viv_wait_opts o;

    // THE COLLISION, stated as an executable fact rather than a comment. This
    // is the whole reason wait4 is a translator: two of the three bits are the
    // identity by coincidence, and the third is not -- and the value it would
    // collide with is occupied by a DIFFERENT Linux flag.
    TEST_ASSERT((u32)VIV_WNOHANG   == (u32)WAIT_WNOHANG,
                "WNOHANG is the identity (1) -- by coincidence, not by design");
    TEST_ASSERT((u32)VIV_WUNTRACED == (u32)WAIT_UNTRACED,
                "WUNTRACED is the identity (2) -- likewise");
    TEST_ASSERT((u32)VIV_WCONTINUED != (u32)WAIT_CONTINUED,
                "WCONTINUED is NOT the identity: Linux 8 vs Thylacine 4");
    TEST_ASSERT((u32)VIV_WEXITED == (u32)WAIT_CONTINUED,
                "and the gap is OCCUPIED -- Linux WEXITED is numerically "
                "WAIT_CONTINUED, so a passthrough would silently opt a guest "
                "into continue-reports AND the packed status encoding");

    // The plain shape: `wait(&st)` -> waitpid(-1, &st, 0) -> wait4(-1, &st, 0, 0).
    o.nohang = o.untraced = o.continued = true;
    TEST_EXPECT_EQ((int)vivarium_wait4_decide(0, 0, &o),
                   (int)VIV_TRANSLATED, "options 0 is the plain blocking wait");
    TEST_ASSERT(!o.nohang && !o.untraced && !o.continued,
                "options 0 asks for nothing -- every field clear");

    // Each admitted bit ALONE, so a map that happened to set the right field
    // for the wrong bit cannot pass by accident.
    o.nohang = false; o.untraced = true; o.continued = true;
    TEST_EXPECT_EQ((int)vivarium_wait4_decide((u64)VIV_WNOHANG, 0, &o),
                   (int)VIV_TRANSLATED, "WNOHANG is admitted");
    TEST_ASSERT(o.nohang && !o.untraced && !o.continued, "WNOHANG sets ONLY nohang");

    o.nohang = true; o.untraced = false; o.continued = true;
    TEST_EXPECT_EQ((int)vivarium_wait4_decide((u64)VIV_WUNTRACED, 0, &o),
                   (int)VIV_TRANSLATED, "WUNTRACED is admitted");
    TEST_ASSERT(!o.nohang && o.untraced && !o.continued, "WUNTRACED sets ONLY untraced");

    // THE LOAD-BEARING ONE. Linux bit 8 must reach `.continued`; the shell then
    // turns that into WAIT_CONTINUED (bit 4). A passthrough would instead set a
    // bit the native handler rejects as unknown, so the guest's WCONTINUED wait
    // would fail outright.
    o.nohang = true; o.untraced = true; o.continued = false;
    TEST_EXPECT_EQ((int)vivarium_wait4_decide((u64)VIV_WCONTINUED, 0, &o),
                   (int)VIV_TRANSLATED, "WCONTINUED is admitted");
    TEST_ASSERT(!o.nohang && !o.untraced && o.continued,
                "Linux bit 8 sets ONLY continued -- the non-identity map");

    // Options COMPOSE, which is why the admitted word is a MASK here and an
    // EXACT value in the clone/mmap rows: every subset of these three is a
    // meaningful request, whereas musl emits exactly one mmap flags word.
    o.nohang = o.untraced = o.continued = false;
    TEST_EXPECT_EQ((int)vivarium_wait4_decide(
                       (u64)(VIV_WNOHANG | VIV_WUNTRACED | VIV_WCONTINUED), 0, &o),
                   (int)VIV_TRANSLATED, "the three admitted bits compose");
    TEST_ASSERT(o.nohang && o.untraced && o.continued, "all three carry together");

    // THE DANGEROUS INPUT. WEXITED belongs to waitid, not wait4 -- but musl
    // defines it in the same header a guest includes, so a confused caller
    // reaches it, and its VALUE is WAIT_CONTINUED. Declining is what keeps the
    // mistranslation from being expressible at all.
    TEST_EXPECT_EQ((int)vivarium_wait4_decide((u64)VIV_WEXITED, 0, &o),
                   (int)VIV_FORWARD,
                   "WEXITED declines -- it is waitid's, and it sits on WAIT_CONTINUED");
    TEST_EXPECT_EQ((int)vivarium_wait4_decide((u64)VIV_WNOWAIT, 0, &o),
                   (int)VIV_FORWARD, "WNOWAIT declines -- also waitid's");

    // The Linux-only trio. Excluded as a DOMAIN matter, not an oversight: all
    // three discriminate thread-children from process-children, and Thylacine's
    // process table does not draw that line (a Thread is not a child). There is
    // nothing to approximate them with.
    TEST_EXPECT_EQ((int)vivarium_wait4_decide((u64)VIV_WALL, 0, &o),
                   (int)VIV_FORWARD, "__WALL declines -- no thread/process child split");
    TEST_EXPECT_EQ((int)vivarium_wait4_decide((u64)VIV_WCLONE, 0, &o),
                   (int)VIV_FORWARD, "__WCLONE declines -- likewise");
    TEST_EXPECT_EQ((int)vivarium_wait4_decide((u64)VIV_WNOTHREAD, 0, &o),
                   (int)VIV_FORWARD, "__WNOTHREAD declines -- likewise");

    // A valid bit alongside an invalid one declines the WHOLE word. Masking the
    // known bits out and proceeding would serve a request the caller did not
    // make, which is the failure mode an allow-list exists to prevent.
    TEST_EXPECT_EQ((int)vivarium_wait4_decide(
                       (u64)(VIV_WNOHANG | VIV_WEXITED), 0, &o),
                   (int)VIV_FORWARD, "one bad bit declines the whole word");

    // rusage. musl's waitpid and wait pass a literal 0 (src/process/waitpid.c),
    // so this only turns away a deliberate wait4(..., &ru) -- and it turns it
    // away rather than zeroing the struct, which would be a stored lie about a
    // child that used no CPU.
    TEST_EXPECT_EQ((int)vivarium_wait4_decide(0, 0x40000000ull, &o),
                   (int)VIV_FORWARD, "a non-NULL rusage declines");
    TEST_EXPECT_EQ((int)vivarium_wait4_decide((u64)VIV_WNOHANG, 1, &o),
                   (int)VIV_FORWARD, "even an unaligned junk rusage declines");

    // NARROWED TO 32 BITS, the OPPOSITE of clone_domain's full-width read -- and
    // the asymmetry is the ABI, not a preference. Linux declares wait4's
    // `options` as `int`, so a caller that leaves x2 sign-extended must be read
    // identically to one that zero-extends. clone's `flags` is an unsigned long,
    // which is why THAT comparison keeps the high half.
    o.nohang = o.untraced = o.continued = true;
    TEST_EXPECT_EQ((int)vivarium_wait4_decide(0xDEADBEEF00000000ull, 0, &o),
                   (int)VIV_TRANSLATED, "the high half is not part of an `int` options word");
    TEST_ASSERT(!o.nohang && !o.untraced && !o.continued,
                "a high-half-only word still asks for nothing");

    // FAIL CLOSED on a NULL out-param, so a shell that ignores the verdict
    // cannot read an uninitialised struct and compose flags from garbage.
    TEST_EXPECT_EQ((int)vivarium_wait4_decide(0, 0, NULL),
                   (int)VIV_FORWARD, "a NULL out declines");

    // wait4 is TIER-2, never a renumber: the option map, the conditional status
    // pack, and the ECHILD mapping all live in the shell. A demotion to T1 would
    // copy the option word verbatim into SYS_WAIT_PID, where the unknown-bit
    // gate would reject WCONTINUED outright and WEXITED would sail through as
    // WAIT_CONTINUED.
    {
        u64 args[VIV_NARGS];
        struct viv_call out;
        viv_fill_args(args);
        out.nr = 0xDEADu;
        TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_WAIT4, args, &out),
                       (int)VIV_TIER2, "wait4 is TIER2 -- never a renumber");
        TEST_EXPECT_EQ((u64)out.nr, (u64)0xDEADu, "a T2 verdict leaves out untouched");
    }
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

    // #102 F4: the SENTINEL is not an index. VIV_SIGNOTE_NONE == 0, so the old
    // `>= COUNT` gate admitted it and read act[0].
    //
    // Poisoning act[0] BY HAND is what makes this non-vacuous. Through the API
    // the slot is unwritable, and a zeroed slot answers "no handler / not
    // ignored" either way -- so a test that only called the accessors would
    // pass identically with the fix reverted. Writing the field directly is the
    // only way to ask the question the guard actually answers: when slot 0
    // holds something, does a NONE lookup find it?
    tab.act[0].handler  = 0x400000;
    tab.act[0].flags    = VIV_SA_RESTORER;
    tab.act[0].restorer = 0x400100;
    TEST_ASSERT(!viv_sigtab_note_handler(&tab, VIV_SIGNOTE_NONE, &got),
                "the sentinel resolves to no handler even when act[0] is set");
    tab.act[0].handler = VIV_SIG_IGN;
    TEST_ASSERT(!viv_sigtab_note_ignored(&tab, VIV_SIGNOTE_NONE),
                "the sentinel is not 'ignored' even when act[0] says SIG_IGN");

    // ...and it stays unwritable, which is what keeps the read guard's job
    // small: no API path can put a disposition in slot 0 to be found.
    TEST_ASSERT(!viv_sigtab_set(&tab, VIV_SIGNOTE_NONE, &hnd),
                "the sentinel cannot be written");
    TEST_ASSERT(tab.act[0].handler == VIV_SIG_IGN,
                "the refused set left act[0] untouched");
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

    // nfds == 0 is Linux's timed sleep, and it is SERVED -- it was refused until
    // V-5c-2. SYS_POLL still rejects nfds == 0 (deliberately), so the decision
    // this asserts is that the domain check must LET IT THROUGH for the caller
    // to route to sys_poll_sleep_for rather than reject it here.
    err = -1;
    TEST_ASSERT(vivarium_ppoll_decide(0, 0, &err), "nfds == 0 is served -- a sleep");
    TEST_ASSERT(err == 0, "with no error");

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
                "a sigmask is refused even on the sleep shape");
    TEST_ASSERT(err == T_E_NOSYS, "reporting the sigmask, which is checked first");

    TEST_ASSERT(!vivarium_ppoll_decide(1, 0, NULL), "NULL out_err is refused");
}

// V-5c-2: the pselect6 argument domain + the nfds clamp.
void test_vivarium_pselect6_decide(void);
void test_vivarium_pselect6_decide(void) {
    s32 err = -1;
    u32 n   = 99;

    TEST_ASSERT(vivarium_pselect6_decide(8, 0, &n, &err), "8 fds, no sigmask, served");
    TEST_ASSERT(err == 0 && n == 8, "nfds passes through untouched");

    TEST_ASSERT(vivarium_pselect6_decide(0, 0, &n, &err), "nfds == 0 is served");
    TEST_ASSERT(n == 0, "as zero -- the caller routes it to the sleep");

    // Same refusal as ppoll's, same reason, same code.
    err = -1;
    TEST_ASSERT(!vivarium_pselect6_decide(8, 0x40000000, &n, &err),
                "a sigmask pair pointer is refused");
    TEST_ASSERT(err == T_E_NOSYS, "as ENOSYS -- unserved shape, not a bad value");

    // nfds is an int, so negative is reachable and IS a bad value.
    err = -1;
    TEST_ASSERT(!vivarium_pselect6_decide((u64)(s64)-1, 0, &n, &err),
                "negative nfds refused (sign-extended -- what musl passes)");
    // V-5d F3. The SAME -1 left merely ZERO-extended, which a different libc or
    // a hand-rolled wrapper may produce. Before the truncation this read as
    // 4294967295 -- positive, therefore CLAMPED to PROC_HANDLE_MAX and served,
    // so select(-1, ...) worked on one toolchain and was EINVAL on another.
    TEST_ASSERT(!vivarium_pselect6_decide(0x00000000FFFFFFFFull, 0, &n, &err),
                "negative nfds refused (zero-extended -- the same -1)");
    TEST_EXPECT_EQ(err, (s32)T_E_INVAL, "and it is EINVAL, not a clamp");
    TEST_ASSERT(err == T_E_INVAL, "as EINVAL");

    // THE CLAMP, which is Linux's `if (n > max_fds) n = max_fds` and NOT an
    // error. A bit above the fd table names an fd that cannot exist, so Linux
    // simply does not scan it -- and neither do we.
    err = -1;
    TEST_ASSERT(vivarium_pselect6_decide(100000, 0, &n, &err),
                "an absurd nfds is CLAMPED, not refused -- Linux does not error here");
    TEST_ASSERT(n == PROC_HANDLE_MAX, "clamped to the fd table, not to FD_SETSIZE");
    TEST_ASSERT(err == 0, "with no error");

    // The clamp is at PROC_HANDLE_MAX, and the value below it must survive: this
    // is the assertion that would have caught pouch's F-a, where the bound was
    // copied as 64 and the kernel later moved it.
    TEST_ASSERT(vivarium_pselect6_decide(200, 0, &n, &err) && n == 200,
                "fd 199 is a perfectly ordinary handle -- 64 is NOT the ceiling");

    TEST_ASSERT(!vivarium_pselect6_decide(1, 0, NULL, &err), "NULL out_nfds refused");
    TEST_ASSERT(!vivarium_pselect6_decide(1, 0, &n, NULL),   "NULL out_err refused");
}

// V-5c-2: FDS_BYTES -- how much of an fd_set covers the low nfds bits.
void test_vivarium_fdset_bytes(void);
void test_vivarium_fdset_bytes(void) {
    // Rounded up to whole 8-byte longs, which is what Linux's get_fd_set and
    // set_fd_set touch. Copying more could fault on a caller that sized its
    // allocation to its nfds; copying less would miss a requested bit.
    TEST_ASSERT(vivarium_fdset_bytes(0)  == 0,  "no fds needs no bytes");
    TEST_ASSERT(vivarium_fdset_bytes(1)  == 8,  "1 fd still rounds to a whole long");
    TEST_ASSERT(vivarium_fdset_bytes(64) == 8,  "64 fds is exactly one long");
    TEST_ASSERT(vivarium_fdset_bytes(65) == 16, "65 spills into a second");
    TEST_ASSERT(vivarium_fdset_bytes(256) == 32, "the clamp ceiling is 4 longs");
    TEST_ASSERT(vivarium_fdset_bytes(1024) == VIV_FD_SET_BYTES,
                "a full fd_set is 128 bytes");
    TEST_ASSERT(vivarium_fdset_bytes(100000) == VIV_FD_SET_BYTES,
                "and it never exceeds the object, whatever it is asked");
}

// V-5c-2: the forward conversion, three fd_sets -> a pollfd array.
void test_vivarium_fdset_to_pollfds(void);
void test_vivarium_fdset_to_pollfds(void) {
    u8 rd[VIV_FD_SET_BYTES], wr[VIV_FD_SET_BYTES], ex[VIV_FD_SET_BYTES];
    struct pollfd out[POLL_MAX_NFDS];
    u32 count = 99;
    s32 err   = -1;

    for (u32 i = 0; i < VIV_FD_SET_BYTES; i++) { rd[i] = 0; wr[i] = 0; ex[i] = 0; }

    // An fd in BOTH sets is ONE pollfd asking for both directions -- not two
    // entries. That is what lets the reverse map count it twice later.
    rd[0] = (u8)(1u << 3);             // fd 3 readable
    wr[0] = (u8)((1u << 3) | (1u << 5)); // fd 3 writable, fd 5 writable
    TEST_ASSERT(vivarium_fdset_to_pollfds(rd, wr, NULL, 8, out, POLL_MAX_NFDS,
                                          &count, &err),
                "a two-set scan converts");
    TEST_ASSERT(count == 2, "fd 3 (both) and fd 5 (write) are TWO pollfds, not three");
    TEST_ASSERT(out[0].fd == 3 && out[0].events == (POLLIN | POLLOUT),
                "fd 3 asks for both directions in one entry");
    TEST_ASSERT(out[1].fd == 5 && out[1].events == POLLOUT, "fd 5 asks to write");
    TEST_ASSERT(out[0].revents == 0 && out[1].revents == 0, "revents starts clear");

    // NULL sets are Linux's "I do not want this direction".
    for (u32 i = 0; i < VIV_FD_SET_BYTES; i++) { rd[i] = 0; wr[i] = 0; }
    rd[0] = (u8)(1u << 1);
    TEST_ASSERT(vivarium_fdset_to_pollfds(rd, NULL, NULL, 8, out, POLL_MAX_NFDS,
                                          &count, &err),
                "NULL write + NULL except is fine");
    TEST_ASSERT(count == 1 && out[0].fd == 1 && out[0].events == POLLIN,
                "only the read request survives");

    // nfds BOUNDS THE SCAN. A bit above it is simply not looked at -- this is
    // what makes the decide-stage clamp safe.
    for (u32 i = 0; i < VIV_FD_SET_BYTES; i++) rd[i] = 0;
    rd[1] = (u8)(1u << 0);             // fd 8
    TEST_ASSERT(vivarium_fdset_to_pollfds(rd, NULL, NULL, 8, out, POLL_MAX_NFDS,
                                          &count, &err),
                "a bit at fd 8 with nfds 8 converts");
    TEST_ASSERT(count == 0, "and contributes nothing -- the range is [0, nfds)");
    TEST_ASSERT(vivarium_fdset_to_pollfds(rd, NULL, NULL, 9, out, POLL_MAX_NFDS,
                                          &count, &err) && count == 1,
                "the same bit with nfds 9 does contribute");

    // A HIGH fd VALUE IS FINE -- the ceiling is on the COUNT. This is pouch's
    // F-a as an assertion: it would return EBADF here, and there is nothing
    // wrong with polling fd 200.
    for (u32 i = 0; i < VIV_FD_SET_BYTES; i++) rd[i] = 0;
    rd[25] = (u8)(1u << 0);            // fd 200
    TEST_ASSERT(vivarium_fdset_to_pollfds(rd, NULL, NULL, 256, out,
                                          POLL_MAX_NFDS, &count, &err),
                "fd 200 converts -- a high fd VALUE is not an error");
    TEST_ASSERT(count == 1 && out[0].fd == 200, "as one pollfd naming fd 200");

    // ... and the count ceiling really does bite, on LOW fds.
    for (u32 i = 0; i < VIV_FD_SET_BYTES; i++) rd[i] = 0xFF;
    err = -1;
    TEST_ASSERT(!vivarium_fdset_to_pollfds(rd, NULL, NULL, POLL_MAX_NFDS + 1, out,
                                           POLL_MAX_NFDS, &count, &err),
                "65 contributing fds overflows the array");
    TEST_ASSERT(err == T_E_INVAL, "as EINVAL");

    // EXCEPTFDS DECLINES. pouch maps it to POLLPRI, which native poll cannot
    // report, so its select blocks forever on a pure-exceptfds wait instead of
    // failing (F-b). Declining is the honest answer.
    for (u32 i = 0; i < VIV_FD_SET_BYTES; i++) { rd[i] = 0; ex[i] = 0; }
    ex[0] = (u8)(1u << 2);
    err = -1;
    TEST_ASSERT(!vivarium_fdset_to_pollfds(NULL, NULL, ex, 8, out, POLL_MAX_NFDS,
                                           &count, &err),
                "a SET exceptfds bit is refused");
    TEST_ASSERT(err == T_E_NOSYS, "as ENOSYS -- there is no POLLPRI to map it to");

    // An all-zero exceptfds is not a request and must pass -- select callers
    // routinely pass a zeroed set they never check.
    for (u32 i = 0; i < VIV_FD_SET_BYTES; i++) ex[i] = 0;
    rd[0] = (u8)(1u << 2);
    TEST_ASSERT(vivarium_fdset_to_pollfds(rd, NULL, ex, 8, out, POLL_MAX_NFDS,
                                          &count, &err) && count == 1,
                "an all-zero exceptfds passes through");

    // A set exceptfds bit ABOVE nfds is out of range and must not decline.
    for (u32 i = 0; i < VIV_FD_SET_BYTES; i++) ex[i] = 0;
    ex[1] = (u8)(1u << 0);             // fd 8
    TEST_ASSERT(vivarium_fdset_to_pollfds(rd, NULL, ex, 8, out, POLL_MAX_NFDS,
                                          &count, &err),
                "an exceptfds bit outside [0, nfds) is not a request");
}

// V-5c-2: the reverse conversion -- the asymmetric mapping and the BIT count.
void test_vivarium_pollfds_to_fdset(void);
void test_vivarium_pollfds_to_fdset(void) {
    u8 rd[VIV_FD_SET_BYTES], wr[VIV_FD_SET_BYTES], ex[VIV_FD_SET_BYTES];
    struct pollfd pfds[4];
    u32 bits = 99;
    s32 err  = -1;

    // Pre-dirty the outputs: select OVERWRITES, so a stale bit must be cleared.
    for (u32 i = 0; i < VIV_FD_SET_BYTES; i++) { rd[i] = 0xFF; wr[i] = 0xFF; ex[i] = 0xFF; }

    // fd 3 asked both ways and is ready both ways -> TWO bits, ONE fd. This is
    // the contract pouch's F-d gets wrong by counting fds.
    pfds[0] = (struct pollfd){ .fd = 3, .events = POLLIN | POLLOUT,
                               .revents = POLLIN | POLLOUT };
    TEST_ASSERT(vivarium_pollfds_to_fdset(pfds, 1, rd, wr, ex, &bits, &err),
                "a both-ways-ready fd converts");
    TEST_ASSERT(bits == 2, "and counts TWICE -- the return is BITS, not fds");
    TEST_ASSERT((rd[0] & (1u << 3)) != 0, "set in readfds");
    TEST_ASSERT((wr[0] & (1u << 3)) != 0, "and in writefds");
    TEST_ASSERT(rd[1] == 0 && wr[1] == 0 && ex[0] == 0,
                "and every other byte was CLEARED -- select overwrites");

    // THE ASYMMETRY. POLLHUP is in Linux's POLLIN_SET and NOT in POLLOUT_SET, so
    // a hung-up fd asked about both ways comes back READABLE ONLY. pouch reports
    // it writable too (F-c), commented "(Linux semantics)".
    for (u32 i = 0; i < VIV_FD_SET_BYTES; i++) { rd[i] = 0; wr[i] = 0; }
    pfds[0] = (struct pollfd){ .fd = 2, .events = POLLIN | POLLOUT,
                               .revents = POLLHUP };
    TEST_ASSERT(vivarium_pollfds_to_fdset(pfds, 1, rd, wr, NULL, &bits, &err),
                "a hung-up fd converts");
    TEST_ASSERT((rd[0] & (1u << 2)) != 0, "POLLHUP reports READABLE");
    TEST_ASSERT((wr[0] & (1u << 2)) == 0,
                "and NOT writable -- POLLHUP is not in POLLOUT_SET");
    TEST_ASSERT(bits == 1, "so it is one bit, not two");

    // POLLERR is in BOTH sets, so an errored fd asked both ways comes back both.
    for (u32 i = 0; i < VIV_FD_SET_BYTES; i++) { rd[i] = 0; wr[i] = 0; }
    pfds[0] = (struct pollfd){ .fd = 4, .events = POLLIN | POLLOUT,
                               .revents = POLLERR };
    TEST_ASSERT(vivarium_pollfds_to_fdset(pfds, 1, rd, wr, NULL, &bits, &err),
                "an errored fd converts");
    TEST_ASSERT((rd[0] & (1u << 4)) != 0 && (wr[0] & (1u << 4)) != 0,
                "POLLERR reports in BOTH directions");
    TEST_ASSERT(bits == 2, "as two bits");

    // ... but only in the directions the caller ASKED about. An errored fd the
    // caller listed only in readfds must not appear in writefds.
    for (u32 i = 0; i < VIV_FD_SET_BYTES; i++) { rd[i] = 0; wr[i] = 0; }
    pfds[0] = (struct pollfd){ .fd = 4, .events = POLLIN, .revents = POLLERR };
    TEST_ASSERT(vivarium_pollfds_to_fdset(pfds, 1, rd, wr, NULL, &bits, &err),
                "a read-only-requested errored fd converts");
    TEST_ASSERT((rd[0] & (1u << 4)) != 0, "reports readable");
    TEST_ASSERT((wr[0] & (1u << 4)) == 0, "and NOT writable -- it was never asked");
    TEST_ASSERT(bits == 1, "as one bit");

    // POLLNVAL FAILS THE WHOLE CALL. poll reports a bad fd per-entry; select has
    // no per-fd error channel, so POSIX makes it EBADF for everything.
    for (u32 i = 0; i < VIV_FD_SET_BYTES; i++) { rd[i] = 0xAA; wr[i] = 0xAA; }
    pfds[0] = (struct pollfd){ .fd = 1, .events = POLLIN, .revents = POLLIN };
    pfds[1] = (struct pollfd){ .fd = 2, .events = POLLIN, .revents = POLLNVAL };
    err = -1;
    TEST_ASSERT(!vivarium_pollfds_to_fdset(pfds, 2, rd, wr, NULL, &bits, &err),
                "POLLNVAL anywhere fails the call");
    TEST_ASSERT(err == T_E_BADF, "as EBADF, not as a per-fd result");
    TEST_ASSERT(rd[0] == 0xAA,
                "and the caller's sets are UNTOUCHED -- Linux does not write "
                "them back on an error");

    // Nothing ready is a legal, common answer: zero bits and clear sets.
    for (u32 i = 0; i < VIV_FD_SET_BYTES; i++) { rd[i] = 0xFF; wr[i] = 0xFF; }
    pfds[0] = (struct pollfd){ .fd = 1, .events = POLLIN, .revents = 0 };
    TEST_ASSERT(vivarium_pollfds_to_fdset(pfds, 1, rd, wr, NULL, &bits, &err),
                "a timed-out poll converts");
    TEST_ASSERT(bits == 0, "with no bits");
    TEST_ASSERT(rd[0] == 0 && wr[0] == 0, "and fully cleared sets");

    TEST_ASSERT(!vivarium_pollfds_to_fdset(pfds, 1, rd, wr, NULL, NULL, &err),
                "NULL out_bits refused");
    TEST_ASSERT(!vivarium_pollfds_to_fdset(NULL, 1, rd, wr, NULL, &bits, &err),
                "NULL pfds with a non-zero count refused");
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

// V-5d SA-1. The socktab keys on the fd NUMBER, so an entry must be dropped
// whenever that number is freed -- and exactly one place does it: the close
// hook in viv_linux_dispatch, which fires on VIV_LINUX_CLOSE alone.
//
// That is SUFFICIENT only while close is the ONLY fd-freeing row, and nothing
// in the code says so. This test says it. Each number below frees an fd on
// Linux and each is a near-trivial renumber (dup3 -> SYS_DUP is nearly one), so
// adding one as an ordinary T1 row is an easy and invisible mistake -- and it
// would reintroduce precisely the bug the hook exists to prevent: a freed fd
// number whose (proto, N) entry survives to be handed to the next fd-creating
// call, so a later connect() writes a dial verb to a stranger's connection.
//
// If this test fails, the fix is NOT to delete the line. It is to extend the
// close hook to the newly-served number, in the same commit.
void test_vivarium_fd_freeing_rows_stay_unserved(void);
void test_vivarium_fd_freeing_rows_stay_unserved(void) {
    u64 args[VIV_NARGS];
    struct viv_call out;
    viv_fill_args(args);

    // STILL UNSERVED, and each still owes the socktab a drop of the entry keyed
    // on the number it frees before it can be promoted.
    static const struct { u64 nr; const char *what; } frees_an_fd[] = {
        { VIV_LINUX_DUP,         "dup" },
        { VIV_LINUX_CLOSE_RANGE, "close_range" },
    };

    for (u32 i = 0; i < (u32)(sizeof(frees_an_fd) / sizeof(frees_an_fd[0])); i++) {
        out.nr = 0xDEADu;
        enum viv_verdict v = vivarium_translate(frees_an_fd[i].nr, args, &out);
        TEST_ASSERT(v != VIV_TRANSLATED && v != VIV_TIER2,
                    "an unserved fd-freeing call stays unserved -- pay the "
                    "socktab drop in the same commit that promotes it");
        TEST_EXPECT_EQ((u64)out.nr, (u64)0xDEADu,
                       "a declined verdict leaves out untouched");
        (void)frees_an_fd[i].what;
    }

    // THE TWO THAT ARE SERVED, and their CLASSIFICATION is the assertion --
    // because it is the classification that decides WHERE each pays the drop,
    // and the two answers are different (#157):
    //
    //   close is T1, so it never runs a shell of its own, and its drop
    //   therefore lives in the ENTRY HOOK in viv_linux_dispatch. That is sound
    //   only because a close whose fd carries an entry always proceeds.
    //
    //   dup3 is TIER2, so it HAS a shell, and its drop lives there -- it must,
    //   because dup3 can be refused (bad flags, old == new, bad old) while its
    //   target is a live socket, and an entry-time drop would destroy the
    //   guest's socket state on a call that failed.
    //
    // Asserting the tiers here is what makes a future re-classification a
    // deliberate act: turning dup3 into a T1 renumber would strand its drop
    // (and would be wrong anyway -- see the arity note below).
    out.nr = 0xDEADu;
    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_CLOSE, args, &out),
                   (int)VIV_TRANSLATED, "close is served (T1) -- the hook's subject");
    TEST_EXPECT_EQ((u64)out.nr, (u64)SYS_CLOSE, "and renumbers to the native close");

    out.nr = 0xDEADu;
    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_DUP3, args, &out),
                   (int)VIV_TIER2, "dup3 is served as TIER2 -- it pays in its shell");
    TEST_EXPECT_EQ((u64)out.nr, (u64)0xDEADu,
                   "and a TIER2 verdict still leaves out untouched (no renumber). "
                   "It could not be one: SYS_DUP's second argument is a RIGHTS "
                   "MASK where dup3's is a TARGET FD, so a renumber would read "
                   "an fd number as a set of capability bits");
}

// -----------------------------------------------------------------------------
// The startup batch (#150). The set busybox issues between _start and its first
// useful instruction -- MEASURED off a running guest, not guessed.
// -----------------------------------------------------------------------------

// Local, because this file has deliberately never pulled in a string.h.
static bool viv_str_eq(const char *a, const char *b) {
    u32 i = 0;
    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) return false;
        i++;
    }
    return a[i] == b[i];
}

// Each row asserted BY NAME and BY TIER, for the reason the sibling
// rejects_are_deliberate test states: promoting or demoting one later should
// fail a test rather than pass silently. The tier split is the interesting part
// -- getuid and getgid have exact native twins and matching arity and are still
// T2, so "it renumbers cleanly" is visibly not the criterion.
void test_vivarium_startup_batch_rows(void);
void test_vivarium_startup_batch_rows(void) {
    u64 args[VIV_NARGS];
    struct viv_call out;
    viv_fill_args(args);

    // getpid is the ONLY renumber in the batch: no arguments, a pid return that
    // can never land in the errno band, and no error path.
    out.nr = 0xDEADu;
    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_GETPID, args, &out),
                   (int)VIV_TRANSLATED, "getpid is T1 (0 args, pid return)");
    TEST_EXPECT_EQ((u64)out.nr, (u64)SYS_GETPID, "getpid renumbers to SYS_GETPID");

    // getuid/getgid: same shape as getpid, still T2. The sentinel mapping
    // (PRINCIPAL_SYSTEM -> 0) has to happen somewhere and a renumber has no
    // place to put it -- this pair is the standing counterexample to "matching
    // arity means it is a T1 row".
    out.nr = 0xDEADu;
    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_GETUID, args, &out),
                   (int)VIV_TIER2, "getuid is T2 despite an exact native twin");
    TEST_EXPECT_EQ((u64)out.nr, (u64)0xDEADu, "a T2 verdict leaves out untouched");
    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_GETGID, args, &out),
                   (int)VIV_TIER2, "getgid is T2 for the same reason");

    // writev is the arity rule's sharpest case: three registers that line up
    // with SYS_WRITE's three and mean something completely different. A T1
    // verdict here would mean the kernel writes the guest's iovec ARRAY -- its
    // own pointers -- to the fd.
    out.nr = 0xDEADu;
    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_WRITEV, args, &out),
                   (int)VIV_TIER2,
                   "writev is T2, never T1 -- arg1 is an array, arg2 a count");
    TEST_EXPECT_EQ((u64)out.nr, (u64)0xDEADu, "a T2 verdict leaves out untouched");

    // getcwd: arguments align exactly with SYS_GETCWD and it is still T2, for
    // two independent reasons (the return is off by one, the error is ERANGE
    // not a flat -1). Either alone disqualifies the renumber.
    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_GETCWD, args, &out),
                   (int)VIV_TIER2, "getcwd is T2 (+1 on the length, and ERANGE)");

    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_GETPPID, args, &out),
                   (int)VIV_TIER2, "getppid is T2 -- there is no native twin");
    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_UNAME, args, &out),
                   (int)VIV_TIER2, "uname is T2 (a fabrication, not a translation)");
    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_SET_TID_ADDRESS, args, &out),
                   (int)VIV_TIER2, "set_tid_address is T2 (an errno translation)");
    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_SETUID, args, &out),
                   (int)VIV_TIER2, "setuid is T2 (EPERM, except the no-op)");
    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_SETGID, args, &out),
                   (int)VIV_TIER2, "setgid is T2 (EPERM, except the no-op)");

    // fcntl was ENOSYS through #150 because Thylacine had no close-on-exec at
    // all, so the cmds a libc actually reaches for could ONLY have been served
    // by silently succeeding. #151 built the feature first and then served the
    // row -- which is the order this assert exists to record. It is Tier-2 and
    // NOT a renumber onto SYS_DUP: that call's second argument is a rights mask
    // where F_DUPFD's is a minimum fd (the arity rule).
    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_FCNTL, args, &out),
                   (int)VIV_TIER2,
                   "fcntl is T2 (close-on-exec exists; the rest of the cmds decline)");

    // The two that stay declined ON PURPOSE, re-asserted here because the
    // census lists them alongside the batch and a future reader will wonder why
    // they were skipped.
    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_BRK, args, &out),
                   (int)VIV_ENOSYS, "brk stays ENOSYS -- musl falls to mmap");
    TEST_EXPECT_EQ((int)vivarium_translate(VIV_LINUX_MPROTECT, args, &out),
                   (int)VIV_ENOSYS, "mprotect stays ENOSYS -- I-12");
}

void test_vivarium_writev_domain(void);
void test_vivarium_writev_domain(void) {
    u32 count = 0xFFFFFFFFu;

    // 0 is IN domain, and that is the load-bearing case: Linux resolves the fd
    // before it looks at the array, so writev(badfd, x, 0) must reach the
    // handle check and answer EBADF rather than short-circuit to 0.
    TEST_EXPECT_EQ((int)vivarium_writev_decide(0, &count),
                   (int)VIV_TRANSLATED, "iovcnt 0 is in domain (the fd is still checked)");
    TEST_EXPECT_EQ((u64)count, (u64)0, "iovcnt 0 passes through as 0");

    TEST_EXPECT_EQ((int)vivarium_writev_decide(1, &count),
                   (int)VIV_TRANSLATED, "iovcnt 1 is in domain");
    TEST_EXPECT_EQ((u64)count, (u64)1, "iovcnt 1 passes through");

    TEST_EXPECT_EQ((int)vivarium_writev_decide(VIV_UIO_MAXIOV, &count),
                   (int)VIV_TRANSLATED, "iovcnt == UIO_MAXIOV is in domain");
    TEST_EXPECT_EQ((u64)count, (u64)VIV_UIO_MAXIOV, "the boundary value passes through");

    TEST_EXPECT_EQ((int)vivarium_writev_decide(VIV_UIO_MAXIOV + 1, &count),
                   (int)VIV_FORWARD, "one past UIO_MAXIOV declines");

    // Linux's third argument is an `int`, so a value with bit 31 set is NEGATIVE
    // on the guest's side and Linux answers EINVAL. Read as u64 it is enormous,
    // so the single unsigned comparison catches it -- this asserts that
    // equivalence rather than assuming it.
    TEST_EXPECT_EQ((int)vivarium_writev_decide(0xFFFFFFFFull, &count),
                   (int)VIV_FORWARD, "a guest-negative count declines");
    TEST_EXPECT_EQ((int)vivarium_writev_decide(0xFFFFFFFFFFFFFFFFull, &count),
                   (int)VIV_FORWARD, "a full-width count declines");

    // A NULL out is a caller bug; fail closed rather than write through it.
    TEST_EXPECT_EQ((int)vivarium_writev_decide(1, NULL),
                   (int)VIV_FORWARD, "a NULL out declines");

    // The SSIZE_MAX rule. Linux checks it BEFORE writing anything, because a
    // total above SSIZE_MAX cannot be returned without colliding with the
    // negative-errno band.
    const u64 ssize_max = 0x7FFFFFFFFFFFFFFFull;
    u64 total = 0;
    TEST_EXPECT_EQ((int)vivarium_writev_accumulate(&total, 100), 1, "a small add succeeds");
    TEST_EXPECT_EQ(total, (u64)100, "the running total advances");
    TEST_EXPECT_EQ((int)vivarium_writev_accumulate(&total, 0), 1, "a zero-length entry is fine");
    TEST_EXPECT_EQ(total, (u64)100, "a zero-length entry does not move the total");

    total = ssize_max - 10;
    TEST_EXPECT_EQ((int)vivarium_writev_accumulate(&total, 10), 1, "landing exactly on SSIZE_MAX succeeds");
    TEST_EXPECT_EQ(total, ssize_max, "the total reaches SSIZE_MAX exactly");
    TEST_EXPECT_EQ((int)vivarium_writev_accumulate(&total, 1), 0, "one byte past SSIZE_MAX fails");
    TEST_EXPECT_EQ(total, ssize_max, "a failed add leaves the total unchanged");

    // The wrap the ROOM test exists to prevent: a naive `total + add > max`
    // would overflow to a small number and pass.
    total = 1;
    TEST_EXPECT_EQ((int)vivarium_writev_accumulate(&total, 0xFFFFFFFFFFFFFFFFull), 0,
                   "an add that would wrap u64 fails rather than aliasing small");
    TEST_EXPECT_EQ(total, (u64)1, "the wrapping add left the total alone");

    TEST_EXPECT_EQ((int)vivarium_writev_accumulate(NULL, 1), 0, "a NULL total fails closed");
}

void test_vivarium_uname_fill(void);
void test_vivarium_uname_fill(void) {
    struct viv_linux_utsname uts;

    // Pre-poison EVERY byte. The zero-fill is an I-13 obligation, not tidiness:
    // the shell copies all 390 bytes to EL0, so any byte the fill does not
    // overwrite would leak whatever the kernel stack held. Filling with 0xAA
    // first is what makes the "and the rest is zero" assertion mean anything --
    // over a zeroed stack it would pass whether or not the fill zeroed at all.
    u8 *raw = (u8 *)&uts;
    for (u32 i = 0; i < (u32)sizeof(uts); i++) raw[i] = 0xAA;

    vivarium_uname_fill(&uts);

    TEST_EXPECT_EQ((int)viv_str_eq(uts.sysname, "Linux"), 1,
                   "sysname is Linux -- the ABI the guest sees IS Linux's");
    TEST_EXPECT_EQ((int)viv_str_eq(uts.machine, "aarch64"), 1, "machine is aarch64");
    TEST_EXPECT_EQ((int)viv_str_eq(uts.nodename, "thylacine"), 1, "nodename is thylacine");
    TEST_EXPECT_EQ((int)viv_str_eq(uts.domainname, "(none)"), 1, "domainname is Linux's default");

    // The field the decision is about. 4.4 is picked as the newest kernel that
    // promises nothing this table lacks -- below statx (4.11), io_uring (5.1),
    // clone3 (5.3), openat2 (5.6), close_range (5.9) -- and above glibc's 3.2
    // floor, under which a glibc binary aborts before main().
    TEST_EXPECT_EQ((int)viv_str_eq(uts.release, "4.4.0"), 1,
                   "release is 4.4.0 -- low enough to promise nothing we lack");

    // And the field that carries the truth, because nothing parses it.
    TEST_EXPECT_EQ((int)viv_str_eq(uts.version, "#1 Thylacine VIVARIUM"), 1,
                   "version names Thylacine -- observable, never load-bearing");

    // Every byte past each terminator must be 0, not poison. Walk the whole
    // struct field by field rather than spot-checking: the leak this guards
    // against is exactly a byte nobody thought to look at.
    const char *fields[6];
    fields[0] = uts.sysname;  fields[1] = uts.nodename;   fields[2] = uts.release;
    fields[3] = uts.version;  fields[4] = uts.machine;    fields[5] = uts.domainname;
    for (u32 f = 0; f < 6; f++) {
        u32 len = 0;
        while (fields[f][len] != '\0') len++;
        TEST_ASSERT(len < (u32)VIV_UTS_FIELD_LEN, "each field terminates inside its bound");
        for (u32 i = len; i < (u32)VIV_UTS_FIELD_LEN; i++)
            TEST_ASSERT(fields[f][i] == '\0', "every byte past the terminator is zero (I-13)");
    }

    // A NULL must not fault. The shell never passes one, which is exactly why
    // the guard needs a test rather than a reader's trust.
    vivarium_uname_fill(NULL);
}

void test_vivarium_identity_map(void);
void test_vivarium_identity_map(void) {
    // The sentinel. Passed through raw, PRINCIPAL_SYSTEM reads to a Linux guest
    // as (uid_t)-2 -- historically "nobody", the number meaning LEAST
    // privileged -- so the raw pass-through inverts the fact being asked about.
    TEST_EXPECT_EQ((u64)vivarium_map_uid(PRINCIPAL_SYSTEM), (u64)0,
                   "PRINCIPAL_SYSTEM maps to uid 0 -- the identity it corresponds to");
    TEST_EXPECT_EQ((u64)vivarium_map_gid(GID_SYSTEM), (u64)0, "GID_SYSTEM maps to gid 0");

    // Everything else passes through, and CANNOT collide, because
    // PRINCIPAL_INVALID and GID_INVALID are both 0 -- no real principal or group
    // is ever 0 to begin with. Asserting that sentinel here is what makes the
    // mapping injective by construction rather than by luck.
    TEST_EXPECT_EQ((u64)PRINCIPAL_INVALID, (u64)0,
                   "0 is the INVALID principal, so the mapping cannot collide");
    TEST_EXPECT_EQ((u64)GID_INVALID, (u64)0, "0 is the INVALID gid, likewise");
    TEST_EXPECT_EQ((u64)vivarium_map_uid(1000), (u64)1000, "an ordinary principal passes through");
    TEST_EXPECT_EQ((u64)vivarium_map_uid(1), (u64)1, "principal 1 passes through");
    TEST_EXPECT_EQ((u64)vivarium_map_gid(50), (u64)50, "an ordinary gid passes through");

    // PRINCIPAL_NONE is a DIFFERENT sentinel (unauthenticated "nobody") and must
    // NOT be folded into 0 -- it genuinely is the unprivileged identity, so
    // (uid_t)-1 is the honest thing for a guest to see.
    TEST_EXPECT_EQ((u64)vivarium_map_uid(PRINCIPAL_NONE), (u64)PRINCIPAL_NONE,
                   "PRINCIPAL_NONE is not remapped -- it really is nobody");

    // setuid(getuid()) is the call the no-op exists for: every "drop to my own
    // uid" path issues it and it asks for nothing. Comparing in the GUEST's
    // number space is what makes it work for a PRINCIPAL_SYSTEM Proc -- the case
    // that needs it most, and the one a raw comparison would refuse.
    u32 mapped = vivarium_map_uid(PRINCIPAL_SYSTEM);
    TEST_EXPECT_EQ((int)vivarium_setid_is_noop(0, mapped), 1,
                   "setuid(0) from a SYSTEM Proc is the no-op it looks like");
    TEST_EXPECT_EQ((int)vivarium_setid_is_noop(PRINCIPAL_SYSTEM, mapped), 0,
                   "the RAW sentinel is not the no-op -- the guest never saw it");
    TEST_EXPECT_EQ((int)vivarium_setid_is_noop(1000, mapped), 0,
                   "an actual identity change is refused");
    TEST_EXPECT_EQ((int)vivarium_setid_is_noop(1000, 1000), 1,
                   "an ordinary Proc's setuid to itself is the no-op too");
}

// #151. The fcntl classifier. The served set is MEASURED (busybox issues exactly
// F_SETFD(FD_CLOEXEC) and F_DUPFD_CLOEXEC(10) at startup), so both of those get
// their own case here spelled with the values the guest actually sends.
void test_vivarium_fcntl_domain(void);
void test_vivarium_fcntl_domain(void) {
    enum viv_fcntl_op op;
    bool cx;
    u64  minfd;

    // The two MEASURED calls, by their raw values. Writing 0x2 / 0x406 rather
    // than the names is deliberate: these are the numbers observed on the wire,
    // and a mistyped constant would otherwise agree with itself.
    op = VIV_FCNTL_UNSERVED; cx = false; minfd = 0xBADu;
    TEST_EXPECT_EQ((int)vivarium_fcntl_decide(0x2, 1, &op, &cx, &minfd),
                   (int)VIV_TRANSLATED, "F_SETFD(FD_CLOEXEC) is served");
    TEST_EXPECT_EQ((int)op, (int)VIV_FCNTL_SETFD, "0x2 classifies as SETFD");
    TEST_EXPECT_EQ((u64)(cx ? 1 : 0), (u64)1, "FD_CLOEXEC asks for the flag ON");

    op = VIV_FCNTL_UNSERVED; cx = false; minfd = 0xBADu;
    TEST_EXPECT_EQ((int)vivarium_fcntl_decide(0x406, 10, &op, &cx, &minfd),
                   (int)VIV_TRANSLATED, "F_DUPFD_CLOEXEC(10) is served");
    TEST_EXPECT_EQ((int)op, (int)VIV_FCNTL_DUPFD, "0x406 classifies as DUPFD");
    TEST_EXPECT_EQ((u64)(cx ? 1 : 0), (u64)1, "the _CLOEXEC form sets the flag");
    TEST_EXPECT_EQ(minfd, (u64)10, "and carries the MINIMUM fd, not a rights mask");

    // F_DUPFD is the same op with the flag off. Serving one of the pair and not
    // the other would be an arbitrary edge for a guest to find at runtime.
    op = VIV_FCNTL_UNSERVED; cx = true; minfd = 0xBADu;
    TEST_EXPECT_EQ((int)vivarium_fcntl_decide(VIV_F_DUPFD, 3, &op, &cx, &minfd),
                   (int)VIV_TRANSLATED, "F_DUPFD is served");
    TEST_EXPECT_EQ((int)op, (int)VIV_FCNTL_DUPFD, "classifies as DUPFD");
    TEST_EXPECT_EQ((u64)(cx ? 1 : 0), (u64)0, "the plain form leaves the flag off");
    TEST_EXPECT_EQ(minfd, (u64)3, "and carries its minimum");

    op = VIV_FCNTL_UNSERVED; cx = true; minfd = 0xBADu;
    TEST_EXPECT_EQ((int)vivarium_fcntl_decide(VIV_F_GETFD, 0, &op, &cx, &minfd),
                   (int)VIV_TRANSLATED, "F_GETFD is served");
    TEST_EXPECT_EQ((int)op, (int)VIV_FCNTL_GETFD, "classifies as GETFD");

    // F_SETFD with the bit CLEAR is a real request -- "stop being close-on-exec"
    // -- not an absence of one.
    op = VIV_FCNTL_UNSERVED; cx = true; minfd = 0xBADu;
    TEST_EXPECT_EQ((int)vivarium_fcntl_decide(VIV_F_SETFD, 0, &op, &cx, &minfd),
                   (int)VIV_TRANSLATED, "F_SETFD(0) is served");
    TEST_EXPECT_EQ((u64)(cx ? 1 : 0), (u64)0, "F_SETFD(0) asks for the flag OFF");

    // MASK, do not reject. Linux's F_SETFD ignores every bit but FD_CLOEXEC
    // rather than refusing the call; being STRICTER than Linux for an input a
    // guest may legally send is its own mistranslation.
    op = VIV_FCNTL_UNSERVED; cx = false; minfd = 0xBADu;
    TEST_EXPECT_EQ((int)vivarium_fcntl_decide(VIV_F_SETFD, 0xFFFFu, &op, &cx, &minfd),
                   (int)VIV_TRANSLATED, "F_SETFD with stray bits is still served");
    TEST_EXPECT_EQ((u64)(cx ? 1 : 0), (u64)1, "the stray bits are masked, not rejected");

    // Only the low 32 bits of cmd are significant -- it is an `int`. A high-half
    // value whose low word is F_GETFD still means F_GETFD.
    op = VIV_FCNTL_UNSERVED; cx = true; minfd = 0xBADu;
    TEST_EXPECT_EQ((int)vivarium_fcntl_decide(0x1234567800000001ull, 0, &op, &cx,
                                              &minfd),
                   (int)VIV_TRANSLATED, "the high half of cmd is not consulted");
    TEST_EXPECT_EQ((int)op, (int)VIV_FCNTL_GETFD, "still GETFD");

    // The declines. Each is a cmd that EXISTS on Linux and is simply not here --
    // F_GETFL/F_SETFL (access + status flags), the locking family. A declined
    // call must leave the outputs at their unserved values, so a caller that
    // ignores the verdict cannot act on a plausible-looking op.
    static const u64 declined[] = { 3, 4, 5, 6, 7, 8, 9, 1024, 1033 };
    for (u32 i = 0; i < (u32)(sizeof(declined) / sizeof(declined[0])); i++) {
        op = VIV_FCNTL_DUPFD; cx = true; minfd = 0xBADu;
        TEST_EXPECT_EQ((int)vivarium_fcntl_decide(declined[i], 0, &op, &cx, &minfd),
                       (int)VIV_FORWARD, "an unserved cmd declines");
        TEST_EXPECT_EQ((int)op, (int)VIV_FCNTL_UNSERVED,
                       "a declined cmd leaves op UNSERVED, never a stale one");
        TEST_EXPECT_EQ((u64)(cx ? 1 : 0), (u64)0, "and clears cloexec");
        TEST_EXPECT_EQ(minfd, (u64)0, "and clears min_fd");
    }

    // Fail toward the decline on a bad call site, each output nulled in turn --
    // a guard that checked only the first would write through the others.
    TEST_EXPECT_EQ((int)vivarium_fcntl_decide(VIV_F_GETFD, 0, NULL, &cx, &minfd),
                   (int)VIV_FORWARD, "NULL op_out -> FORWARD");
    TEST_EXPECT_EQ((int)vivarium_fcntl_decide(VIV_F_GETFD, 0, &op, NULL, &minfd),
                   (int)VIV_FORWARD, "NULL cloexec_out -> FORWARD");
    TEST_EXPECT_EQ((int)vivarium_fcntl_decide(VIV_F_GETFD, 0, &op, &cx, NULL),
                   (int)VIV_FORWARD, "NULL min_fd_out -> FORWARD");
}

// -----------------------------------------------------------------------------
// vivarium.pipe2_domain (#155). The two admitted values are not a conservative
// guess -- they are what the gate's own busybox issues, measured off the binary:
// four call sites through musl's pipe() with a hardcoded `mov x1, #0`, and two
// through pipe2() with `mov w1, #0x80000`. Everything else declines.
// -----------------------------------------------------------------------------
void test_vivarium_pipe2_domain(void);
void test_vivarium_pipe2_domain(void) {
    bool cx;

    // flags 0 -- musl's pipe(), and on aarch64 the ONLY way a plain pipe() can
    // be spelled, since the generic syscall table has no legacy `pipe`.
    cx = true;
    TEST_EXPECT_EQ((int)vivarium_pipe2_decide(0, &cx), (int)VIV_TRANSLATED,
                   "flags 0 is served -- this IS pipe() on aarch64");
    TEST_EXPECT_EQ((u64)(cx ? 1 : 0), (u64)0, "and asks for no descriptor flag");

    // O_CLOEXEC -- served rather than declined, and the distinction is real:
    // musl's pipe2 would have recovered from a decline via its own ENOSYS
    // fallback (pipe + fcntl, both served since #151), so this row is about
    // being correct for callers that HAVE no such fallback, not about avoiding
    // breakage. #151 is what makes it representable at all.
    cx = false;
    TEST_EXPECT_EQ((int)vivarium_pipe2_decide(VIV_O_CLOEXEC, &cx),
                   (int)VIV_TRANSLATED, "O_CLOEXEC is served (#151 built the flag)");
    TEST_EXPECT_EQ((u64)(cx ? 1 : 0), (u64)1, "and is carried to the shell");

    // THE DECLINES, and each is a flag Linux's pipe2 genuinely accepts -- so
    // this is the allow-list doing its job, not an unreachable branch. Neither
    // has a devpipe counterpart: there is no packet framing and no non-blocking
    // read, so admitting either would tell a guest something false about the
    // pipe it just received. A declined call must also leave cloexec CLEAR, so
    // a caller that ignores the verdict cannot act on a stale true.
    static const u64 declined[] = {
        VIV_O_NONBLOCK,                     // no non-blocking read exists
        VIV_O_DIRECT,                       // no packet mode exists
        VIV_O_NONBLOCK | VIV_O_CLOEXEC,     // one admitted bit does not carry the other
        VIV_O_DIRECT   | VIV_O_CLOEXEC,
        1u,                                 // a bit no pipe2 flag uses at all
        0xFFFFFFFFu,
    };
    for (u32 i = 0; i < (u32)(sizeof(declined) / sizeof(declined[0])); i++) {
        cx = true;
        TEST_EXPECT_EQ((int)vivarium_pipe2_decide(declined[i], &cx),
                       (int)VIV_FORWARD, "a flag outside the domain declines");
        TEST_EXPECT_EQ((u64)(cx ? 1 : 0), (u64)0,
                       "and clears cloexec rather than leaving it stale");
    }

    // Only the low 32 bits are significant -- flags is an `int`, and a caller
    // may leave the register sign- or zero-extended. A high half must not turn
    // an admitted value into a declined one.
    cx = false;
    TEST_EXPECT_EQ((int)vivarium_pipe2_decide(0x1234567800080000ull, &cx),
                   (int)VIV_TRANSLATED, "the high half of flags is not consulted");
    TEST_EXPECT_EQ((u64)(cx ? 1 : 0), (u64)1, "and the low half still decides");

    // Fail toward the decline on a bad call site.
    TEST_EXPECT_EQ((int)vivarium_pipe2_decide(0, NULL), (int)VIV_FORWARD,
                   "NULL cloexec_out -> FORWARD");
}

void test_vivarium_dup3_domain(void);
void test_vivarium_dup3_domain(void) {
    bool cx;

    // flags 0 -- and on aarch64 this IS dup2(), which has no number of its own,
    // so it is the spelling every shell redirection reaches. Three of the four
    // measured call sites in the gate's busybox hardcode it.
    cx = true;
    TEST_EXPECT_EQ((int)vivarium_dup3_decide(0, &cx), (int)VIV_TRANSLATED,
                   "flags 0 is served -- this IS dup2 on aarch64");
    TEST_EXPECT_EQ((u64)(cx ? 1 : 0), (u64)0, "and asks for no descriptor flag");

    // O_CLOEXEC -- the fourth site is musl's __dup3, which passes the caller's
    // flags straight through, so this is reachable from any applet calling
    // dup3() directly.
    cx = false;
    TEST_EXPECT_EQ((int)vivarium_dup3_decide(VIV_O_CLOEXEC, &cx),
                   (int)VIV_TRANSLATED, "O_CLOEXEC is served (#151 built the flag)");
    TEST_EXPECT_EQ((u64)(cx ? 1 : 0), (u64)1, "and is carried to the shell");

    // THE REFUSALS -- and here is the difference from pipe2 worth pinning in a
    // test rather than only in a comment. pipe2's excluded flags (O_DIRECT,
    // O_NONBLOCK) are ones LINUX SERVES and we cannot, so declining them is a
    // real subset. dup3's accepted set is {0, O_CLOEXEC} and NOTHING ELSE --
    // ksys_dup3 answers EINVAL for the rest -- so this allow-list is EQUAL to
    // Linux's, and the shell must answer EINVAL rather than the ENOSYS decline.
    // The bare `1u` is the load-bearing case: it is what a deny-list built from
    // "reject O_DIRECT and O_NONBLOCK" would serve silently.
    static const u64 refused[] = {
        1u,                                 // a bit no dup3 flag uses at all
        VIV_O_NONBLOCK,                     // Linux's dup3 rejects it too
        VIV_O_DIRECT,
        VIV_O_NONBLOCK | VIV_O_CLOEXEC,     // one admitted bit does not carry the other
        VIV_O_DIRECT   | VIV_O_CLOEXEC,
        0xFFFFFFFFu,
    };
    for (u32 i = 0; i < (u32)(sizeof(refused) / sizeof(refused[0])); i++) {
        cx = true;
        TEST_EXPECT_EQ((int)vivarium_dup3_decide(refused[i], &cx),
                       (int)VIV_FORWARD, "a flag outside the domain is refused");
        TEST_EXPECT_EQ((u64)(cx ? 1 : 0), (u64)0,
                       "and clears cloexec rather than leaving it stale");
    }

    // Only the low 32 bits are significant, and this matters MORE here than for
    // pipe2: musl's __dup3 emits `sxtw x2, w2`, so a negative flags word really
    // does arrive with the whole high half set. A high half must not turn an
    // admitted value into a refused one.
    cx = false;
    TEST_EXPECT_EQ((int)vivarium_dup3_decide(0xFFFFFFFF00080000ull, &cx),
                   (int)VIV_TRANSLATED, "the high half of flags is not consulted");
    TEST_EXPECT_EQ((u64)(cx ? 1 : 0), (u64)1, "and the low half still decides");

    // Fail toward the refusal on a bad call site.
    TEST_EXPECT_EQ((int)vivarium_dup3_decide(0, NULL), (int)VIV_FORWARD,
                   "NULL cloexec_out -> FORWARD");
}

// The phenotype's fork/exec signal-state rule (task #127; ARCH 7.6, POSIX;
// operator-voted 2026-08-17): rfork COPIES the sigtab into the child's OWN
// table; execve resets CAUGHT rows to SIG_DFL and KEEPS SIG_IGN rows. This
// pins the two primitives where the table is directly observable (the in-guest
// legs L217-L228 drive them through a real fork + execve). Each leg names the
// row that would read wrong under the OLD behaviour (a full zero at exec; no
// copy at fork).
void test_vivarium_sigtab_fork_exec_rule(void);
void test_vivarium_sigtab_fork_exec_rule(void) {
    struct viv_sigtab *tab =
        (struct viv_sigtab *)kzalloc(sizeof(struct viv_sigtab), 0);
    TEST_ASSERT(tab != NULL, "sigtab alloc");
    struct viv_ksigaction ign = { .handler = VIV_SIG_IGN, .flags = 0,
                                  .restorer = 0, .mask = 0 };
    struct viv_ksigaction hnd = { .handler = 0x400000ull, .flags = 0x14000000ull,
                                  .restorer = 0x400100ull, .mask = 0x2ull };
    (void)viv_sigtab_set(tab, VIV_SIGNOTE_PIPE, &ign);        // ignored
    (void)viv_sigtab_set(tab, VIV_SIGNOTE_INTERRUPT, &hnd);   // caught
    (void)viv_sigtab_set(tab, VIV_SIGNOTE_TTY_HUP, &hnd);     // caught

    // ---- exec: caught -> SIG_DFL (flags/restorer/mask too); ignored KEPT.
    viv_sigtab_reset_caught(tab);
    struct viv_ksigaction got;
    TEST_ASSERT(!viv_sigtab_note_handler(tab, VIV_SIGNOTE_INTERRUPT, &got),
                "exec: the caught interrupt row is no longer a handler");
    TEST_ASSERT(tab->act[(u32)VIV_SIGNOTE_INTERRUPT].handler == VIV_SIG_DFL
                && tab->act[(u32)VIV_SIGNOTE_INTERRUPT].flags == 0
                && tab->act[(u32)VIV_SIGNOTE_INTERRUPT].restorer == 0
                && tab->act[(u32)VIV_SIGNOTE_INTERRUPT].mask == 0,
                "exec: the caught row is a clean SIG_DFL (flags/restorer/mask zeroed)");
    TEST_ASSERT(!viv_sigtab_note_handler(tab, VIV_SIGNOTE_TTY_HUP, &got),
                "exec: the second caught row reset too");
    TEST_ASSERT(viv_sigtab_note_ignored(tab, VIV_SIGNOTE_PIPE),
                "exec: SIG_IGN SURVIVES execve (POSIX) -- the row the old full zero lost");
    TEST_ASSERT(!viv_sigtab_note_ignored(tab, VIV_SIGNOTE_CHILD_EXIT),
                "exec: an untouched SIG_DFL row stays SIG_DFL");
    viv_sigtab_reset_caught(NULL);                             // NULL-safe

    // ---- fork: the child gets its OWN equal copy; a NULL parent table -> NULL.
    struct Proc *parent = proc_alloc();
    struct Proc *child  = proc_alloc();
    TEST_ASSERT(parent != NULL && child != NULL, "proc_alloc x2");
    parent->phenotype = PHENO_LINUX;
    child->phenotype  = PHENO_LINUX;
    (void)viv_sigtab_set(tab, VIV_SIGNOTE_INTERRUPT, &hnd);   // re-arm a caught row
    parent->sigtab = tab;
    TEST_EXPECT_EQ(viv_sigtab_clone_into(child, parent), 0, "fork: clone succeeds");
    TEST_ASSERT(child->sigtab != NULL && child->sigtab != parent->sigtab,
                "fork: the child has its OWN table (never the parent's pointer)");
    bool equal = true;
    for (u32 i = 0; i < (u32)VIV_SIGNOTE_COUNT; i++) {
        if (child->sigtab->act[i].handler  != tab->act[i].handler  ||
            child->sigtab->act[i].flags    != tab->act[i].flags    ||
            child->sigtab->act[i].restorer != tab->act[i].restorer ||
            child->sigtab->act[i].mask     != tab->act[i].mask) equal = false;
    }
    TEST_ASSERT(equal, "fork: every row copied -- caught AND ignored (POSIX fork(2))");
    TEST_ASSERT(viv_sigtab_note_ignored(child->sigtab, VIV_SIGNOTE_PIPE)
                && viv_sigtab_note_handler(child->sigtab, VIV_SIGNOTE_INTERRUPT, &got)
                && got.handler == 0x400000ull,
                "fork: the child reads the ignored PIPE and the caught INTERRUPT");
    // The copy is a SNAPSHOT: a later change on the parent does not reach the child.
    (void)viv_sigtab_set(tab, VIV_SIGNOTE_PIPE, &hnd);
    TEST_ASSERT(viv_sigtab_note_ignored(child->sigtab, VIV_SIGNOTE_PIPE),
                "fork: the child's table is independent of later parent changes");
    struct Proc *child2 = proc_alloc();
    struct Proc *bare   = proc_alloc();
    TEST_ASSERT(child2 != NULL && bare != NULL, "proc_alloc x2 (bare)");
    bare->phenotype = PHENO_LINUX;
    child2->phenotype = PHENO_LINUX;
    TEST_EXPECT_EQ(viv_sigtab_clone_into(child2, bare), 0, "fork: a NULL parent table succeeds");
    TEST_ASSERT(child2->sigtab == NULL, "fork: ...and leaves the child's NULL (all-SIG_DFL)");
    TEST_EXPECT_EQ(viv_sigtab_clone_into(NULL, bare), -1, "fork: NULL child refused");

    // proc_free owns each table (the immortal-per-Proc rule): parent frees tab.
    parent->state = PROC_STATE_ZOMBIE; proc_free(parent);
    child->state  = PROC_STATE_ZOMBIE; proc_free(child);
    child2->state = PROC_STATE_ZOMBIE; proc_free(child2);
    bare->state   = PROC_STATE_ZOMBIE; proc_free(bare);
}

extern s64 viv_fcntl_for_test(struct Proc *p, u64 fd, u64 cmd, u64 arg);
// The T2 fcntl SHELL's F_DUPFD errnos (found by the c8ab2744 close's L-6c legs).
//
// The decide half above is pure; this drives the arm that turns a decision into
// an errno, because that is where the defect lived: handle_dup_posix folds
// "no such fd" and "table full" into one -1, and the arm answered EMFILE for
// both. busybox ash's redirect() probes the TARGET fd of every `N>&M` with
// fcntl(N, F_DUPFD, 10) precisely to learn whether N is open -- EBADF means
// "not open, nothing to save", ANY other errno is "strange" and aborts the
// command. fd 3 is not open in the L-6c gate's shell, so every `3>&1` died with
// `fcntl(3,F_DUPFD,10): No file descriptors available`, the command
// substitution around it yielded "", and the two legs asserting an EMPTY
// capture passed VACUOUSLY -- only the positive control (L6C-K) said no.
//
// Driven through viv_fcntl_for_test (the real arm, on a fresh Proc: no
// exception frame, which the FCNTL case never reads).
void test_vivarium_fcntl_dupfd_errnos(void);
void test_vivarium_fcntl_dupfd_errnos(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc");
    p->phenotype = PHENO_LINUX;

    // ---- POSITIVE CONTROL: a live fd dups at or above the minimum. ------
    // Every negative below is an errno; a shell whose DUPFD arm was wired to
    // nothing would answer them all with the same wrong number. This proves the
    // arm reaches handle_dup_posix and honours the minimum first.
    hidx_t live = handle_alloc(p, KOBJ_PROCESS, RIGHT_READ | RIGHT_TRANSFER, p);
    s64 ctl = viv_fcntl_for_test(p, (u64)live, VIV_F_DUPFD, 10);

    // ---- LEG A: F_DUPFD on a CLOSED fd is EBADF -- the ash probe. ---------
    // fd 3 is closed in a fresh Proc (the control's source took slot 0 and its
    // dup landed at or above 10); asserted, not assumed.
    int a_closed = handle_get_cloexec(p, 3);
    s64 a = viv_fcntl_for_test(p, 3, VIV_F_DUPFD, 10);
    // The CLOEXEC spelling is the same op with the flag on -- same answer.
    s64 a2 = viv_fcntl_for_test(p, 3, VIV_F_DUPFD_CLOEXEC, 10);

    // ---- LEG B: F_DUPFD on a LIVE fd with a FULL table is EMFILE. ---------
    // Fill every free slot, prove fullness two ways (the count and a live
    // alloc now refused), then ask.
    int filled = 0;
    while (handle_alloc(p, KOBJ_THREAD, RIGHT_READ, NULL) >= 0) filled++;
    int b_count = handle_table_count(p->handles);
    hidx_t b_refused = handle_alloc(p, KOBJ_THREAD, RIGHT_READ, NULL);
    s64 b = viv_fcntl_for_test(p, (u64)live, VIV_F_DUPFD, 0);
    // (Leg A had to run BEFORE the fill: full means every index is live, so
    // no closed fd exists to probe afterwards.)

    // ---- LEG C: a minimum at or past the table is EINVAL (Linux: RLIMIT). --
    s64 c = viv_fcntl_for_test(p, (u64)live, VIV_F_DUPFD, (u64)PROC_HANDLE_MAX);

    // ---- TEARDOWN, then assert. ---------------------------------------
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);                       // closes every straggler handle

    TEST_ASSERT(live >= 0, "control precondition: a live fd was allocated");
    TEST_ASSERT(ctl >= 10,
                "CONTROL: F_DUPFD(live, 10) lands at or above 10 -- the arm is "
                "wired and honours the minimum");
    TEST_EXPECT_EQ(a_closed, -1, "A precondition: fd 3 is CLOSED");
    TEST_EXPECT_EQ(a, -(s64)T_E_BADF,
                   "A: F_DUPFD on a closed fd is EBADF -- ash's `N>&M` probe "
                   "reads any other errno as fatal (pre-fix: EMFILE)");
    TEST_EXPECT_EQ(a2, -(s64)T_E_BADF,
                   "A: F_DUPFD_CLOEXEC on a closed fd is EBADF too");
    TEST_ASSERT(filled > 0, "B precondition: the fill allocated something");
    TEST_EXPECT_EQ(b_count, PROC_HANDLE_MAX, "B precondition: the table is FULL");
    TEST_EXPECT_EQ(b_refused, -1, "B precondition: a live alloc is now refused");
    TEST_EXPECT_EQ(b, -(s64)T_E_MFILE,
                   "B: F_DUPFD on a LIVE fd with a full table is EMFILE -- the "
                   "two errnos are DISTINCT (a fix that always said EBADF "
                   "would fail here)");
    TEST_EXPECT_EQ(c, -(s64)T_E_INVAL,
                   "C: a minimum at the table size is EINVAL, not EMFILE");
}
