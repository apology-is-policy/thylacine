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
    VIV_LINUX_STATX      = 291,
    VIV_LINUX_EXIT_GROUP = 94,
};

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

#endif // THYLACINE_VIVARIUM_H
