// VIVARIUM V-2 — the Linux syscall translation table (docs/VIVARIUM.md §4/§5.3).
//
// The contract, the admission rule, and why this file is PURE are all in
// <thylacine/vivarium.h>. Read that first; this file is the data plus a lookup.

#include <thylacine/vivarium.h>

#include <thylacine/syscall.h>
#include <thylacine/types.h>

// A T1 row: a Linux number, the Thylacine number it renumbers to, and the arity
// that must carry across unchanged. `nargs` is not used to copy (the whole
// six-word vector is copied verbatim — a Linux caller may leave the unused words
// as garbage and the native handler ignores them exactly as it does for a native
// caller). It is recorded because it documents WHICH words the equivalence claim
// actually covers, and the tests assert on it.
struct viv_row {
    u16 linux_nr;
    u16 thyla_nr;
    u8  nargs;
};

// -----------------------------------------------------------------------------
// TIER 1 — pure renumbers.
//
// Every row here satisfies the §4 rule in its strictest form: the argument words
// are identical in order, width, and MEANING, so the translation is the number
// substitution alone. Each row's equivalence was checked against the Thylacine
// side's documented contract in syscall.h, not against argument shape.
//
// The claims, one per row, so a future reader can re-check them without
// re-deriving:
//
//   read(fd, buf, count)        == SYS_READ(fd, buf_va, len)      syscall.h:105
//   write(fd, buf, count)       == SYS_WRITE(fd, buf_va, len)     syscall.h:106
//   close(fd)                   == SYS_CLOSE(fd)                  syscall.h:118
//   exit_group(status)          == SYS_EXIT_GROUP(status)         syscall.h:1276
//   lseek(fd, offset, whence)   == SYS_LSEEK(fd, offset, whence)  syscall.h:978
//
// `lseek` is the only row whose equivalence is not self-evident from the
// signature, because it carries an enumerated argument. It qualifies because
// T_SEEK_SET/CUR/END are 0/1/2 (syscall.h:955) and Linux's SEEK_SET/CUR/END are
// also 0/1/2 — the enumerations coincide, so there is no mapping to apply. Were
// they ever to diverge, this row would drop to T2 (a flag-bit mapping), not stay
// here. Both sides also reject an out-of-range whence, so the error semantics
// agree too.
//
// The negative space is as load-bearing as the rows, and is asserted by the
// tests so it cannot rot silently:
//
//   openat  — NOT a renumber, but a TIER-2 row (built at V-2b, below). Linux
//             passes a NUL-terminated path; SYS_OPEN wants an explicit path_len
//             (syscall.h:1340), so translating means SCANNING user memory for
//             the terminator -- which is why it cannot live in this pure table.
//             CORRECTION to the V-2a note that stood here: it said openat was
//             "total and stateless still". That is not right. V-2b found the
//             translation is total only over a STATED ARGUMENT DOMAIN --
//             O_CREAT and O_DIRECTORY have no SYS_OPEN counterpart at all -- so
//             the T2 translator checks each call's flags and forwards the rest.
//             See <vivarium.h> "THE ARGUMENT DOMAIN".
//   fstat   — NOT a renumber; also a TIER-2 row. `struct t_stat` is 88 bytes
//             (syscall.h:2143); Linux aarch64 `struct stat` is 128 with a
//             different field order. The translation is a struct conversion.
//             This one IS total: every t_stat maps, with no argument domain.
//   mmap    — FORWARD. addr hints, PROT_*, MAP_FIXED/ANONYMOUS/PRIVATE and
//             fd-backed mappings are POLICY. SYS_BURROW_ATTACH_LAZY takes a
//             length and nothing else. This is the "needs judgement" case the
//             rule exists to exclude.
//   munmap  — FORWARD, and this is the instructive one. The arguments line up
//             perfectly — munmap(addr, len) vs SYS_BURROW_DETACH(vaddr, length)
//             — and it LOOKS like a free row. It is not: burrow_detach requires
//             an exact VMA match and explicitly refuses a partial detach
//             (syscall.h:611-620), while Linux permits partial and
//             multi-mapping unmaps. The renumber would be silently WRONG for a
//             legal class of inputs, which is exactly what "total" forbids.
//   brk     — ENOSYS, honestly. Thylacine's heap is Burrow-based; there is no
//             break pointer, so there is nothing to translate to. Reporting
//             ENOSYS lets a libc fall back to its mmap path, which is what musl
//             and glibc both do; faking success would strand the allocator.
// -----------------------------------------------------------------------------

