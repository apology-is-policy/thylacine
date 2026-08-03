// VIVARIUM V-2 — the Linux syscall translation table (docs/VIVARIUM.md §4/§5.3).
//
// This header + kernel/vivarium.c are the in-kernel half of Option C (the
// hybrid, user-voted 2026-07-23): the Linux calls whose translation is TOTAL and
// STATELESS are decoded here by table; everything else forwards to the userspace
// supervisor or fails ENOSYS.
//
// WHY A TABLE AND NOT LOGIC. §4's whole defence of Option C is that "the
// in-kernel part is a TABLE, not logic — auditable by inspection, and it adds no
// new kernel *semantics*, only a decode." That is a real constraint on this file,
// not a description of it: `vivarium_translate` is PURE — it takes a syscall
// number and six argument words, and returns a verdict plus a rewritten call. It
// touches no Proc, no handle table, no user memory, no lock, and allocates
// nothing. It is unit-testable with zero kernel plumbing, and that is the point.
//
// THE ADMISSION RULE (§4, binding). A Linux call may occupy a TABLE row iff its
// translation is *total and stateless*: a pure renumber plus an argument-order or
// flag-bit mapping onto exactly one existing `sys_*_for_proc`, with **no new
// kernel state, no new error semantics, and no policy**. The instant a call needs
// state the kernel does not already own — socket tables, signal dispositions,
// /proc content, ioctl dispatch — it forwards.
//
// "TOTAL" IS THE WORD THAT DOES THE WORK, and it is easy to misread as "the
// arguments line up". It does not mean that. It means the translation is correct
// for EVERY input the Linux call accepts. The worked counterexample is `munmap`:
// Linux `munmap(addr, len)` and `SYS_BURROW_DETACH(vaddr, length)` take the same
// two words in the same order and look like a free row — but burrow_detach
// requires an EXACT VMA match ("no partial detach at v1.0", syscall.h:611) while
// Linux explicitly permits partial and multi-mapping unmaps. The renumber would
// therefore be silently wrong for a legal class of inputs, so `munmap` is NOT a
// table row. Arguments aligning is not semantics aligning; check the contract.
//
// WHAT IS DELIBERATELY ABSENT. Nothing here is wired into `syscall_dispatch`.
// V-1a landed `Proc.phenotype` but NOTHING can set it to PHENO_LINUX — verified:
// `exec.c` never touches the field, the only assignment in the tree is the rfork
// inherit, and `PHENO_LINUX` is referenced nowhere outside its own enum. So the
// dispatch branch would today be branching on a field that is provably always 0.
// The branch + the declaration that makes it reachable are V-1b, and the
// declaration's correct SHAPE depends on V-7's container object (§5.2: "the fused
// container+phenotype object is the right granularity"; every peer system except
// FreeBSD has the CONTAINER declare). Building the table first is the reversible
// half: a table is data and can be rewritten freely, whereas a syscall signature
// is append-only ABI.

#ifndef THYLACINE_VIVARIUM_H
#define THYLACINE_VIVARIUM_H

#include <thylacine/types.h>
// For `struct pollfd` + POLL_MAX_NFDS: the pselect6 translator's OUTPUT type is
// the native pollfd, so the ABI it converts INTO belongs in this header's view.
#include <thylacine/poll.h>

// `struct t_stat` is forward-declared rather than pulled in from <syscall.h>.
// A pointer parameter needs no definition, and V-1b will make syscall.c the
// dispatcher that calls into here -- so keeping the dependency one-way now
// removes an include cycle before it can exist.
struct t_stat;

// A Linux aarch64 syscall carries at most 6 argument words (x0..x5).
#define VIV_NARGS 6

// The verdict of a translation attempt.
enum viv_verdict {
    // A TABLE row matched: dispatch `out->nr` with `out->args` natively.
    VIV_TRANSLATED = 0,

    // Total-and-stateless does not hold: the call needs state, policy, or
    // judgement the kernel does not already own. V-3's userspace supervisor
    // handles it. Until V-3 exists the caller's disposition is its own choice —
    // this function only classifies.
    VIV_FORWARD = 1,

    // No Thylacine counterpart exists at all (`brk`: the heap is Burrow-based,
    // there is no break pointer to move). Honest ENOSYS, not a silent lie.
    VIV_ENOSYS = 2,

    // A TIER-2 translator exists for this number: the call is admissible, but
    // the translation is not a renumber, so `vivarium_translate` cannot perform
    // it and leaves `out` untouched. The dispatcher must invoke the named
    // translator below. See "TIER 2" for why these are separate functions.
    VIV_TIER2 = 3,
};

// A translated call: the Thylacine syscall number + its argument vector.
struct viv_call {
    u64 nr;
    u64 args[VIV_NARGS];
};

// Translate one Linux aarch64 syscall.
//
// PURE: no Proc, no uaccess, no locks, no allocation, no globals touched. Safe to
// call from anywhere, including a unit test with a synthetic argument vector.
//
// `args_in` must point to VIV_NARGS words. `out` is written only when the return
// is VIV_TRANSLATED; on FORWARD/ENOSYS it is left untouched, so a caller that
// ignores the verdict cannot accidentally dispatch a garbage number.
//
// A NULL `args_in` or `out` yields VIV_ENOSYS (fail closed, never a dispatch).
enum viv_verdict vivarium_translate(u64 linux_nr, const u64 *args_in,
                                    struct viv_call *out);

// The Linux aarch64 numbers this table knows. Named so the tests assert against
// symbols rather than magic numbers, and so a future row addition is a one-line
// diff next to its number. (Linux's aarch64 table is stable ABI — these values
// are fixed for all time by the Linux kernel's own compatibility promise.)
enum {
    // THE fd-FREEING SET. `close` is the only one that is a row, and that is a
    // PRECONDITION of the socktab close hook rather than a coincidence -- see
    // the hook in viv_linux_dispatch and vivarium.fd_freeing_rows_stay_unserved.
    VIV_LINUX_DUP         = 23,
    VIV_LINUX_DUP3        = 24,
    VIV_LINUX_CLOSE_RANGE = 436,

    VIV_LINUX_OPENAT     = 56,
    VIV_LINUX_CLOSE      = 57,
    VIV_LINUX_LSEEK      = 62,
    VIV_LINUX_READ       = 63,
    VIV_LINUX_WRITE      = 64,
    VIV_LINUX_NEWFSTATAT = 79,
    VIV_LINUX_FSTAT      = 80,
    VIV_LINUX_BRK        = 214,
    VIV_LINUX_MUNMAP     = 215,
    VIV_LINUX_MMAP       = 222,
    VIV_LINUX_MPROTECT   = 226,
    VIV_LINUX_STATX      = 291,
    VIV_LINUX_EXIT_GROUP = 94,

    // The signal family (V-6, §6.22). Contiguous in Linux's aarch64 table.
    VIV_LINUX_RESTART_SYSCALL = 128,
    VIV_LINUX_KILL            = 129,
    VIV_LINUX_TKILL           = 130,
    VIV_LINUX_TGKILL          = 131,
    VIV_LINUX_SIGALTSTACK     = 132,
    VIV_LINUX_RT_SIGSUSPEND   = 133,
    VIV_LINUX_RT_SIGACTION    = 134,
    VIV_LINUX_RT_SIGPROCMASK  = 135,
    VIV_LINUX_RT_SIGPENDING   = 136,
    VIV_LINUX_RT_SIGTIMEDWAIT = 137,
    VIV_LINUX_RT_SIGQUEUEINFO = 138,
    VIV_LINUX_RT_SIGRETURN    = 139,

    // The socket family (V-5, section 5.5). Contiguous in Linux's aarch64
    // table; aarch64 has no socketcall multiplexer, so each is its own number.
    // All are above THE NATIVE CEILING (see below), so the collision re-check
    // the ARCH section 25.4 row mandates is discharged by construction for
    // every row here.
    VIV_LINUX_SOCKET      = 198,
    VIV_LINUX_SOCKETPAIR  = 199,
    VIV_LINUX_BIND        = 200,
    VIV_LINUX_LISTEN      = 201,
    VIV_LINUX_ACCEPT      = 202,
    VIV_LINUX_CONNECT     = 203,
    VIV_LINUX_GETSOCKNAME = 204,
    VIV_LINUX_GETPEERNAME = 205,
    VIV_LINUX_SENDTO      = 206,
    VIV_LINUX_RECVFROM    = 207,
    VIV_LINUX_SETSOCKOPT  = 208,
    VIV_LINUX_GETSOCKOPT  = 209,
    VIV_LINUX_SHUTDOWN    = 210,
    VIV_LINUX_SENDMSG     = 211,
    VIV_LINUX_RECVMSG     = 212,
    VIV_LINUX_ACCEPT4     = 242,

    // Readiness (V-5c, section 5.5.4). aarch64 has NO plain poll(2) or
    // select(2) -- the generic ABI dropped them -- so these two ARE the poll
    // family, and musl's poll()/select() are thin wrappers over them.
    //
    // THE COLLISION RE-CHECK, WHICH FINALLY HAS WORK TO DO. Every row above is
    // over the native ceiling, so the ARCH section 25.4 mandate was discharged
    // by construction. These two are NOT: 72 is SYS_GETPID and 73 is
    // SYS_GETUID. So the argument has to be made per number, and it still
    // holds -- but for a different reason:
    //
    //   * A PHENO_LINUX Proc CANNOT REACH A NATIVE NUMBER AT ALL. Every number
    //     it issues goes through vivarium_translate, and an unclassified one
    //     lands on FORWARD -> ENOSYS (viv_linux_dispatch). It never had getpid
    //     or getuid to lose, and adding these rows takes nothing away.
    //   * A NATIVE program MIS-DECLARED as PHENO_LINUX issuing native 72
    //     intending getpid now dispatches the pselect6 translator over
    //     getpid's (absent, hence garbage) arguments. The translator reads
    //     user memory the caller owns, bounds-checked, and polls fds the
    //     caller already holds -- so the worst outcome is EFAULT, a block, or
    //     (V-5d F4) a BOUNDED WRITE of up to three 32-byte fd_set results into
    //     addresses the caller's own registers named, which is still the
    //     caller's own address space and still confers nothing;
    //     never authority. A mis-declared Proc is comprehensively broken
    //     either way; I-43 is about what it can REACH, not whether it works.
    //
    // A future row below 100 owes the same paragraph. Do not reach for the
    // ceiling argument again without checking the number.
    VIV_LINUX_PSELECT6    = 72,
    VIV_LINUX_PPOLL       = 73,

    // Process creation (LINEAGE L-3d, docs/LINEAGE.md §7). Above the native
    // ceiling, so the collision re-check is discharged by construction -- and
    // this time the ceiling was CHECKED rather than quoted: it had moved from
    // 100 to 102 when SYS_EXECVE and SYS_RFORK landed at L-2a/L-3b, and four
    // separate comments still said 100 (one still said the pre-#97 "256,
    // sparsely"). That is why VIV_NATIVE_CEILING below exists as a symbol
    // rather than as a number repeated in prose.
    VIV_LINUX_CLONE       = 220,
    VIV_LINUX_EXECVE      = 221,
    VIV_LINUX_WAIT4       = 260,

    // The startup batch (#150) -- the set a real Linux binary issues between
    // _start and its first useful instruction. Measured, not guessed: this is
    // exactly the census `viv_report_unserved` printed the moment #149's loader
    // fix let Alpine's busybox execute, minus the two rows that are declined on
    // purpose (brk 214, mprotect 226).
    //
    // FOUR OF THEM ARE BELOW THE NATIVE CEILING, so each owes the per-number
    // collision paragraph the pselect6/ppoll block above demands -- and this is
    // the first time that obligation has had to be discharged for more than two
    // numbers at once. The first half of the argument is shared: a PHENO_LINUX
    // Proc cannot reach a native number AT ALL (every number it issues goes
    // through vivarium_translate; an unclassified one lands on FORWARD ->
    // ENOSYS), so it never had the native call to lose. The second half -- what
    // a NATIVE program MIS-DECLARED as PHENO_LINUX now reaches -- is per number:
    //
    //   17  vs SYS_SET_DUMPABLE(dumpable). getcwd would write the cwd into
    //       args[0], which is the dumpable flag: a VA of 0 or 1. That fails
    //       sys_validate_user_buf, so the outcome is ERANGE/EFAULT and no write.
    //   25  vs SYS_SPAWN_FULL(...). fcntl is ENOSYS below -- there is no shell
    //       to mis-dispatch INTO, so this row costs a mis-declared Proc exactly
    //       an ENOSYS.
    //   66  vs SYS_LOOM_SETUP(entries, params_va). writev would read args[1] as
    //       an iovec array and write to fd args[0]. Both are the caller's OWN --
    //       its own memory, bounds-checked, and an fd it already holds -- so the
    //       worst case is EFAULT/EBADF or a write of the caller's bytes to the
    //       caller's fd. Never authority.
    //   96  vs SYS_TTY_SET_FG(fd, pgid). set_tid_address would store args[0] --
    //       a small fd number -- as clear_child_tid. It is validated (4-byte
    //       aligned, under UACCESS_USER_VA_TOP), and the exit-time store through
    //       it is a uaccess_store_u32 with a fixup entry, so an unmapped VA 4
    //       faults into the fixup and is swallowed. The caller's own address
    //       space, and nothing else.
    //
    // The remaining seven are above the ceiling and discharge by construction.
    VIV_LINUX_GETCWD          = 17,
    VIV_LINUX_FCNTL           = 25,
    VIV_LINUX_WRITEV          = 66,
    VIV_LINUX_SET_TID_ADDRESS = 96,
    VIV_LINUX_SETGID          = 144,
    VIV_LINUX_SETUID          = 146,
    VIV_LINUX_UNAME           = 160,
    VIV_LINUX_GETPID          = 172,
    VIV_LINUX_GETPPID         = 173,
    VIV_LINUX_GETUID          = 174,
    VIV_LINUX_GETGID          = 176,
};

