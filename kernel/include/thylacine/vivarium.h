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

#endif // THYLACINE_VIVARIUM_H