static const struct viv_row g_viv_t1[] = {
    { VIV_LINUX_READ,       SYS_READ,       3 },
    { VIV_LINUX_WRITE,      SYS_WRITE,      3 },
    { VIV_LINUX_CLOSE,      SYS_CLOSE,      1 },
    { VIV_LINUX_LSEEK,      SYS_LSEEK,      3 },
    { VIV_LINUX_EXIT_GROUP, SYS_EXIT_GROUP, 1 },
};

#define VIV_T1_COUNT ((u32)(sizeof(g_viv_t1) / sizeof(g_viv_t1[0])))

// The calls we have DECIDED are not table rows. Keeping them as an explicit list
// rather than letting them fall through to a default matters: a Linux number that
// we have never considered and one we have considered and rejected are different
// facts, and collapsing them would lose the analysis. It also makes the V-2b work
// list mechanical — the T2 entries are exactly the FORWARD rows a translator
// later promotes.
struct viv_reject {
    u16              linux_nr;
    enum viv_verdict verdict;
};

static const struct viv_reject g_viv_rejects[] = {
    { VIV_LINUX_OPENAT, VIV_TIER2   },  // V-2b: vivarium_openat_decide/_build
    { VIV_LINUX_FSTAT,  VIV_TIER2   },  // V-2b: vivarium_stat_to_linux
    { VIV_LINUX_MMAP,   VIV_FORWARD },  // policy: addr/prot/flags/fd-backing
    { VIV_LINUX_MUNMAP, VIV_FORWARD },  // NOT total: burrow_detach is exact-match
    { VIV_LINUX_BRK,    VIV_ENOSYS  },  // no counterpart; libc falls back to mmap
};

#define VIV_REJECT_COUNT ((u32)(sizeof(g_viv_rejects) / sizeof(g_viv_rejects[0])))

enum viv_verdict vivarium_translate(u64 linux_nr, const u64 *args_in,
                                    struct viv_call *out) {
    // Fail closed. A caller that ignores the verdict must never be handed a
    // dispatchable number, so a bad call site degrades to ENOSYS rather than to
    // an arbitrary syscall.
    if (!args_in || !out) return VIV_ENOSYS;

    // A number outside the u16 band cannot be in either table; reject before the
    // narrowing comparison so a 64-bit value cannot alias a real row.
    if (linux_nr > 0xFFFFu) return VIV_FORWARD;

    for (u32 i = 0; i < VIV_T1_COUNT; i++) {
        if (g_viv_t1[i].linux_nr != (u16)linux_nr) continue;

        out->nr = (u64)g_viv_t1[i].thyla_nr;
        for (u32 a = 0; a < VIV_NARGS; a++) out->args[a] = args_in[a];
        return VIV_TRANSLATED;
    }

    for (u32 i = 0; i < VIV_REJECT_COUNT; i++)
        if (g_viv_rejects[i].linux_nr == (u16)linux_nr)
            return g_viv_rejects[i].verdict;

    // Not yet classified. FORWARD is the right default rather than ENOSYS: the
    // supervisor is where unclassified calls belong under Option C, and claiming
    // "this does not exist" about a call we simply have not reached would be a
    // lie the guest cannot distinguish from a real one.
    return VIV_FORWARD;
}

// =============================================================================
// TIER 2 — the translators (V-2b).
// =============================================================================

// The Plan 9 open modes SYS_OPEN's `omode` argument carries. The KERNEL declares
// no constants for these (only the `SYS_WALK_OPEN_OMODE_VALID` mask at
// syscall.h:2376 and the prose at :2363); userspace spells them T_OREAD.. in
// libt's syscall.h:232. Named here so the flag map below reads as a mapping
// between two vocabularies rather than a shuffle of magic numbers.
enum {
    VIV_OMODE_READ   = 0u,
    VIV_OMODE_WRITE  = 1u,
    VIV_OMODE_RDWR   = 2u,
    VIV_OMODE_TRUNC  = 0x10u,   // == T_OTRUNC
};