// The highest ASSIGNED native Thylacine syscall number. Every vivarium row
// above this is free of collision by construction; the two rows below it
// (pselect6 72, ppoll 73) carry their own per-number argument, above.
//
// THE OBLIGATION, and the reason this is a symbol: a new native syscall above
// 102 makes the ceiling argument stop holding for every row at or below the new
// value, SILENTLY. Bumping this constant is therefore part of adding a syscall,
// and the `_Static_assert` in vivarium.c pins it to SYS_RFORK's identity so a
// renumber of the current top cannot drift unnoticed. (It cannot catch a NEW
// higher number on its own -- C has no max-over-an-enum -- so the rows that
// depend on the ceiling assert against it individually there.)
//
// Stated ONCE, deliberately. It was previously written out as a literal in four
// places and was stale in all four.
#define VIV_NATIVE_CEILING 102

// -----------------------------------------------------------------------------
// TIER 2 — translators (V-2b).
//
// A T1 row is a renumber, so one table plus one loop serves every row. A T2 call
// is a real translation — a flag map, an argument reshape, a struct conversion —
// so each gets its own named function. They are still PURE, and that is a
// deliberate constraint, not an accident of what happened to be easy:
//
//   * `openat` needs the path's LENGTH, which lives in user memory. Rather than
//     let uaccess into this file, the measurement is HOISTED OUT to the caller:
//     `vivarium_openat_decide` makes the whole decision without touching memory,
//     and `vivarium_openat_build` assembles the call from a length the caller
//     measured. The part that needs a kernel is a strnlen; the part that needs
//     REVIEW is pure and unit-tested.
//   * `fstat` converts an 88-byte `struct t_stat` into the 128-byte Linux
//     aarch64 `struct stat`. Both structs are plain data, so the conversion is
//     pure; the shell around it (`spoor_stat_native` into a kernel t_stat, then
//     one 128-byte copy-out) touches no translation logic at all.
//
// WHY THE DECIDE/BUILD SPLIT IS NOT CEREMONY. Measuring the path is a user-memory
// read that can fault. Doing it before knowing whether the call is even
// translatable would (a) waste the read for every forwarded call and (b) let a
// call that we are going to hand to the supervisor anyway take a fault HERE,
// inside the kernel's fast path, on a buffer the supervisor would have validated
// itself. Deciding first and measuring second is the correct order, so the API
// is shaped to make the wrong order awkward.
//
// THE ARGUMENT DOMAIN — a refinement of §4, and the important part of V-2b.
// §4 admits "a pure renumber plus an argument-order/flag-bit mapping". A flag map
// is inherently PARTIAL: `openat` accepts flags (O_CREAT, O_DIRECTORY, O_APPEND)
// that SYS_OPEN has no way to honour. So a T2 row is admitted over a **stated
// argument domain**, and a call outside that domain FORWARDS. This is not a
// loosening of "total" — it is stricter in practice, because it replaces
// "openat is a table row" (which §4's illustrative list implies) with a
// per-call check. The soundness property is the one that matters:
//
//     the translator NEVER silently mistranslates; it either produces an
//     exactly-equivalent call or declines.
//
// Declining is always safe (the supervisor is strictly more capable). Accepting
// a flag we cannot honour is the failure mode this whole tier exists to avoid.
// -----------------------------------------------------------------------------

// Linux aarch64 open flags, in OCTAL so they can be diffed line-for-line against
// `third_party/musl/arch/aarch64/bits/fcntl.h` (they were taken from it, not from
// memory). O_RDONLY/WRONLY/RDWR come from musl's generic `include/fcntl.h`.
enum {
    VIV_O_RDONLY    = 00,
    VIV_O_WRONLY    = 01,
    VIV_O_RDWR      = 02,
    VIV_O_ACCMODE   = 03,

    VIV_O_CREAT     = 0100,
    VIV_O_EXCL      = 0200,
    VIV_O_NOCTTY    = 0400,
    VIV_O_TRUNC     = 01000,
    VIV_O_APPEND    = 02000,
    VIV_O_NONBLOCK  = 04000,
    VIV_O_DSYNC     = 010000,
    VIV_O_ASYNC     = 020000,
    VIV_O_DIRECTORY = 040000,
    VIV_O_NOFOLLOW  = 0100000,
    VIV_O_DIRECT    = 0200000,
    VIV_O_LARGEFILE = 0400000,
    VIV_O_NOATIME   = 01000000,
    VIV_O_CLOEXEC   = 02000000,
    VIV_O_SYNC      = 04010000,
    VIV_O_PATH      = 010000000,
    VIV_O_TMPFILE   = 020040000,
};

// Linux's "resolve relative to the process cwd" dirfd (musl `fcntl.h`: -100).
// Compared as a SIGNED 32-BIT value, because Linux passes `dirfd` as an `int`:
// a caller may leave x0 either sign-extended (0xFFFFFFFFFFFFFF9C) or merely
// zero-extended (0x00000000FFFFFF9C), and both must be recognised.
#define VIV_AT_FDCWD (-100)

// The `flags` word the *at() family carries (musl `include/fcntl.h`). Distinct
// from the O_* space above: these qualify the RESOLUTION, not the open.
enum {
    VIV_AT_SYMLINK_NOFOLLOW = 0x100,
    VIV_AT_REMOVEDIR        = 0x200,
    VIV_AT_SYMLINK_FOLLOW   = 0x400,
    VIV_AT_NO_AUTOMOUNT     = 0x800,
    VIV_AT_EMPTY_PATH       = 0x1000,
};

// Decide whether an `openat` is inside the translatable domain, and compute the
// two rewritten arguments. PURE — no user memory, no Proc, no locks.
//
//   `dirfd` / `flags` are the raw x0 / x2 register values; only their low 32
//   bits are significant (both are `int` in the Linux ABI).
//
// Returns VIV_TRANSLATED with *start_fd_out and *omode_out set, or VIV_FORWARD
// (outputs untouched) for anything outside the domain. Never ENOSYS: `openat`
// exists — an out-of-domain call is one the supervisor should serve, not one to
// deny.
enum viv_verdict vivarium_openat_decide(u64 dirfd, u64 flags,
                                        u64 *start_fd_out, u32 *omode_out);

// Assemble the SYS_OPEN call from a decision plus a caller-measured path length.
// Trivial by design — its value is that SYS_OPEN's argument ORDER is stated in
// exactly one place, and that place is covered by a test.
void vivarium_openat_build(u64 start_fd, u64 path_va, u32 path_len, u32 omode,
                           struct viv_call *out);

// The Linux aarch64 `struct stat` (128 bytes) — `include/uapi/asm-generic/stat.h`,
// which `third_party/musl/arch/aarch64/bits/stat.h` reproduces field-for-field.
// Spelled with explicit-width members rather than musl's typedefs so the layout is
// readable without chasing `alltypes.h`; the aarch64 overrides that make it work
// out are `blksize_t = int` and `nlink_t = unsigned int`.
//
// This is an ABI type: the kernel writes sizeof(*this) bytes into a Linux guest's
// buffer, so the offsets are pinned below exactly as `struct t_stat`'s are.
struct viv_linux_stat {
    u64 st_dev;         //   0
    u64 st_ino;         //   8
    u32 st_mode;        //  16
    u32 st_nlink;       //  20
    u32 st_uid;         //  24
    u32 st_gid;         //  28
    u64 st_rdev;        //  32
    u64 __pad1;         //  40
    s64 st_size;        //  48
    s32 st_blksize;     //  56
    s32 __pad2;         //  60
    s64 st_blocks;      //  64
    s64 st_atime_sec;   //  72
    u64 st_atime_nsec;  //  80
    s64 st_mtime_sec;   //  88
    u64 st_mtime_nsec;  //  96
    s64 st_ctime_sec;   // 104
    u64 st_ctime_nsec;  // 112
    u32 __unused4;      // 120
    u32 __unused5;      // 124
};

_Static_assert(sizeof(struct viv_linux_stat) == 128,
               "struct viv_linux_stat is the Linux aarch64 stat ABI -- pinned at 128 "
               "bytes. The kernel writes sizeof() bytes into a Linux guest's buffer, "
               "so a drifted layout corrupts the guest's stack.");
_Static_assert(__builtin_offsetof(struct viv_linux_stat, st_dev)        ==   0, "st_dev @0");
_Static_assert(__builtin_offsetof(struct viv_linux_stat, st_ino)        ==   8, "st_ino @8");
_Static_assert(__builtin_offsetof(struct viv_linux_stat, st_mode)       ==  16, "st_mode @16");
_Static_assert(__builtin_offsetof(struct viv_linux_stat, st_nlink)      ==  20, "st_nlink @20");
_Static_assert(__builtin_offsetof(struct viv_linux_stat, st_uid)        ==  24, "st_uid @24");
_Static_assert(__builtin_offsetof(struct viv_linux_stat, st_gid)        ==  28, "st_gid @28");
_Static_assert(__builtin_offsetof(struct viv_linux_stat, st_rdev)       ==  32, "st_rdev @32");
_Static_assert(__builtin_offsetof(struct viv_linux_stat, st_size)       ==  48, "st_size @48");
_Static_assert(__builtin_offsetof(struct viv_linux_stat, st_blksize)    ==  56, "st_blksize @56");
_Static_assert(__builtin_offsetof(struct viv_linux_stat, st_blocks)     ==  64, "st_blocks @64");
_Static_assert(__builtin_offsetof(struct viv_linux_stat, st_atime_sec)  ==  72, "st_atim.tv_sec @72");
_Static_assert(__builtin_offsetof(struct viv_linux_stat, st_atime_nsec) ==  80, "st_atim.tv_nsec @80");
_Static_assert(__builtin_offsetof(struct viv_linux_stat, st_mtime_sec)  ==  88, "st_mtim.tv_sec @88");
_Static_assert(__builtin_offsetof(struct viv_linux_stat, st_mtime_nsec) ==  96, "st_mtim.tv_nsec @96");
_Static_assert(__builtin_offsetof(struct viv_linux_stat, st_ctime_sec)  == 104, "st_ctim.tv_sec @104");
_Static_assert(__builtin_offsetof(struct viv_linux_stat, st_ctime_nsec) == 112, "st_ctim.tv_nsec @112");

// Convert a Thylacine `struct t_stat` into the Linux aarch64 `struct stat`.
// PURE — plain data in, plain data out; the caller owns both buffers.
//
// `out` is fully written (every reserved/pad word zeroed), so a stale kernel
// stack frame cannot leak into a guest through the gaps. That is an I-13
// obligation, not tidiness.
void vivarium_stat_to_linux(const struct t_stat *in, struct viv_linux_stat *out);

// Decide whether a `newfstatat` is inside the translatable domain (V-2c).
// PURE, and the smallest translator here: it returns a verdict and NOTHING else.
//
// That emptiness is the finding. `openat` had to compute a rewritten start_fd
// because SYS_OPEN TAKES one; SYS_STAT does not take a base at all — it is
// hardcoded to "absolute from the Territory root, relative joined with the LS-4
// cwd" (syscall.h:1605), which IS the AT_FDCWD rule. `sys_stat_for_proc` and
// `sys_open_handler` perform that join with the same `territory_join_cwd`
// call, so the correspondence is one implementation, not two that agree.
//
// The consequence cuts both ways: AT_FDCWD is free, and a REAL dirfd is not
// merely unimplemented but INEXPRESSIBLE — there is no argument to put it in.
//
// WHY THERE IS NO `vivarium_fstatat_build`. `openat` gets one because its
// translation ends in a native SYS_OPEN the dispatcher can run. This one cannot:
// SYS_STAT copies out an 88-byte `struct t_stat`, and the guest's buffer wants
// the 128-byte Linux layout, so dispatching SYS_STAT at the guest's pointer
// would write the wrong struct into it. The shell must instead call
// `sys_stat_for_proc` into a KERNEL t_stat, run it through
// `vivarium_stat_to_linux`, and copy out 128 bytes — exactly `fstat`'s shape.
// So `newfstatat` is `openat`'s front half joined to `fstat`'s back half, and
// the missing build function is what that join looks like.
enum viv_verdict vivarium_fstatat_decide(u64 dirfd, u64 flags);

// -----------------------------------------------------------------------------
// TIER 2 — `mmap` (V-2d). See VIVARIUM.md §6.21.
//
// V-2a classified `mmap` FORWARD as "the 'needs judgement' case the rule exists
// to exclude". §4.1 changed what FORWARD costs: V-3 is deferred, so FORWARD now
// means ENOSYS, and `mmap` is on musl's critical path twice over
// (`__init_tls.c:137` for TLS; mallocng for every heap area). A guest cannot
// reach main() without it. So it is promoted the same way `openat` was — over a
// STATED ARGUMENT DOMAIN, which is the tool V-2b already built.
//
// The target is SYS_BURROW_ATTACH_LAZY: it takes a length, picks the address,
// and produces a demand-zero RW/XN anonymous region.
//
// THE PROTECTION QUESTION, and why the strict answer loses. Thylacine anonymous
// memory is ALWAYS RW/XN and there is NO prot-mutation syscall anywhere — that
// is an I-12 design choice, not a gap. So this row cannot honour PROT_NONE or a
// read-only PROT_READ exactly; it grants read+write regardless.
//
// Declining every prot but PROT_READ|PROT_WRITE would be the letter of §6.19's
// "never silently mistranslate". But PROT_NONE is the DOMINANT anonymous shape
// in musl -- the thread guard page (pthread_create.c:295) and mallocng's meta
// areas (malloc.c:82) -- so declining it means malloc never initialises and
// nothing runs at all. Admitting it is a STATED FIDELITY DEGRADATION, and musl
// itself is the evidence that it is the sanctioned one:
//
//     mallocng/malloc.c:92 -- if (mprotect(p, pagesize, PROT_READ|PROT_WRITE)
//                                 && errno != ENOSYS) return 0;
//
// The libc ANTICIPATES a system with no mprotect and proceeds on the assumption
// that the PROT_NONE mapping is already usable, which is exactly what Thylacine
// produces. The consequence is named in VIVARIUM.md §9's DEGRADED tier rather
// than buried here: guard pages are NOT protective under the Linux phenotype,
// and a PROT_READ anonymous mapping is writable. It costs FIDELITY, never
// AUTHORITY -- the pages are the guest's own, every gate is unchanged, and
// nothing crosses a Proc boundary, so I-43 is untouched.
//
// PROT_EXEC is the hard line and is REFUSED, not degraded. An executable
// anonymous mapping is what CAP_JIT / I-42 governs (JIT-ON-WX-DESIGN.md), and
// W^X (I-12) forbids the RW-and-X region the naive translation would produce.
// The admission is therefore an ALLOW-LIST of two bits rather than "everything
// except PROT_EXEC" -- measured, aarch64 musl also defines PROT_BTI/PROT_MTE and
// generic musl PROT_GROWSDOWN/PROT_GROWSUP, none of which we can honour either.
// -----------------------------------------------------------------------------

// Linux `prot` bits. Generic musl `include/sys/mman.h`, plus the two aarch64
// additions from `arch/aarch64/bits/mman.h` -- read from the tree, not recalled.
enum {
    VIV_PROT_NONE       = 0,
    VIV_PROT_READ       = 1,
    VIV_PROT_WRITE      = 2,
    VIV_PROT_EXEC       = 4,
    VIV_PROT_BTI        = 0x10,        // aarch64
    VIV_PROT_MTE        = 0x20,        // aarch64
    VIV_PROT_GROWSDOWN  = 0x01000000,
    VIV_PROT_GROWSUP    = 0x02000000,
};

// Linux `flags` bits (generic musl `include/sys/mman.h`).
enum {
    VIV_MAP_SHARED          = 0x01,
    VIV_MAP_PRIVATE         = 0x02,
    VIV_MAP_FIXED           = 0x10,
    VIV_MAP_ANONYMOUS       = 0x20,
    VIV_MAP_NORESERVE       = 0x4000,
    VIV_MAP_STACK           = 0x20000,
    VIV_MAP_FIXED_NOREPLACE = 0x100000,
};

// The only admitted `flags` word -- an EXACT match, not a mask test.
//
// MAP_STACK and MAP_NORESERVE are absent DELIBERATELY. Both are arguably
// honourable (MAP_STACK is a no-op on Linux; Thylacine's lazy anon already IS
// the no-reserve behaviour), but MEASURED, musl passes neither: both
// pthread_create sites and all four mallocng sites pass exactly
// MAP_PRIVATE|MAP_ANON. Admitting a flag no caller sends would be speculation
// dressed as generosity, and this file's standard is that each admission is a
// claim about behaviour that has to be justified individually.
#define VIV_MMAP_FLAGS_ADMITTED ((u32)(VIV_MAP_PRIVATE | VIV_MAP_ANONYMOUS))

// Decide whether an `mmap` is inside the translatable domain. PURE -- no user
// memory, no Proc, no locks.
//
// `addr` is accepted at ANY value and deliberately ignored: without MAP_FIXED,
// Linux specifies `addr` as a HINT the kernel may disregard, and the caller
// learns the real address from the return value. Ignoring it is conforming, not
// a compromise. MAP_FIXED / MAP_FIXED_NOREPLACE -- where the address is a
// REQUIREMENT -- are outside the admitted flags word and therefore decline.
//
// `len` is NOT judged here. It is a semantic question, not a domain one: Linux
// answers EINVAL for 0 and ENOMEM for too-large, and the shell produces both
// exactly (0 up front; the target's own refusal for the rest). Forwarding on
// length would answer ENOSYS for a call Linux gives a specific errno.
//
// Returns VIV_TRANSLATED (the shell may call SYS_BURROW_ATTACH_LAZY with `len`)
// or VIV_FORWARD. Never ENOSYS: `mmap` exists.
enum viv_verdict vivarium_mmap_decide(u64 addr, u64 prot, u64 flags,
                                      u64 fd, u64 offset);

// -----------------------------------------------------------------------------
// TIER 0/2 — signals (V-6). See VIVARIUM.md §6.22.
//
// Thylacine has no POSIX signals; it has Plan 9 NOTES (I-19). Every row below is
// a decode onto a note that ALREADY EXISTS, which is what makes Tier 0 a
// translation rather than new machinery.
//
// THE PIECE THAT IS NEW: the per-Proc disposition table. Pouch keeps its sigtab
// in USERSPACE (`__pouch_sigtab`, patch 0007) because the pouch libc is ours to
// patch -- it registers ONE bootstrap via SYS_NOTIFY and dispatches per-signal
// itself. A Vivarium guest's libc is not ours, so that architecture is simply
// unavailable and the kernel has to hold the table. It is the reason `rt_sigaction`
// is a shell with state rather than a pure row.
// -----------------------------------------------------------------------------

// Linux aarch64 signal numbers (musl `arch/aarch64/bits/signal.h`, read from the
// tree). Only the ones a row actually names are listed; the rest are handled by
// range checks, so an unlisted number is not a silent gap.
enum {
    VIV_SIGHUP  = 1,  VIV_SIGINT  = 2,  VIV_SIGQUIT = 3,  VIV_SIGILL  = 4,
    VIV_SIGABRT = 6,  VIV_SIGBUS  = 7,  VIV_SIGFPE  = 8,  VIV_SIGKILL = 9,
    VIV_SIGSEGV = 11, VIV_SIGPIPE = 13, VIV_SIGTERM = 15, VIV_SIGCHLD = 17,
    VIV_SIGCONT = 18, VIV_SIGSTOP = 19, VIV_SIGTSTP = 20, VIV_SIGWINCH = 28,
};

// Linux's `_NSIG` is 65 and signals are 1..64 inclusive.
#define VIV_NSIG 64

// `sa_handler` sentinels (generic musl `include/signal.h`).
#define VIV_SIG_DFL 0UL
#define VIV_SIG_IGN 1UL
#define VIV_SIG_ERR ((u64)-1)

// `sa_flags` bits (musl `arch/aarch64/bits/signal.h`).
enum {
    VIV_SA_NOCLDSTOP = 1,
    VIV_SA_NOCLDWAIT = 2,
    VIV_SA_SIGINFO   = 4,
    VIV_SA_ONSTACK   = 0x08000000,
    VIV_SA_RESTART   = 0x10000000,
    VIV_SA_NODEFER   = 0x40000000,
    VIV_SA_RESETHAND = 0x80000000,
    VIV_SA_RESTORER  = 0x04000000,
};

// `rt_sigprocmask` how-values (generic musl `include/signal.h`).
enum { VIV_SIG_BLOCK = 0, VIV_SIG_UNBLOCK = 1, VIV_SIG_SETMASK = 2 };

// The kernel-ABI sigaction struct the GUEST sends -- musl's `struct k_sigaction`
// with SA_RESTORER present, which is the shape aarch64 musl actually emits.
//
// MEASURED, not assumed: musl compiles with `-D_XOPEN_SOURCE=700` (its Makefile
// line 50), which satisfies the guard exposing SA_RESTORER in
// `arch/aarch64/bits/signal.h:114`, so `src/internal/ksigaction.h` includes the
// `restorer` member and `sigaction.c` always fills it with `__restore_rt`
// (`mov x8,#139; svc 0`).
//
// V-6b CORRECTION. V-6a shipped this as a RUNTIME discrimination -- helpers that
// returned 24 or 32 depending on whether the caller set SA_RESTORER -- on the
// theory that a libc omitting the flag sends the shorter shape. Reading musl
// showed the layout is fixed by the ARCH, not chosen per call:
//
//   * `src/internal/ksigaction.h` gates `restorer` on `#ifdef SA_RESTORER`, a
//     COMPILE-time arch property. aarch64 has no override in `arch/aarch64/`,
//     and its `bits/signal.h` defines SA_RESTORER, so the member is always there.
//   * `sigaction.c` sets `ksa.flags |= SA_RESTORER` UNCONDITIONALLY for every
//     install -- `signal(SIGPIPE, SIG_IGN)` included. There is no musl call that
//     arrives without it.
//   * Linux copies `sizeof(struct sigaction)` from `act` regardless of flags. If
//     the kernel wanted 24 on aarch64, musl's 32-byte struct would land `mask`
//     8 bytes past where the kernel reads it and every install would carry the
//     wrong mask -- so musl working at all is the proof.
//
// So the size is a CONSTANT here, matching what Linux itself reads, and a guest
// that sends a short buffer gets the same fault it would get on Linux rather
// than a shape we invented for it.
struct viv_ksigaction {
    u64 handler;
    u64 flags;
    u64 restorer;
    u64 mask;
};
#define VIV_KSIGACTION_SIZE         32u
#define VIV_KSIGACTION_OFF_FLAGS     8u
#define VIV_KSIGACTION_OFF_RESTORER 16u
#define VIV_KSIGACTION_OFF_MASK     24u
_Static_assert(sizeof(struct viv_ksigaction) == VIV_KSIGACTION_SIZE,
               "the kernel-side mirror must match the size the guest sends");
_Static_assert(__builtin_offsetof(struct viv_ksigaction, flags)    == VIV_KSIGACTION_OFF_FLAGS,    "flags @8");
_Static_assert(__builtin_offsetof(struct viv_ksigaction, restorer) == VIV_KSIGACTION_OFF_RESTORER, "restorer @16");
_Static_assert(__builtin_offsetof(struct viv_ksigaction, mask)     == VIV_KSIGACTION_OFF_MASK,     "mask @24");

// Which note carries a given Linux signal. PURE.
//
// VIV_SIGNOTE_NONE means "no note in the tree carries this signal" -- the row
// declines rather than inventing a delivery. That is the honest answer for
// SIGALRM (no timer note), SIGUSR1/2 (no general-purpose note), and the rest of
// the realtime range.
enum viv_signote {
    VIV_SIGNOTE_NONE = 0,
    VIV_SIGNOTE_INTERRUPT,       // SIGINT only -- see viv_signal_note for why
                                 // V-6b evicted SIGTERM from this row
    VIV_SIGNOTE_KILL,            // SIGKILL          (non-catchable both sides)
    VIV_SIGNOTE_PIPE,            // SIGPIPE
    VIV_SIGNOTE_CHILD_EXIT,      // SIGCHLD
    VIV_SIGNOTE_SNARE_SEGV,      // SIGSEGV
    VIV_SIGNOTE_SNARE_BUS,       // SIGBUS
    VIV_SIGNOTE_SNARE_ILL,       // SIGILL
    VIV_SIGNOTE_SNARE_FPE,       // SIGFPE
    VIV_SIGNOTE_TTY_HUP,         // SIGHUP
    VIV_SIGNOTE_TTY_QUIT,        // SIGQUIT
    VIV_SIGNOTE_TTY_WINCH,       // SIGWINCH
    VIV_SIGNOTE_TTY_SUSP,        // SIGTSTP
    VIV_SIGNOTE_TTY_CONT,        // SIGCONT
};