// The `openat` flags whose effect SYS_OPEN can honour EXACTLY. Every bit outside
// this set forwards. Each admission is a claim about behaviour, so each is
// justified individually -- "we ignore it and nothing seems to break" is not a
// justification, it is the bug.
//
//   O_RDONLY/O_WRONLY/O_RDWR  the access mode; maps onto OREAD/OWRITE/ORDWR.
//   O_TRUNC                   maps onto OTRUNC.
//   O_PATH                    maps onto SYS_WALK_OPEN_OPATH -- the same "walk to
//                             it, do not open it" object on both sides.
//
//   O_CLOEXEC   ACCEPTED AS A NO-OP, and this is a claim worth stating. The flag
//               requests "this fd must not survive exec". Thylacine has no
//               close-on-exec concept because it has nothing to opt out of: a
//               spawned child "inherits no Spoor handles" (syscall.h:327) and
//               SYS_SPAWN_WITH_FDS passes an EXPLICIT list. So the behaviour
//               O_CLOEXEC asks for is what we do unconditionally, for every fd.
//               (The converse -- a guest opening WITHOUT O_CLOEXEC and expecting
//               the fd to cross exec -- is NOT served, but that is a property of
//               the process model, identical whether or not we admit this bit.
//               It is a V-7 seam, not a flag-map question.)
//   O_NOCTTY    Same shape. It requests "do not make this my controlling
//               terminal"; Thylacine acquires a ct only through the explicit
//               SYS_TTY_ACQUIRE (PTY-1), never implicitly on open. Already
//               relied on: the pouch pty patch opens /dev/pts/ptmx O_RDWR|O_NOCTTY.
//   O_LARGEFILE Requests ">2 GiB offsets permitted". Every Thylacine offset is
//               64-bit, so this is unconditionally true -- exactly as it is on
//               64-bit Linux, whose kernel force-sets it internally.
//
// The three admissions above share one structure: the flag requests behaviour we
// ALREADY provide unconditionally, so honouring it is a no-op AND correct. That
// is the only reason a flag may be ignored. Contrast the rejects below.
#define VIV_OPENAT_ADMITTED                                                  \
    ((u32)(VIV_O_ACCMODE | VIV_O_TRUNC | VIV_O_PATH |                        \
           VIV_O_CLOEXEC | VIV_O_NOCTTY | VIV_O_LARGEFILE))

// Why each notable rejected flag is rejected -- i.e. why ignoring it would be a
// SILENT WRONG ANSWER rather than a no-op:
//
//   O_CREAT      SYS_OPEN cannot create. Creation is SYS_WALK_CREATE, a different
//                syscall taking a `perm`. Ignoring the bit turns "create it if
//                absent" into ENOENT. (V-2c could route it; that is a second
//                target, not a flag map -- and task #50 already tracks the
//                userspace half.)
//   O_DIRECTORY  Requires the target BE a directory (Linux: ENOTDIR otherwise).
//                SYS_OPEN has no such check, so ignoring it turns an error into
//                a successful open of a regular file. The worst kind of wrong.
//   O_APPEND     Every write must seek to end. SYS_OPEN's omode mask has no
//                append bit; ignoring it silently corrupts a log writer.
//   O_NOFOLLOW   Thylacine's resolver has no symlinks at v1.0, so ignoring it is
//                harmless TODAY -- and would become wrong the moment symlinks
//                land, with nothing to catch it. Rejected on that basis: a flag
//                whose correctness depends on a feature being absent is a trap.
//   O_EXCL, O_NONBLOCK, O_SYNC/O_DSYNC, O_DIRECT, O_NOATIME, O_TMPFILE, O_ASYNC
//                each carry semantics with no SYS_OPEN counterpart.