enum viv_signote viv_signal_note(u64 signum);

// One past the last real note kind -- the array bound for a per-note table.
#define VIV_SIGNOTE_COUNT ((u32)VIV_SIGNOTE_TTY_CONT + 1u)

// Does `signum` EXCLUSIVELY own its note -- is it the ONLY Linux signal this
// map routes there? PURE.
//
// V-6b found this the hard way. A DISPOSITION is per-signal, but the note
// substrate carries no signal identity: the poster posts the NAME "interrupt",
// and SIGINT and SIGTERM both land on it. So `sigaction(SIGINT, SIG_IGN)` with
// SIGTERM left at SIG_DFL is NOT REPRESENTABLE -- honouring it silences SIGTERM
// too, and not honouring it kills a Proc that asked to ignore Ctrl-C. Both
// directions are wrong, which means the call is outside the domain, not merely
// approximated. It declines.
//
// Computed by SCANNING the map rather than listing exclusive signals, so it can
// never drift: adding a second signal to any note automatically narrows the
// domain instead of silently making one of the two wrong. The exclusivity rule
// is also what makes the REVERSE direction (note -> signal) well-defined, which
// is what delivery needs to answer "is this note ignored?".
bool viv_signal_owns_note_exclusively(u64 signum);

// Can a note of this kind actually REACH a Proc's queue? PURE.
//
// MEASURED, not assumed: `g_known_notes` (notes.c) holds interrupt / kill /
// pipe / child_exit / tty:{winch,susp,cont,quit,hup} -- and NOT the snare:*
// family. `proc_fault_terminate` calls `exits(name)` DIRECTLY without going
// through `notes_post`, so an EL0 fault terminates its Proc before any queue
// or mask is consulted (notes.h says the same of NOTE_BIT_SNARE: "this bit has
// no consumer today").
//
// The consequence for the phenotype is sharp and permanent-until-v1.x: SIGSEGV,
// SIGBUS, SIGILL and SIGFPE can be given SIG_DFL (terminate -- which is what
// already happens) but can NEVER be caught or ignored. A guest that installs a
// SIGSEGV handler is told so, rather than discovering it at the first fault.
bool viv_signote_is_deliverable(enum viv_signote note);

// Which Linux signal owns this note -- the inverse of `viv_signal_note`. PURE.
//
// Well-defined ONLY because `viv_signal_owns_note_exclusively` gates every
// disposition write, so a note with a live entry has exactly one signal behind
// it. Computed by SCANNING for the same reason the exclusivity check does: a
// second table would be a second thing to keep in step.
//
// Returns 0 for a note no signal maps to. Delivery needs this to fill
// `si_signo` and x0, which is the whole reason the exclusivity rule is load-
// bearing rather than merely tidy.
u64 viv_signote_to_signal(enum viv_signote note);

// Is this note's LINUX default action "ignore"? PURE.
//
// SIGCHLD, SIGWINCH and SIGCONT are the three whose default is to do nothing.
// It matters because Thylacine's queue holds an undelivered note until someone
// reads it, and a Linux guest has no notes fd to read one with (there is no
// translation row for SYS_NOTE_OPEN, and a native number reaches the table
// first) -- so without this the queue would fill with notes nothing will ever
// consume. Dropping them at delivery is what Linux does and what keeps the
// queue bounded.
//
// SIGTSTP is deliberately NOT here: its default is STOP, not ignore, and the
// kernel NDFLT-stop arm is an unbuilt ABI decision (task #15). Claiming
// "ignore" for it would be a stored lie in the other direction.
bool viv_signote_default_is_ignore(enum viv_signote note);

// Per-Proc signal disposition. Lazily allocated on a Proc's first translatable
// `rt_sigaction`; freed at proc_free. NOT inherited across rfork -- the
// `handler_va` precedent (notes.h F13), and the POSIX-exec rule agrees for
// handlers. SIG_IGN's survival across exec is a stated fidelity gap (§9).
//
// Indexed by `enum viv_signote`, NOT by signal number -- legitimate ONLY
// because `viv_signal_owns_note_exclusively` gates every write, so each live
// entry has exactly one signal behind it.
//
// V-6c widened this from `u8 ignored[]` to the whole `k_sigaction`. V-6b stored
// only what it could honour, which was one bit; delivery needs the handler
// address, SA_RESTORER (the guest's return trampoline) and the flags. A zeroed
// table reads as all-SIG_DFL, which is the correct initial state, so the lazy
// allocation needs no separate initialiser.
struct viv_sigtab {
    struct viv_ksigaction act[VIV_SIGNOTE_COUNT];
};

// -----------------------------------------------------------------------------
// SOCKETS (V-5) -- the per-Proc socket table.
//
// Linux fuses into one fd what /net splits across three files, so a translated
// socket carries state no fd and no path can hold. docs/VIVARIUM.md section 5.5
// has the design and the measurements; the short version:
//
//   socket()  open /net/<proto>/clone -> the fid BECOMES ctl; read it for N
//   connect() write the verb to ctl; open .../N/data; SWAP the fd onto data
//   read/write/close  untranslated -- T1 rows on an ordinary Spoor fd
//
// `N` is re-readable from ctl, but only while the fd still IS ctl. `proto` is
// knowable ONLY at socket(), where the guest passes SOCK_STREAM/SOCK_DGRAM and
// never mentions it again. Recovering it later would mean decoding netd's qid
// layout -- REFUSED, because /net is a mount point that need not be netd, and
// decoding a foreign server's qid as netd's is exactly the silent
// mistranslation the argument-domain rule exists to forbid. So proto is
// REMEMBERED. That is the whole reason this table exists.
// -----------------------------------------------------------------------------

// The /net protocol directories. Values are internal (never crossing to EL0),
// so they are ordinals, not an ABI.
enum viv_net_proto {
    VIV_NET_TCP = 0,
    VIV_NET_UDP = 1,
};

// A socket's lifecycle position, which is also WHICH FILE THE FD DENOTES:
//
//   FRESH      -> the fd is the connection's `ctl`  (fresh, or bound, or both)
//   LISTENING  -> the fd is STILL `ctl` (announce is a ctl write, not a swap)
//   CONNECTED  -> the fd has been swapped onto `data`
//
// So the ctl/data split is FRESH|LISTENING vs CONNECTED, not FRESH vs the rest:
// a listening socket keeps its ctl fd forever, because that is the fd `accept`
// re-walks from and the fd whose reference keeps the listener alive. Anything
// that writes a ctl verb (connect, announce) requires a non-CONNECTED fd, and
// anything that moves bytes requires CONNECTED.
enum viv_sock_state {
    VIV_SOCK_FREE      = 0,   // the entry is not in use (zeroed table = all free)
    VIV_SOCK_FRESH     = 1,
    VIV_SOCK_CONNECTED = 2,
    VIV_SOCK_LISTENING = 3,
};

// Bounded: a guest must not be able to grow kernel memory without bound (the
// I-32 posture). 64 is generous against PROC_HANDLE_MAX (256) while keeping the
// table under a page; a guest that exhausts it gets EMFILE, which is a POSIX
// answer rather than an extinction.
#define VIV_SOCK_MAX 64u

struct viv_sock {
    s32 fd;          // the guest's fd; < 0 when the entry is free
    u32 n;           // the /net connection number
    u32 bound_addr;  // bind(): the requested local address, host order (0 = any)
    u16 bound_port;  // bind(): the requested local port,    host order (0 = any)
    u8  proto;       // enum viv_net_proto
    u8  state;       // enum viv_sock_state
};

// THERE IS DELIBERATELY NO `bound` FLAG. An unbound socket and one bound to
// 0.0.0.0:0 are indistinguishable in EVERY path this table feeds:
//   * listen()  needs a concrete non-zero port either way (netd's announce
//               parser rejects port 0), so both are refused identically;
//   * connect() is unconstrained by both, so both proceed identically.
// A flag would therefore be state that no reader could ever branch on. If a
// future row makes the two differ -- getsockname() on a bound-but-idle socket
// is the candidate -- add it THEN, with the reader that needs it.
_Static_assert(sizeof(struct viv_sock) == 16, "viv_sock pinned at 16 bytes");

// The per-Proc table (Proc.socktab). Lazily allocated on the first translated
// socket(), CAS-installed, freed at proc_free, NOT rfork-inherited -- the
// viv_sigtab shape exactly.
//
// LOCK-FREE, AND WHY (the same argument sigtab carries, and the same warning).
// Entries are read and written with no lock. That is sound because a
// PHENO_LINUX Proc CANNOT SPAWN A THREAD -- clone/clone3 are not table rows, so
// they FORWARD to ENOSYS -- and a single-threaded Proc has no peer to race.
// This is a property of the TRANSLATION TABLE, not of the data, and it
// EVAPORATES the moment process creation lands (VIVARIUM.md task #93).
// Re-derive it there; do not assume this comment still holds.
struct viv_socktab {
    struct viv_sock s[VIV_SOCK_MAX];
};

// Find the entry for `fd`, or NULL. PURE + NULL-safe.
struct viv_sock *viv_socktab_find(struct viv_socktab *tab, s32 fd);

// Claim a free entry for `fd` in state FRESH. Returns the entry, or NULL if the
// table is full (-> EMFILE) or `tab` is NULL. Does NOT check for a duplicate
// fd: the caller has just been handed that fd by handle_alloc, so it cannot
// already be in the table -- an entry left behind by a closed fd would be the
// close-hook bug this table's drop path exists to prevent, and a duplicate here
// would be its symptom rather than its cause.
struct viv_sock *viv_socktab_claim(struct viv_socktab *tab, s32 fd,
                                   enum viv_net_proto proto, u32 n);

// Release the entry for `fd`, if any. Idempotent -- an fd with no entry (a
// plain file, or a socket already dropped) is a no-op, which is what lets the
// close hook run unconditionally for a phenotyped Proc.
void viv_socktab_drop(struct viv_socktab *tab, s32 fd);

// True when a claim would succeed. PURE. accept() asks BEFORE it blocks: it is
// about to make a real inbound connection exist, and discovering afterwards
// that the table is full means accepting a peer only to hang up on it. (claim()
// still returns NULL on a full table -- this is the courtesy check, not the
// safety one.)
bool viv_socktab_has_room(const struct viv_socktab *tab);

// Decide whether a `socket(domain, type, protocol)` is inside the translatable
// domain, and if so which /net protocol directory it names. PURE.
//
// THE ARGUMENT DOMAIN. AF_INET only: AF_INET6 has no /net representation at
// v1.0 and is refused honestly (EAFNOSUPPORT) rather than silently served as
// v4. SOCK_STREAM -> tcp, SOCK_DGRAM -> udp; SOCK_SEQPACKET/SOCK_RAW have no
// /net analogue. `protocol` must be 0 or the family default (IPPROTO_TCP/UDP);
// anything else names a protocol netd does not speak.
//
// SOCK_NONBLOCK/SOCK_CLOEXEC in the type word are REFUSED rather than ignored:
// a guest that asks for a non-blocking socket and silently gets a blocking one
// hangs where it expected EAGAIN, which is the mistranslation this tier exists
// to prevent. (Both are v1.x rows -- NONBLOCK needs a /net readiness story,
// CLOEXEC needs an exec that preserves fds.)
//
// Returns true + writes *out_proto when translatable; false leaves *out_proto
// untouched and the caller answers the errno in *out_err.
bool vivarium_socket_decide(u64 domain, u64 type, u64 protocol,
                            enum viv_net_proto *out_proto, s32 *out_err);

// The /net protocol directory name for a proto ("tcp" / "udp"). PURE; never
// NULL for a value the decide function produced.
const char *vivarium_net_proto_dir(enum viv_net_proto proto);

// Parse a Linux `struct sockaddr_in` (already copied into kernel memory) into
// its four address octets + host-order port. PURE.
//
// Returns false when the family is not AF_INET, the length is short, or the
// port is 0 (netd's dial parser rejects it, and a connect to port 0 is
// meaningless) -- so a malformed address can never reach the ctl writer.
bool vivarium_sockaddr_in_parse(const u8 *sa, u64 salen,
                                u8 out_ip4[4], u16 *out_port);

// The same parse WITHOUT the port-0 rejection, for bind(), where 0.0.0.0:0 is
// the ordinary "any address, any port" request rather than a malformed one.
// PURE. vivarium_sockaddr_in_parse is this plus `port != 0`.
bool vivarium_sockaddr_in_parse_any(const u8 *sa, u64 salen,
                                    u8 out_ip4[4], u16 *out_port);

// Build a Linux `struct sockaddr_in` (the accept() peer-address out-parameter)
// into `buf`. PURE. Returns 16 -- the fixed sockaddr_in size -- or 0 if `buflen`
// is short. The bytes are laid out exactly as Linux expects: family LE, port and
// address BOTH network order.
u32 vivarium_sockaddr_in_build(u8 *buf, u32 buflen, const u8 ip4[4], u16 port);

// Build a netd ctl command line: "<verb> a.b.c.d!port". PURE. Returns the
// length written, or 0 if it would not fit (which the caller must treat as a
// refusal, never as a zero-length write).
u32 vivarium_net_cmd_ipport(char *buf, u32 buflen, const char *verb,
                            const u8 ip4[4], u16 port);

// Build the `announce` ctl verb. PURE. An address of 0.0.0.0 becomes Plan 9's
// wildcard `announce *!port`; a concrete address becomes `announce a.b.c.d!port`.
//
// The distinction is not cosmetic. netd migrates a listener announced on an
// EXPLICIT 127.x address onto its loopback stack, while a `*` listener stays on
// the NIC and does NOT span loopback -- so a guest binding 127.0.0.1 and a guest
// binding INADDR_ANY reach genuinely different listeners, and rendering one as
// the other would silently move the server.
u32 vivarium_net_cmd_announce(char *buf, u32 buflen, const u8 ip4[4], u16 port);

// Parse netd's `a.b.c.d!port` endpoint rendering (the `remote` / `local` files)
// back into octets + host-order port. PURE -- the inverse of the ipport builder.
// Returns false on any malformation, on a >255 octet, or on a >65535 port, so a
// garbled endpoint file can never become a plausible-looking peer address.
bool vivarium_parse_ipport(const char *buf, u32 len, u8 out_ip4[4], u16 *out_port);

// Decide whether listen() may proceed on a socket in this state. PURE.
//
// Writes the POSIX errno to *out_err and returns false when it may not:
//   * a UDP socket cannot listen at all         -> EOPNOTSUPP
//   * a CONNECTED socket cannot listen          -> EINVAL
//   * an unbound (or port-0) socket cannot      -> EOPNOTSUPP
// The last is a DEGRADED answer, not an equivalent one: Linux would auto-bind an
// ephemeral port, and netd's announce parser rejects port 0 outright. Declining
// is the honest half of the argument domain -- and harmless in practice, because
// discovering an auto-bound port needs getsockname(), which is not a row yet.
bool vivarium_listen_decide(enum viv_net_proto proto, enum viv_sock_state state,
                            u16 bound_port, s32 *out_err);

// Parse the decimal connection number netd returns when a `clone`/`ctl` fid is
// read. PURE. Returns false on empty, non-decimal, or out-of-range input --
// which the caller must treat as a protocol failure rather than connection 0.
bool vivarium_parse_conn_n(const char *buf, u32 len, u32 *out_n);

// -----------------------------------------------------------------------------
// Readiness (V-5c, section 5.5.4).
// -----------------------------------------------------------------------------

// Convert a Linux `struct timespec` to the native SYS_POLL millisecond timeout.
// PURE.
//
// ROUNDS UP. A sub-millisecond timeout must not become 0: 0 means "return
// immediately" to SYS_POLL, so truncating would turn a caller's 100us wait into
// a busy loop -- the one conversion error a poll loop would never notice and
// never stop paying for. Linux rounds a ppoll timeout up to the next tick for
// the same reason.
//
// SATURATES at INT32_MAX ms (~24.8 days) rather than overflowing. A caller
// asking for longer gets a shorter wait than it asked for and then loops --
// which is what a poll caller does anyway.
//
// Returns false with *out_err = EINVAL for a malformed timespec (a negative
// field, or tv_nsec >= 1e9), matching Linux's own validation.
bool vivarium_timespec_to_ms(s64 sec, s64 nsec, s32 *out_ms, s32 *out_err);

// Decide whether a ppoll() is inside the translatable domain. PURE.
//
// THE ARGUMENT DOMAIN, and note that both declines below are shapes rather than
// bad values -- ENOSYS, not EINVAL, because the arguments are perfectly valid
// Linux and it is this kernel that cannot serve them:
//
//   * sigmask != NULL -- ppoll's WHOLE REASON TO EXIST over poll() is that it
//     swaps the signal mask ATOMICALLY with the wait. Thylacine has no way to
//     do that, and doing it non-atomically re-opens exactly the race the caller
//     chose ppoll to close. Honouring it approximately would be the silent
//     mistranslation this tier exists to prevent. (musl's poll() passes NULL,
//     so the common path is unaffected.)
// nfds > POLL_MAX_NFDS is EINVAL rather than ENOSYS: that IS a bad value, and
// the native cap is the real bound a caller must respect.
//
// nfds == 0 IS SERVED, and it did not used to be. Linux reads it as "sleep for
// the timeout", and V-5c-1 declined it because native SYS_POLL rejects nfds == 0
// and there is no native sleep SYSCALL to route it to. That reasoning looked one
// layer too high: there is no sleep syscall, but there has always been a sleep
// PRIMITIVE -- `tsleep` with a deadline and a cond that is never true, which is
// what poll's own slow path parks on. V-5c-2 needed it anyway for
// `select(0, NULL, NULL, NULL, &tv)`, the classic portable sleep, so both
// zero-fd forms now route to `viv_timed_sleep` and neither declines.
bool vivarium_ppoll_decide(u64 nfds, u64 sigmask_va, s32 *out_err);

// -----------------------------------------------------------------------------
// pselect6 (V-5c-2) -- the fd_set reshape.
//
// The one T2 row whose translation is a genuine CHANGE OF REPRESENTATION rather
// than a renumber or a field copy: three 1024-bit bitmaps in, one pollfd array
// out, and three bitmaps back. It is also the row with the most ways to be
// subtly wrong, so the whole conversion is PURE and unit-driven, and the shell
// in syscall.c does nothing but uaccess and the poll call.
//
// THE PRIOR ART IS IN THIS TREE, AND IT IS WRONG IN FOUR WAYS. pouch's
// userspace select() (`usr/lib/pouch/patches/0005-pouch-poll.patch`) performs
// this same translation over native SYS_POLL. Reading it before writing this
// was worth more than the writing: every one of its four defects is a decision
// point here (task #99).
//
//   * IT CAPS THE WRONG AXIS. It rejects any fd >= 64 with EBADF, on the stated
//     grounds that "the handle would be unreachable through any Thylacine
//     syscall". That was true when PROC_HANDLE_MAX was 64; commit ffcc64b7 split
//     PROC_HANDLE_MAX (256, the fd-VALUE ceiling) from POLL_MAX_NFDS (64, the
//     pollfd-ARRAY ceiling) and made it false. The cap belongs on the COUNT of
//     contributing fds -- a select over fds 200 and 201 is two pollfds and is
//     fine; a select over 65 low fds is not.
//   * IT TRANSLATES exceptfds TO POLLPRI, which native poll has no bit for, so
//     the request is silently dropped and the exceptfds bit can never be set. A
//     select waiting ONLY on exceptfds therefore blocks forever instead of
//     failing. Silently-never-true is the mistranslation this tier exists to
//     prevent; see the exceptfds note on the scan function below.
//   * IT FORWARDS POLLHUP INTO THE WRITE SET, commented "(Linux semantics)".
//     Linux's POLLOUT_SET is POLLOUT|POLLERR; POLLHUP is in POLLIN_SET only.
//   * IT COUNTS FDS, NOT BITS. See the return-value note below.
//
// None of this makes pouch's select unusable -- it makes it a boundary-line that
// drifted from the kernel underneath it. This one is written against the kernel
// as it is today, with the constants named rather than copied.
// -----------------------------------------------------------------------------

// An fd_set is a fixed 1024-bit bitmap (musl `include/sys/select.h`:
// FD_SETSIZE 1024). The ABI is the caller's `fd_set *`, so this is the size of
// the object, not a tunable.
#define VIV_FD_SETSIZE   1024
#define VIV_FD_SET_BYTES (VIV_FD_SETSIZE / 8)

// Decide whether a pselect6() is inside the translatable domain, and clamp nfds.
// PURE.
//
// `nfds` arrives as a signed Linux `int` widened to 64 bits, so the negative
// case is real and must be checked as signed.
//
//   * sigmask != NULL -- declines for EXACTLY ppoll's reason (the atomic mask
//     swap has no counterpart), and note that pselect6 packs it indirectly: the
//     6th argument is a POINTER to {const sigset_t *ss; size_t ss_len;} because
//     aarch64 caps a syscall at 6 registers. So a NULL 6th argument and a
//     non-NULL pointer to a NULL ss are different things; only the former is
//     unambiguously "no mask", and the latter is declined rather than peeked at.
//   * nfds < 0 -- EINVAL, Linux's own check.
//
// CLAMPING IS LINUX'S BEHAVIOUR, NOT A SHORTCUT. `core_sys_select` does
// `if (n > max_fds) n = max_fds;` -- bits above the fd table are simply not
// scanned. Our handle table is a FIXED PROC_HANDLE_MAX entries (never grown), so
// "max_fds" here is a constant, and clamping to it is exactly what Linux does on
// a process whose table happens to be that size. A bit set above the clamp names
// an fd that cannot exist, so ignoring it loses nothing; a bit set BELOW it that
// names no open handle still becomes POLLNVAL -> EBADF, which is also Linux.
bool vivarium_pselect6_decide(u64 nfds_raw, u64 sigmask_va, u32 *out_nfds,
                              s32 *out_err);

// How many bytes of an fd_set cover the low `nfds` bits, rounded up to the
// 8-byte granule Linux uses (FDS_BYTES: FDS_LONGS(nr) * sizeof(long)). PURE.
//
// Copying only these bytes rather than the full 128 is fidelity, not thrift:
// Linux's get_fd_set/set_fd_set touch exactly this much, so a caller that sized
// its allocation to its nfds -- legal, and done by code that keeps fd_sets in
// packed structs -- must not take a fault here that it would not take there.
u32 vivarium_fdset_bytes(u32 nfds);

// Scan three fd_sets into a pollfd array. PURE.
//
// A NULL set pointer means "all zero" (Linux passes NULL for an unwanted set).
// `out_cap` is the caller's array length; the count of CONTRIBUTING fds is what
// the native ceiling applies to.
//
// EXCEPTFDS DECLINES RATHER THAN NEVER-FIRES. Native poll has no POLLPRI: the
// requestable set is (POLLIN|POLLOUT), full stop. So there is no honest way to
// represent "wake me on out-of-band data", and the two dishonest ways are both
// worse than declining -- dropping the bit silently (pouch) turns a pure
// exceptfds wait into an infinite block, and treating it as POLLIN would report
// ordinary data as an exception. A NULL or all-zero exceptfds is not a request
// and passes through untouched; only a SET bit inside [0, nfds) declines.
//
// Returns false with ENOSYS for a set exceptfds bit, EINVAL if more than
// `out_cap` fds contribute.
bool vivarium_fdset_to_pollfds(const u8 *rd, const u8 *wr, const u8 *ex,
                               u32 nfds, struct pollfd *out, u32 out_cap,
                               u32 *out_count, s32 *out_err);

// Map poll results back into three fd_sets and count the ready BITS. PURE.
//
// The three output buffers are ZEROED FIRST and then have only ready bits set --
// select reports its result by overwriting the caller's sets, so a bit the
// caller asked about and did not get back must come home clear.
//
// THE REVERSE MAPPING IS DELIBERATELY ASYMMETRIC. Linux's do_select tests
//     POLLIN_SET  = POLLIN  | POLLHUP | POLLERR
//     POLLOUT_SET = POLLOUT | POLLERR
// POLLHUP is in the read set and NOT the write set, which is not an oversight:
// a peer that hung up leaves data readable (then EOF, which read() must be
// allowed to observe), while writing to it is an error rather than a completion.
// Each test is additionally gated on the fd having been REQUESTED in that
// direction, so an errored fd the caller only listed in readfds comes back in
// readfds only.
//
// THE RETURN IS A COUNT OF BITS, NOT OF FDS. Linux increments retval once per
// bit set, so an fd ready for both reading and writing counts TWICE and the
// return can exceed the number of fds passed in. Callers written against that
// contract loop "while (n-- > 0) find the next set bit" and stop one short if
// the count is per-fd -- which is pouch's fourth defect.
//
// POLLNVAL FAILS THE WHOLE CALL. poll reports a bad fd per-entry; select has no
// per-fd error channel, so POSIX makes an invalid fd anywhere EBADF for the
// entire call. Returns false with EBADF, leaving the output buffers untouched --
// on an error Linux does not write the sets back at all.
bool vivarium_pollfds_to_fdset(const struct pollfd *pfds, u32 count,
                               u8 *rd, u8 *wr, u8 *ex, u32 *out_bits,
                               s32 *out_err);