enum viv_verdict vivarium_openat_decide(u64 dirfd, u64 flags,
                                        u64 *start_fd_out, u32 *omode_out) {
    // Fail toward the supervisor, never toward a dispatch (cf. vivarium_translate).
    if (!start_fd_out || !omode_out) return VIV_FORWARD;

    // Linux passes `dirfd` and `flags` as `int`, so ONLY the low 32 bits are
    // significant. This matters concretely for AT_FDCWD: a caller may leave x0
    // sign-extended (0xFFFFFFFFFFFFFF9C) or merely zero-extended (0xFFFFFF9C),
    // and both mean -100. Comparing the raw u64 would recognise one and forward
    // the other -- i.e. work on some toolchains and not others.
    s32 dfd = (s32)(u32)dirfd;
    u32 fl  = (u32)flags;

    // AT_FDCWD only, at V-2b.
    //
    // A REAL dirfd is excluded not because the fd would not carry across -- a
    // phenotyped Proc's fds ARE Thylacine handles, so the number passes through
    // unchanged -- but because Linux IGNORES the dirfd when the path is
    // ABSOLUTE, and deciding that requires reading the path's first byte out of
    // user memory. That would make this function impure for a case that
    // `open()` never generates (musl compiles every open() to AT_FDCWD; only the
    // *at() family passes a real fd). Purity is worth more than the *at()
    // family here; V-2c can revisit it with the path already measured.
    if (dfd != VIV_AT_FDCWD) return VIV_FORWARD;

    if (fl & ~VIV_OPENAT_ADMITTED) return VIV_FORWARD;

    u32 omode;
    if (fl & VIV_O_PATH) {
        // O_PATH dominates on BOTH sides: Linux ignores the access mode (and
        // O_TRUNC) for an O_PATH open, and SYS_WALK_OPEN_OPATH "ignores the
        // access bits" (syscall.h:2373). Emitting the bare OPATH -- rather than
        // OR-ing in whatever else was set -- keeps the agreement exact instead
        // of resting on how Thylacine happens to treat OPATH|OTRUNC.
        omode = SYS_WALK_OPEN_OPATH;
    } else {
        switch (fl & VIV_O_ACCMODE) {
        case VIV_O_RDONLY: omode = VIV_OMODE_READ;  break;
        case VIV_O_WRONLY: omode = VIV_OMODE_WRITE; break;
        case VIV_O_RDWR:   omode = VIV_OMODE_RDWR;  break;
        default:
            // (flags & O_ACCMODE) == 3 is EINVAL on Linux. Forwarding rather
            // than inventing the error keeps error semantics in one place --
            // §4 forbids the table from minting new ones.
            return VIV_FORWARD;
        }
        if (fl & VIV_O_TRUNC) omode |= VIV_OMODE_TRUNC;
    }

    // Belt-and-braces: never emit an omode SYS_OPEN would reject. A future
    // admission that forgets to add its bit to the kernel's mask fails HERE, as
    // a forward, rather than downstream as an unexplained -1.
    if (omode & ~SYS_WALK_OPEN_OMODE_VALID) return VIV_FORWARD;

    // AT_FDCWD <-> SYS_WALK_OPEN_FROM_ROOT is an exact correspondence, not an
    // approximation: SYS_OPEN with the sentinel joins a RELATIVE path against
    // the per-Proc cwd (LS-4) and resolves an ABSOLUTE one from the Territory
    // root -- which is precisely what AT_FDCWD specifies. The absolute/relative
    // split therefore needs no inspection here; both sides make it identically.
    *start_fd_out = SYS_WALK_OPEN_FROM_ROOT;
    *omode_out    = omode;
    return VIV_TRANSLATED;
}

void vivarium_openat_build(u64 start_fd, u64 path_va, u32 path_len, u32 omode,
                           struct viv_call *out) {
    if (!out) return;

    for (u32 i = 0; i < VIV_NARGS; i++) out->args[i] = 0;

    out->nr      = (u64)SYS_OPEN;
    out->args[0] = start_fd;
    out->args[1] = path_va;
    out->args[2] = (u64)path_len;
    out->args[3] = (u64)omode;
}

void vivarium_stat_to_linux(const struct t_stat *in, struct viv_linux_stat *out) {
    if (!in || !out) return;

    // Zero FIRST, then fill. The kernel links no `memset`, so this is the tree's
    // byte-loop idiom (dev9p.c:701 `t_stat_from_p9_attr` does the same for the
    // same reason). Zeroing wholesale rather than assigning every field
    // individually is an I-13 obligation: this buffer is copied to a guest, so
    // any word left unwritten -- a reserved field today, a field added
    // tomorrow -- would ship a slice of the kernel stack.
    for (u64 i = 0; i < (u64)sizeof(*out); i++) ((u8 *)out)[i] = 0;

    // st_dev / st_ino: the (devno, qid.path) pair IS Thylacine's file identity
    // (#100), and it is already the pair userspace maps onto (st_dev, st_ino) --
    // pouch patch 0010 does exactly this, and gopls's robustio keys FileID on it.
    // So this is not a new correspondence invented here; it is the established
    // one, applied one layer lower.
    out->st_dev = (u64)in->devno;
    out->st_ino = in->qid_path;

    out->st_mode  = in->mode;
    out->st_nlink = in->nlink;
    out->st_uid   = in->uid;
    out->st_gid   = in->gid;

    out->st_size    = (s64)in->size;
    out->st_blksize = (s32)in->blksize;
    out->st_blocks  = (s64)in->blocks;

    // t_stat carries whole seconds only, so the nsec words stay 0 -- an honest
    // "unknown sub-second", the same answer the native surface gives. st_rdev
    // stays 0: Thylacine has no dev_t for its character objects, and fabricating
    // one would invite a guest to switch on it.
    out->st_atime_sec = (s64)in->atime_sec;
    out->st_mtime_sec = (s64)in->mtime_sec;
    out->st_ctime_sec = (s64)in->ctime_sec;
}