// The budget a caller-requested timeout of 0 gets when the array holds a /net
// socket, so netd's async readiness probe has a chance to land (task #98; the
// reasoning is at the use site in viv_ppoll). Small enough that a zero-timeout
// poll is still "immediate", generous enough for a loopback round trip.
#define VIV_PPOLL_PROBE_MS 10

// The reverse step: which note kind is this note NAME? PURE.
//
// Takes the name rather than a bit because NOTE_BIT_TTY is one bit for five
// distinct names, and a disposition has to tell tty:winch from tty:hup. Returns
// VIV_SIGNOTE_NONE for anything unrecognised, so an unknown name is never
// treated as a signal.
enum viv_signote viv_signote_from_note_name(const char *name);

// Is the note carrying `note` currently ignored by this Proc? PURE + NULL-safe
// (a Proc that never called rt_sigaction has no table and ignores nothing).
bool viv_sigtab_note_ignored(const struct viv_sigtab *tab, enum viv_signote note);

// Does this Proc have a REAL handler for `note` -- neither SIG_DFL nor SIG_IGN?
// PURE + NULL-safe. On true, `*out` receives the whole recorded k_sigaction
// (handler + flags + restorer + mask); on false `*out` is untouched, so a
// caller that ignores the return cannot deliver to a stale address.
bool viv_sigtab_note_handler(const struct viv_sigtab *tab, enum viv_signote note,
                             struct viv_ksigaction *out);

// Record a disposition. Returns false (and changes nothing) if the note is out
// of range or `act` is NULL -- the caller has already run the domain check, so
// this is defence-in-depth on the array index.
bool viv_sigtab_set(struct viv_sigtab *tab, enum viv_signote note,
                    const struct viv_ksigaction *act);

// Decide whether an `rt_sigaction` is inside the translatable domain. PURE.
//
// `signum` must be 1..64, must not be SIGKILL/SIGSTOP (POSIX: uncatchable, and
// Linux itself answers EINVAL), and must map to a note -- a disposition we can
// record but could never act on would be a stored lie.
//
// `handler` must not be SIG_ERR (POSIX-invalid; the pouch layer's F11 audit
// close found the same thing -- without the check a bootstrap calls h(sig) at
// address -1).
//
// THE ARGUMENT DOMAIN: installing a REAL handler requires SA_RESTORER, because
// the guest's own trampoline is how the handler returns. Thylacine will not
// synthesise one: the alternative is a vDSO sigreturn page, and Thylacine's vDSO
// is deliberately RO+XN (I-12/I-13) -- making it executable to serve a
// compatibility row would be a real weakening of an audited surface. SIG_DFL and
// SIG_IGN need no trampoline and are admitted without it.
//
// AT V-6b THE DOMAIN IS NARROWER STILL, and deliberately so:
//
//   * The signal must EXCLUSIVELY own its note (above) -- a shared note cannot
//     carry two independent dispositions.
//   * SIG_IGN on SIGCHLD declines. On Linux that is not "ignore", it is
//     AUTO-REAP -- the child never becomes a zombie. Thylacine has no such
//     mode, so honouring the surface meaning while dropping the real one would
//     leave a guest leaking zombies it believes were reaped.
//   * The note must be DELIVERABLE (above), so a SIGSEGV handler or an ignored
//     SIGBUS is refused rather than stored where nothing will ever read it.
//     SIG_DFL is still admitted for those -- terminate is what already happens.
//
// `sigsetsize` must be 8 (Linux checks `sizeof(sigset_t)` and musl passes
// `_NSIG/8` == 8).
enum viv_verdict vivarium_sigaction_decide(u64 signum, u64 handler, u64 flags,
                                           u64 sigsetsize);

// Decide whether an `rt_sigprocmask` is inside the translatable domain. PURE.
//
// The target is the per-Thread `note_mask`, which exists for exactly this reason
// ("so multi-thread Procs can have different threads accept different signals --
// POSIX pthread_sigmask semantics", notes.h:107).
//
// `how` must be one of BLOCK/UNBLOCK/SETMASK; `sigsetsize` must be 8. The MASK
// VALUE is not judged here -- bits naming signals with no note are dropped by
// `viv_sigset_to_notemask` rather than declining the whole call, because musl
// blocks wide masks (`__block_all_sigs` sets every bit) on paths where declining
// would break an otherwise-translatable program.
enum viv_verdict vivarium_sigprocmask_decide(u64 how, u64 sigsetsize);

// Convert a Linux `sigset_t` word into the kernel's NOTE_BIT_* mask. PURE.
//
// Bits naming signals with no note are DROPPED, deliberately: the alternative is
// to refuse a mask musl routinely sends. The consequence -- blocking SIGALRM has
// no effect because nothing can deliver SIGALRM either -- is consistent rather
// than lossy.
//
// `out_bits` receives a mask of (1u << NOTE_BIT_*) values; the NOTE_BIT_*
// numbering is notes.h's and is passed in by the caller so this file stays free
// of kernel headers.
struct viv_notebit_map {
    u8 interrupt, kill, pipe, child_exit, snare, tty;
};
u64 viv_sigset_to_notemask(u64 sigset, const struct viv_notebit_map *m);

// THE canonical instance, filled from notes.h's NOTE_BIT_* in vivarium.c.
//
// The map is still a PARAMETER rather than a global read inside the
// translators, because that is what keeps them unit-testable against a
// synthetic numbering. But there is exactly ONE kernel instance, because two
// consumers (the syscall shells and the delivery path) each hand-rolling a
// copy is precisely the mirror-drift trap: a per-file `static` verifies only
// that FILE's numbering, never that it agrees with the other's.
extern const struct viv_notebit_map g_viv_notebits;

// The inverse: report a NOTE_BIT_* mask back as a Linux `sigset_t` word. PURE.
//
// Deliberately reports EVERY signal a set bit actually blocks, which makes the
// coarseness VISIBLE instead of hidden. NOTE_BIT_TTY is one bit for five signals
// (notes.h: "per-kind masking is a v1.x extension"), so blocking SIGWINCH really
// does block SIGHUP as well -- and a guest that reads its mask back is told so,
// rather than being shown the tidy answer it asked for while the system does
// something wider. Over-blocking DEFERS a signal, it does not lose one; the
// honest report is what keeps that a stated cost rather than a surprise.
//
// Signals with no note are never reported blocked: nothing can deliver them, so
// "blocked" would describe a state that does not exist.
u64 viv_notemask_to_sigset(u64 notemask, const struct viv_notebit_map *m);

// -----------------------------------------------------------------------------
// TIER 1 -- the signal frame (V-6c). See VIVARIUM.md §6.22.
//
// THE SHAPE, and why it deletes a hazard rather than guarding one. Delivery
// writes a real `siginfo_t` + `ucontext_t` to the guest's stack so a handler
// that READS them works, but `rt_sigreturn` restores from the kernel-side
// `Thread` snapshot and ignores the frame entirely. §8's audit-trigger row
// originally warned that Tier 1 "restores pstate/pc from user memory -- a
// classic privilege-escalation shape; must reject any frame that would
// elevate". Under this design NO field of the user frame ever reaches pstate,
// pc or sp, so there is no frame to reject and no validator to get wrong.
//
// The stated cost, in §9's DEGRADED tier: writing to `uc_mcontext` does not
// change where execution resumes. That breaks signal-driven control transfer
// (Go's sigpanic, JIT deoptimisation); neither reaches this path at v1.0.
//
// THE LAYOUT IS THE TARGET'S, NOT THE HOST'S -- and that distinction cost a
// measurement. `struct sigcontext` ends in musl's `long double __reserved[256]`,
// which is 16 bytes per element on aarch64 (4096 bytes, 16-ALIGNED, so it lands
// at 288 rather than 280) but only 8 on an arm64 Mac. Compiling the layout probe
// with the host cc gave sizeof == 2328; the same probe under
// `--target=aarch64-linux-gnu` gives 4384, and that is the number the guest
// uses. The offsets below were confirmed by _Static_assert against the real
// target compiler before being written down.
//
// WHAT IS WRITTEN vs WHAT IS RESERVED. `sigcontext.__reserved` is a 4096-byte
// area holding a chain of `struct _aarch64_ctx { u32 magic; u32 size; }` records
// (FPSIMD, ESR, SVE...). We write only the 8-byte TERMINATOR -- magic 0, size 0
// -- so a guest that walks the chain stops immediately and correctly concludes
// "no extension records". The remaining 4088 bytes are left as they were, which
// is the guest's OWN stack memory below its own sp: it could read those bytes
// before the signal and can read them after, so nothing crosses a boundary and
// I-13 is untouched. Zeroing them would cost 4 KiB of copy per delivery to hide
// data from the process that owns it.
//
// The absent FPSIMD record is HONEST rather than lazy: note delivery does not
// save or restore Q0-Q31 at all (task #96 -- a pre-existing property of the
// native note path, which V-6c makes more reachable), so a record claiming to
// hold them would be a lie. An empty chain says exactly what is true.
// -----------------------------------------------------------------------------

// Sizes of the FULL Linux structures -- what the stack pointer must skip, as
// distinct from the prefix this file actually writes.
#define VIV_SIGINFO_SIZE      128u    // siginfo_t
#define VIV_UCONTEXT_SIZE    4560u    // ucontext_t (176 header + 4384 mcontext)
#define VIV_SIGFRAME_SIZE    (VIV_SIGINFO_SIZE + VIV_UCONTEXT_SIZE)   // 4688
#define VIV_FRAME_RECORD_SIZE 16u     // { fp, lr } -- the walkable frame chain
#define VIV_SIGFRAME_TOTAL   (VIV_SIGFRAME_SIZE + VIV_FRAME_RECORD_SIZE)

// `si_code` for a kernel-generated signal (Linux `SI_KERNEL`). Chosen over
// SI_USER because it is the one value that claims NOTHING about the union: a
// note carries a 16-byte name and one u32 arg, so si_pid / si_uid / si_status /
// si_addr have no source. SI_USER would invite a guest to read si_pid and get a
// confident zero.
#define VIV_SI_KERNEL 0x80

// The Linux aarch64 `siginfo_t` (128 bytes). Only the three leading ints have a
// source here; the union is zeroed rather than guessed at.
struct viv_linux_siginfo {
    s32 si_signo;       //   0
    s32 si_errno;       //   4
    s32 si_code;        //   8
    s32 __pad0;         //  12
    u8  __pad1[112];    //  16 -- the _sifields union, deliberately all zero
};
_Static_assert(sizeof(struct viv_linux_siginfo) == VIV_SIGINFO_SIZE,
               "siginfo_t is 128 bytes on Linux aarch64");
_Static_assert(__builtin_offsetof(struct viv_linux_siginfo, si_signo) == 0, "si_signo @0");
_Static_assert(__builtin_offsetof(struct viv_linux_siginfo, si_errno) == 4, "si_errno @4");
_Static_assert(__builtin_offsetof(struct viv_linux_siginfo, si_code)  == 8, "si_code @8");

// The WRITTEN prefix of `struct sigcontext` -- through the first 8 bytes of
// `__reserved`, which carry the record-chain terminator. The full struct is 4384
// bytes; the 4088 past this are the guest's own untouched stack (see above).
struct viv_linux_mcontext_head {
    u64 fault_address;  //   0 -- 0: no deliverable note is fault-generated
    u64 regs[31];       //   8 -- x0..x30
    u64 sp;             // 256
    u64 pc;             // 264
    u64 pstate;         // 272
    u8  __pad_res[8];   // 280 -- __reserved is 16-aligned, so it starts at 288
    u32 end_magic;      // 288 -- _aarch64_ctx.magic == 0 (end of chain)
    u32 end_size;       // 292 -- _aarch64_ctx.size  == 0
};
_Static_assert(sizeof(struct viv_linux_mcontext_head) == 296, "mcontext head 296");
_Static_assert(__builtin_offsetof(struct viv_linux_mcontext_head, regs)      ==   8, "regs @8");
_Static_assert(__builtin_offsetof(struct viv_linux_mcontext_head, sp)        == 256, "sp @256");
_Static_assert(__builtin_offsetof(struct viv_linux_mcontext_head, pc)        == 264, "pc @264");
_Static_assert(__builtin_offsetof(struct viv_linux_mcontext_head, pstate)    == 272, "pstate @272");
_Static_assert(__builtin_offsetof(struct viv_linux_mcontext_head, end_magic) == 288,
               "sigcontext.__reserved is __aligned__(16), so it begins at 288 -- "
               "NOT 280. Getting this wrong puts every mcontext field the guest "
               "reads 8 bytes out of place.");

// The WRITTEN prefix of `ucontext_t`, which is everything up to and including
// the mcontext head.
struct viv_linux_ucontext_head {
    u64 uc_flags;       //   0
    u64 uc_link;        //   8 -- 0: no linked context
    u64 ss_sp;          //  16 -- stack_t uc_stack; SS_DISABLE, we have no altstack
    s32 ss_flags;       //  24
    u32 __pad_ss;       //  28
    u64 ss_size;        //  32
    u64 uc_sigmask[16]; //  40 -- sigset_t is 128 bytes in musl userspace
    u8  __pad_mctx[8];  // 168 -- uc_mcontext is 16-aligned, so it starts at 176
    struct viv_linux_mcontext_head uc_mcontext;   // 176
};
_Static_assert(sizeof(struct viv_linux_ucontext_head) == 472, "ucontext head 472");
_Static_assert(__builtin_offsetof(struct viv_linux_ucontext_head, ss_sp)       ==  16, "uc_stack @16");
_Static_assert(__builtin_offsetof(struct viv_linux_ucontext_head, uc_sigmask)  ==  40, "uc_sigmask @40");
_Static_assert(__builtin_offsetof(struct viv_linux_ucontext_head, uc_mcontext) == 176, "uc_mcontext @176");

// The contiguous head of Linux's `struct rt_sigframe { siginfo info; ucontext uc; }`.
// One kernel-side buffer, one copy-out.
struct viv_sigframe_head {
    struct viv_linux_siginfo       info;   //   0
    struct viv_linux_ucontext_head uc;     // 128
};
_Static_assert(sizeof(struct viv_sigframe_head) == 600, "written frame head 600");
_Static_assert(sizeof(struct viv_sigframe_head) <= VIV_SIGFRAME_SIZE,
               "the written prefix must fit inside the frame the sp reserves");

// Build the frame head. PURE -- plain data in, plain data out; the caller owns
// the buffer and performs the copy-out, exactly the vivarium_stat_to_linux
// split.
//
// `out` is fully written including every pad, so a stale kernel stack frame
// cannot reach a guest through the gaps (I-13).
//
// `regs31` must point to 31 words (x0..x30) -- the INTERRUPTED context, which
// the caller has already snapshotted. `sigmask` is the Linux sigset word the
// guest should see as blocked at delivery.
void vivarium_build_sigframe(struct viv_sigframe_head *out, u64 signum,
                             u64 sigmask, const u64 *regs31,
                             u64 sp, u64 pc, u64 pstate);

// -----------------------------------------------------------------------------
// TIER 2 — `clone` (LINEAGE L-3d). See docs/LINEAGE.md §5.3 and §8.
//
// The row that gives a Linux guest a second process. Its target is SYS_RFORK
// (LINEAGE L-3b), and the mapping is a CONSTANT rather than a computation:
//
//     clone(CLONE_VM|CLONE_VFORK|SIGCHLD, stack, ptid, tls, ctid)
//         ->  SYS_RFORK(RFPROC|RFMEM, stack, 0)
//
// so what this decide function actually decides is the DOMAIN, which is where
// every interesting question lives.
//
// THE ARGUMENT ORDER, and why it is not the one most people remember. arm64
// selects CONFIG_CLONE_BACKWARDS, so `tls` comes BEFORE `child_tid`:
//
//     x0 flags   x1 stack   x2 parent_tid   x3 tls   x4 child_tid
//
// musl's own aarch64 clone.s states it in a comment at the top of the file
// ("syscall(SYS_clone, flags, stack, ptid, tls, ctid)"), which is where this was
// read from. The x86-64 order (ptid, ctid, tls) would silently swap two words.
//
// THE HAZARD THAT MAKES THIS ROW DIFFERENT FROM EVERY OTHER ONE: x2, x3 and x4
// ARE GARBAGE ON THE ONLY CALL THAT MATTERS. `posix_spawn` invokes
// `__clone(child, stack, flags, arg)` with FOUR arguments (posix_spawn.c:198),
// and clone.s then does `mov x2,x4 / mov x3,x5 / mov x4,x6` -- moving three
// registers the caller never set. On Linux that is harmless, because
// CLONE_PARENT_SETTID / CLONE_SETTLS / CLONE_CHILD_SETTID are all clear and the
// kernel therefore never reads them.
//
// A translator that reached for `args[3]` as the child's TLS would be reading
// one of those uninitialised registers and handing it to the child as
// TPIDR_EL0 -- and the child would then fault or corrupt on its first
// thread-local access, at a site with no visible connection to the clone. So
// this decide takes flags and stack ONLY, the shell passes a LITERAL 0 for
// child_tls (SYS_RFORK's "inherit the caller's" sentinel, which is what a vfork
// child needs), and the three garbage words are never named at all. The
// admitted domain's exclusion of CLONE_SETTLS is what makes that correct rather
// than merely safe.
//
// (This is the inverse of the arity property the ARCH §25.4 row states for T1
// rows. There the risk is a native target reading MORE argument words than the
// Linux call supplies; here the words are supplied and MEANINGLESS. Both come
// down to the same rule: read a register only when the call's own contract says
// it holds something.)
// -----------------------------------------------------------------------------

// Linux `clone` flag bits (musl `include/sched.h`, read from the tree). Only the
// ones the domain reasoning below names are listed.
enum {
    VIV_CSIGNAL              = 0x000000ff,   // the low byte IS the exit signal
    VIV_CLONE_VM             = 0x00000100,
    VIV_CLONE_FILES          = 0x00000400,
    VIV_CLONE_VFORK          = 0x00004000,
    VIV_CLONE_THREAD         = 0x00010000,
    VIV_CLONE_SETTLS         = 0x00080000,
    VIV_CLONE_PARENT_SETTID  = 0x00100000,
    VIV_CLONE_CHILD_CLEARTID = 0x00200000,
    VIV_CLONE_CHILD_SETTID   = 0x01000000,
};

// SIGCHLD, from `arch/aarch64/bits/signal.h` -- the exit signal posix_spawn asks
// for, and the only one Thylacine can deliver: `exits()` posts the `child_exit`
// note unconditionally (I-19), so SIGCHLD is what the target already does.
#define VIV_CLONE_SIGCHLD 17u

// The only admitted `flags` word -- an EXACT match, exactly as
// VIV_MMAP_FLAGS_ADMITTED is, and for the same reason: a bit we have not
// reasoned about must decline rather than ride along.
//
// Compared at FULL 64-BIT WIDTH, unlike every other decide in this file. Those
// narrow to 32 bits because their Linux parameters ARE `int`; clone's `flags`
// is an `unsigned long`, so narrowing here would be an assumption about Linux's
// own source rather than about its ABI -- and this tree cannot check that. The
// stricter reading is therefore the right one under uncertainty, and it costs
// nothing: musl's clone.s zero-extends (`uxtw x0,w2`), so the high half is
// always 0 from the real consumer.
#define VIV_CLONE_FLAGS_ADMITTED \
    ((u32)(VIV_CLONE_VM | VIV_CLONE_VFORK | VIV_CLONE_SIGCHLD))

// The SECOND admitted word (LINEAGE L-6a): a plain `fork()`. musl's fork() ->
// _Fork() emits exactly `clone(SIGCHLD, 0)` -- no CLONE_VM, so the child gets a
// PRIVATE copy-on-write address space, which is what L-4/L-5 built.
//
// It is a separate exact word rather than a mask relaxation for the reason the
// vfork word is exact: every bit outside these two is one nobody has reasoned
// about. And the two shapes DIFFER in more than a bit -- they take opposite
// `stack` rules (below) and map onto different rfork flag words -- so a single
// mask could not express either correctly.
#define VIV_CLONE_FLAGS_FORK ((u32)VIV_CLONE_SIGCHLD)

// Decide whether a `clone` is inside the translatable domain. PURE -- no user
// memory, no Proc, no locks.
//
// WHY `CLONE_VM` WITHOUT `CLONE_VFORK` IS REFUSED RATHER THAN SERVED. This is
// the one place where L-3c-2's "the fail-safe direction is one-sided" argument
// does NOT carry over, and the difference is worth stating because the two
// chunks reach opposite conclusions from the same shape.
//
// At L-3c-2 the suspend was keyed on RFMEM rather than on a flag of its own,
// and the justification was that an unwanted suspend blocks visibly while an
// unwanted concurrency corrupts silently. That reasoning holds for a NATIVE
// caller, who reaches SYS_RFORK through a Thylacine ABI whose only shape is the
// vfork one. It does not hold here. A stock Linux binary that sets CLONE_VM and
// clears CLONE_VFORK has said, in the only vocabulary it has, "do not suspend
// me" -- and serving it with a suspend converts a working program into a
// DEADLOCK whenever the child neither execs nor exits promptly (a worker thread
// signalling through shared memory is the ordinary case). That is not
// conservative; it is a hang with our name on it.
//
// So the domain is exact, and the caller gets an honest decline it can act on.
// The genuinely concurrent shape has a correct target already -- CLONE_THREAD
// onto SYS_THREAD_SPAWN -- and that row should arrive with its own reasoning.
//
// A ZERO `stack` DECLINES, and that is what keeps `vfork()` proper out of scope
// (LINEAGE.md §9's fourth question, second half). Linux reads stack==0 under
// CLONE_VM as "share the parent's stack", which is safe there ONLY because
// CLONE_VFORK suspends the parent so the two never push concurrently. SYS_RFORK
// refuses a zero child_sp by contract (syscall.h), and weakening a landed
// kernel gate to widen a phenotype row would be the wrong direction of change.
// Declining one line above the kernel keeps the reason visible.
//
// Returns VIV_TRANSLATED or VIV_FORWARD. Never ENOSYS: `clone` exists, and the
// shapes outside the domain are ones a later chunk may serve rather than ones
// to deny forever.
//
// On VIV_TRANSLATED, *share_mem_out says WHICH of the two shapes was admitted:
// true = the vfork shape (the child shares the address space and the parent
// suspends -> RFMEM), false = the fork shape (the child gets a private
// copy-on-write copy). The shell turns that into a flag word; the pure layer
// says share-or-copy without importing proc.h's RFPROC/RFMEM, which do not
// belong on this side of the boundary.
//
// It is an OUT-PARAM rather than something the shell re-derives from `flags`
// (the vivarium_openat_decide shape) because re-deriving would put the same
// decision in two places, and the two shapes differ in exactly the way a second
// reader would get wrong.
enum viv_verdict vivarium_clone_decide(u64 flags, u64 stack,
                                       bool *share_mem_out);

// -----------------------------------------------------------------------------
// TIER 2 — `wait4` (LINEAGE L-6b). See docs/LINEAGE.md §5.5.
//
// The row that lets a Linux guest REAP what L-6a let it create. Its target is
// `wait_pid_for` (PTY-1e), which is already a POSIX `waitpid`: it has the pid /
// pgrp selectors, the non-blocking flag, and the stop/continue reports. So this
// is a MAP, not machinery — and the map is the whole point, because the option
// words look interchangeable and are not.
//
// THE COLLISION. Measured from `third_party/musl/include/sys/wait.h`:
//
//     Linux  WNOHANG 1   WUNTRACED 2   WEXITED 4   WCONTINUED 8
//     Thyla  WNOHANG 1   UNTRACED  2   CONTINUED 4
//
// The first two are the identity BY COINCIDENCE. The third is not, and the gap
// is occupied: Linux's `WEXITED` is 4, which is Thylacine's `WAIT_CONTINUED`.
// So a passthrough would be silently wrong in BOTH directions at once — a guest
// asking for WCONTINUED (8) sets a bit the native handler rejects as unknown,
// and a guest passing WEXITED (4) silently opts into continue-reports AND into
// the packed status encoding. Neither is a decline; both are answers that look
// plausible. That is why this row exists as a translator rather than a T1
// renumber, and why the map below is written out bit by bit.
//
// (WEXITED/WSTOPPED/WNOWAIT belong to `waitid`, not `wait4`. That makes the
// collision unlikely to fire from a correct program — but "unlikely" is not the
// standard this tier holds itself to, and the bit is REAL: musl defines it in
// the same header a guest includes, so a confused caller reaches it.)
//
// THE STATUS ENCODING IS ALREADY LINUX'S, which is the happy half. PTY-1e built
// `WAIT_STATUS_*` as "the Linux wait(2) layout so the Pouch boundary-line maps
// 1:1" (proc.h), and it checks out against musl's accessors:
//
//     WAIT_STATUS_EXITED(c) = (c & 0xff) << 8   <->  WEXITSTATUS(s) = (s & 0xff00) >> 8
//     WAIT_STATUS_STOPPED   = 0x7f | (20 << 8)  <->  WIFSTOPPED(0x147f) is true,
//                                                    WSTOPSIG -> 20 (SIGTSTP)
//     WAIT_STATUS_CONTINUED = 0xffff            <->  WIFCONTINUED(s) = (s == 0xffff)
//
// So no reshape is needed — EXCEPT that the kernel applies that encoding
// CONDITIONALLY. `wait_pid_for` packs only when the caller passed a PTY-1e flag,
// and returns the RAW exit status otherwise, "full compatibility for every
// pre-PTY caller" (proc.h). Linux always wants packed. So the translator packs
// exactly when the kernel did not — and it cannot decide that by inspecting the
// value, because a raw exit status of 5247 and a packed WAIT_STATUS_STOPPED are
// both 0x147f. It has to know what it ASKED for.
//
// WHY THIS DECIDE HANDS BACK A DESCRIPTION RATHER THAN A FLAG WORD. The obvious
// shape would be to return the native `WAIT_*` word directly. That would drag
// proc.h into this file, which the clone row already refused for RFPROC/RFMEM:
// the pure layer says WHAT WAS ASKED, and the shell — the one place that sees
// both ABIs — turns it into the native vocabulary.
//
// The split lands the risk in the right half. The dangerous direction is Linux
// bit 4 silently becoming WAIT_CONTINUED, and that is decided HERE, by an
// allow-list a unit test pins with no kernel plumbing at all. What is left for
// the shell is `.continued -> WAIT_CONTINUED`, a one-line assignment sitting
// directly above the `kernel_packs` derivation it must agree with, which is a
// tighter coupling than an out-param arriving from another file.
// -----------------------------------------------------------------------------

// Linux `wait4` option bits — musl `include/sys/wait.h`, read from the tree.
// WEXITED is listed precisely BECAUSE it is the collision; the four Linux-only
// bits are listed so that "declines" is a statement about named values rather
// than about whatever fell through.
enum {
    VIV_WNOHANG     = 1,
    VIV_WUNTRACED   = 2,          // == WSTOPPED
    VIV_WEXITED     = 4,          // waitid's; collides with WAIT_CONTINUED
    VIV_WCONTINUED  = 8,
    VIV_WNOWAIT     = 0x1000000,  // waitid's
    VIV_WNOTHREAD   = 0x20000000, // __WNOTHREAD
    VIV_WALL        = 0x40000000, // __WALL
    VIV_WCLONE      = 0x80000000, // __WCLONE
};

// The admitted option bits — an ALLOW-LIST, as VIV_MMAP_FLAGS_ADMITTED is, and
// for the same reason. Unlike the mmap and clone words this is a MASK rather
// than an exact value, because options genuinely compose: WNOHANG|WUNTRACED is
// an ordinary shell wait, whereas MAP_PRIVATE|MAP_ANON was the only mmap shape
// musl emits. A mask is the right shape when every subset is meaningful.
//
// __WALL / __WCLONE / __WNOTHREAD are excluded as a DOMAIN matter, not an
// oversight: all three discriminate thread-children from process-children, a
// distinction Thylacine's process table does not draw (a Thread is not a child).
// There is nothing to approximate them with.
#define VIV_WAIT_OPTS_ADMITTED \
    ((u32)(VIV_WNOHANG | VIV_WUNTRACED | VIV_WCONTINUED))

// Decide whether a `wait4` is inside the translatable domain, and compute the
// native flag word. PURE — no user memory, no Proc, no locks.
//
// `pid` is NOT judged here and is passed straight through, because
// `wait_pid_for`'s selectors ARE Linux's: -1 any, >0 that child, 0 the caller's
// group, <-1 the group -pid (proc.h). That correspondence is exact, so
// inspecting it would add a second opinion where there is only one rule.
//
// `rusage` must be 0. Filling it would mean inventing resource figures we do
// not collect per-child, and zeroing it would be a stored lie (a guest reading
// ru_utime would see a child that used no CPU). musl's `waitpid` and `wait`
// pass a literal 0 (`src/process/waitpid.c`), so the shell path and every
// ordinary reap are unaffected; only a deliberate `wait4(..., &ru)` declines.
// The prowl arc's per-Proc `run_ns` is the substrate a future row would use.
//
// On VIV_TRANSLATED, *out describes the request in Linux's own terms; the shell
// composes the native flag word from it.
//
// Returns VIV_TRANSLATED or VIV_FORWARD. Never ENOSYS: `wait4` exists.
struct viv_wait_opts {
    bool nohang;      // WNOHANG
    bool untraced;    // WUNTRACED / WSTOPPED
    bool continued;   // WCONTINUED  -- Linux bit 8, NOT Thylacine's bit 4
};

enum viv_verdict vivarium_wait4_decide(u64 options, u64 rusage,
                                       struct viv_wait_opts *out);

// -----------------------------------------------------------------------------
// writev (#150). The row the L-6c gate was actually blocked on: busybox's `echo`
// writes through writev, so with no translator the shell ran perfectly and
// printed nothing.
//
// IT MUST BE TIER 2, and this is the sharpest case yet of the ARITY RULE
// (section 4 / the ARCH section 25.4 row). A renumber onto SYS_WRITE lines up
// register for register -- writev(fd, iov, iovcnt) vs SYS_WRITE(fd, buf, len),
// three arguments each -- and would be catastrophically wrong: arg 1 is a
// POINTER TO AN ARRAY OF POINTERS, not a buffer, and arg 2 is an ENTRY COUNT,
// not a byte length. The kernel would write `iovcnt` bytes of the iovec array
// itself -- the guest's own pointers -- to the fd. The rule exists for exactly
// this: registers lining up is not arguments meaning the same thing.
#define VIV_UIO_MAXIOV 1024

// Linux `struct iovec` on aarch64: two 64-bit words, and the LP64 layout is
// fixed ABI. Named so the shell reads user memory in the shape it actually has
// rather than by open-coded offsets.
struct viv_linux_iovec {
    u64 base;
    u64 len;
};

_Static_assert(sizeof(struct viv_linux_iovec) == 16,
               "Linux aarch64 struct iovec is two 64-bit words");

// Bound the entry count. Split out as a pure decide because it carries the one
// real judgement in the row -- everything else writev does is uaccess and a loop
// over the existing byte-I/O core. Linux answers EINVAL for a count over
// UIO_MAXIOV and for a negative one (the argument is an `int`, so a huge u64
// with bit 31 set is negative on the Linux side; comparing as u64 catches both).
//
// Returns VIV_TRANSLATED with *out_count set, or VIV_FORWARD for out-of-domain.
// A count of 0 is IN domain and legal: Linux still validates the fd, so the
// shell issues a zero-length write rather than short-circuiting to 0.
enum viv_verdict vivarium_writev_decide(u64 iovcnt, u32 *out_count);

// Accumulate a total byte count with Linux's overflow rule. Linux rejects a
// writev whose lengths sum past SSIZE_MAX (it would make the return value
// indistinguishable from an error), and it checks this BEFORE writing anything.
// Returns false when the addition would break that bound.
bool vivarium_writev_accumulate(u64 *total, u64 add);

// -----------------------------------------------------------------------------
// uname (#150). A fabrication -- there is no underlying Thylacine call -- so the
// question is not HOW to translate but WHAT to claim, and a wrong answer here is
// the mistranslation the argument-domain rule exists to prevent: a guest that
// believes it is on a kernel it is not will take a code path we cannot serve.
//
// THE DECISION, field by field:
//
//   sysname    "Linux". The truthful answer WITHIN the phenotype -- the ABI the
//              guest sees IS Linux's, which is what a vivarium means. Claiming
//              "Thylacine" would send every `uname -s` check down an unknown-OS
//              path with no Thylacine support behind it, which is strictly worse
//              than the path it has.
//
//   release    "4.4.0", and this is the field the task flagged. No number is
//              honest: there is no Linux whose syscall surface matches ours,
//              because ours is a small subset of every version's. So the choice
//              is which direction to be wrong in, and LOW is safer -- a guest
//              that assumes little uses the oldest code paths, which are exactly
//              the ones translated here. 4.4 is picked as THE NEWEST KERNEL THAT
//              PROMISES NOTHING WE LACK: it predates statx (4.11, which we
//              FORWARD), io_uring (5.1), clone3 (5.3), openat2 (5.6),
//              faccessat2 (5.8) and close_range (5.9, also FORWARD) -- every
//              modern number this table declines. It also clears glibc's
//              minimum (3.2), below which a glibc binary aborts outright with
//              "FATAL: kernel too old" before main().
//
//   version    Carries "Thylacine" ON PURPOSE. Programs essentially never parse
//              this field -- it is the free-form build banner -- so it is where
//              `uname -a` can tell the truth without any version check tripping
//              over it. That split is the section 9 DEGRADED tier applied inside
//              a single struct: be compatible where a field is load-bearing, be
//              truthful where it is observable.
//
//   machine    "aarch64". Simply true.
//   nodename   "thylacine". There is no hostname concept to read.
//   domainname "(none)". Linux's own default.
//
// Linux's `struct new_utsname`: six fixed 65-byte NUL-terminated fields
// (__NEW_UTS_LEN 64, plus the terminator). 390 bytes, no padding, no alignment
// beyond a byte -- so the guest's `struct utsname` and this one are the same
// object.
#define VIV_UTS_FIELD_LEN 65

struct viv_linux_utsname {
    char sysname[VIV_UTS_FIELD_LEN];
    char nodename[VIV_UTS_FIELD_LEN];
    char release[VIV_UTS_FIELD_LEN];
    char version[VIV_UTS_FIELD_LEN];
    char machine[VIV_UTS_FIELD_LEN];
    char domainname[VIV_UTS_FIELD_LEN];
};

_Static_assert(sizeof(struct viv_linux_utsname) == 390,
               "Linux struct new_utsname is six 65-byte fields, densely packed");

// Fill a kernel-side utsname. Pure -- the shell copies it out. Zeroes the whole
// struct first, so every byte past each string is a defined 0 rather than
// whatever the caller's stack held (I-13: this struct is copied to EL0).
void vivarium_uname_fill(struct viv_linux_utsname *out);

// -----------------------------------------------------------------------------
// The identity reads (#150). getpid/getuid/getgid have exact native twins with
// matching arity and non-negative returns, so they are T1 rows -- with ONE
// translation that cannot live in a renumber, which is why getuid/getgid are
// shells instead:
//
// THE SENTINEL MAPPING. Thylacine's TCB identity is PRINCIPAL_SYSTEM ==
// 0xFFFFFFFE, and the container's shell runs as exactly that. Passed through
// raw, a Linux guest reads `(uid_t)-2` -- which in Linux practice is the
// historic "nobody"/nfsnobody value, i.e. the number that means the LEAST
// privileged identity. So the raw pass-through is not neutral: it inverts the
// fact being asked about, telling a Proc that holds the system identity that it
// is nobody. Mapping it to 0 (Unix root, the identity PRINCIPAL_SYSTEM
// corresponds to) is the faithful answer.
//
// SAFE BY CONSTRUCTION, not by luck: PRINCIPAL_INVALID and GID_INVALID are both
// 0, so 0 is not assignable to any real principal or group -- the mapping cannot
// collide with a genuine identity, and every other value passes through
// unchanged.
//
// CONFERS NOTHING. Every authority decision in Thylacine reads the real
// `principal_id` through perm_check or a CAP_* gate; the number a guest sees is
// informational to the guest alone. A container shell that believes it is root
// will ATTEMPT privileged operations and be refused at the real gates exactly as
// before -- the mapping changes what it is told, never what it may do (I-22).
u32 vivarium_map_uid(u32 principal_id);
u32 vivarium_map_gid(u32 gid);

// -----------------------------------------------------------------------------
// setuid / setgid (#150). Thylacine identity is set ONCE at spawn (via
// CAP_SET_IDENTITY) and is immutable on a running Proc, so there is nothing to
// translate a real change to. The disposition is therefore an errno choice, and
// the two candidates say different things:
//
//   ENOSYS -- "this system has no such concept". FALSE. Thylacine has a full
//             identity model; it just is not mutable in place.
//   EPERM  -- "you may not do this". TRUE, and it is also what Linux itself
//             answers an unprivileged process, so a guest's existing
//             drop-privilege fallback runs unchanged.
//
// With one exception that must not be collapsed into the refusal: setuid(getuid())
// SUCCEEDS on Linux -- it is the idempotent no-op every "drop to my own uid"
// path performs -- and refusing it would break callers that are asking for
// nothing. So the identity-preserving call succeeds and every other one is EPERM.
// The comparison is made in the GUEST's number space (after vivarium_map_uid),
// because that is the only value the guest has ever been shown.
//
// Returns true when the call is the no-op (the shell answers 0).
bool vivarium_setid_is_noop(u32 requested, u32 current_mapped);

#endif // THYLACINE_VIVARIUM_H
